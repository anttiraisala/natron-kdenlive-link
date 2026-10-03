# natron-kdenlive-link: detailed reference

This is the detailed reference for the components, the Natron worker, the MLT filter, the protocol, the configuration and the tests. The overview, quick start and current status are in the [README](../README.md). Sections that describe how a milestone was verified are kept as development history; the README's status section is the current one.

## Components

| Component | File | Purpose |
|---|---|---|
| `natron-kdenlive-daemon` | `src/daemon.cpp` | Routes frames between filters and workers, owns the cache and the input queue |
| `natron-kdenlive-cache` | `tools/cache_tool.cpp` | `--status`, `--clear`, `--ping` against a running daemon |
| `natron-kdenlive-doctor` | `tools/doctor.cpp` | Installation checks, one `check=... result=...` line each |
| `nkb-mock-worker` | `tools/mock_worker.cpp` | Test stand-in for the Natron worker |
| `nkb-mock-filter` | `tools/mock_filter.cpp` | Test stand-in for the MLT filter, verifies every pixel |
| `libmltnatron.so` | `src/filter/natron_link.cpp` | MLT filter module: sends frames to the daemon, shows the result |
| `tools/install-filter.sh` | | Installs/uninstalls the module and effect XML in an extracted Kdenlive AppImage |
| `nkb-filter-check` | `tests/filter_check.c` | Drives the filter through the MLT API in tests |
| `nkb_base` library | `src/protocol.cpp net.cpp cache.cpp config.cpp client.cpp` | No spdlog dependency, so the MLT filter can link it |

## Build

```bash
sudo apt update
sudo apt install -y build-essential cmake ninja-build git pkg-config libspdlog-dev libfmt-dev
cd natron-kdenlive-link
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug     # use Release for performance measurements
cmake --build build
ctest --test-dir build --output-on-failure                 # unit tests + end-to-end tests (about 12 s)
```

To also build the MLT filter, point `MLT_ROOT` at the MLT 7.40.0 install (see the README for how to build it):
```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
      -DMLT_ROOT=$PWD/third-party/mlt-7.40.0
cmake --build build
ctest --test-dir build --output-on-failure       # now 3 tests: unit, e2e, filter_e2e
# add -DNATRON_COMMAND="snap run natron" (or the path of NatronRenderer) to also register the natron_e2e test
```
This produces `build/libmltnatron.so`.

## Run (manual test without Kdenlive or Natron)

Terminal 1:
```bash
./build/natron-kdenlive-daemon
```
The first start creates the data directory `~/NatronKdenliveLink/` with `config.ini`, `token` (mode 0600) and
`logs/natron-kdenlive.log`.

Terminal 2:
```bash
./build/nkb-mock-worker --mode invert --delay-ms 50
```
Terminal 3:
```bash
./build/nkb-mock-filter --frames 20 --width 1920 --height 1080     # renders, prints a summary line
./build/nkb-mock-filter --frames 20 --width 1920 --height 1080 --expect cache_hit=20   # all from cache
./build/natron-kdenlive-cache --status
./build/natron-kdenlive-cache --clear
./build/natron-kdenlive-doctor
```
Stop the daemon with Ctrl+C.

## The Natron worker (milestone 3)

`natron/nkb_natron_worker.py` runs inside Natron's own Python (3.10, standard library only) with no window:
`NatronRenderer -t natron/nkb_natron_worker.py` (tarball) or `snap run natron -t natron/nkb_natron_worker.py` (snap).
It connects to the daemon as a worker and serves render jobs. Natron's Python API has no call that hands pixels
over directly, so each frame travels as an image file (uncompressed TGA) in the exchange folder:
the worker writes the input frame, points the comp's `NKB_Input` Read node at it, renders the `NKB_Output` Write node
for the job's frame number, reads the result back and sends it to the daemon.

**Compositions.** `<NKB_HOME>/comps/<comp id>.ntp` (comp id = the filter's `comp` property). A comp must contain a Read node
named `NKB_Input` and a Write node named `NKB_Output`; the graph between them is yours (build and save it in the Natron GUI).
The first time a comp id is seen, a default pass-through graph is created and saved. The worker reloads a comp when its file
changes (about 60-120 ms), so saving in Natron takes effect on the next frame without restarting anything.
`tests/natron_e2e.sh` shows the whole flow.

**Colour and alpha.** `NKB_COLOR=srgb` (default): frames are treated as sRGB and the graph works in linear light, as Natron
normally does. `NKB_COLOR=raw`: no conversion, the graph sees the encoded values. Natron's data is premultiplied, so an
effect such as Invert or Grade acts on premultiplied RGB; for semi-transparent pixels use Unpremult, the effect, then Premult.
Measured pass-through error: at most 2 levels per RGB channel for alpha of 64 or more; alpha is exact. Very low alpha
loses colour precision (about 4 levels at alpha 40, worse below) because the pipeline is premultiplied.
Only RGBA8 frames are supported by the worker so far; other formats return an error status.

**Environment variables:** `NKB_HOME`, `NKB_COMPS_DIR`, `NKB_EXCHANGE_DIR` (must be readable and writable by Natron; for the snap use a
non-hidden folder in your real home), `NKB_COLOR`, `NKB_MAX_JOBS`, `NKB_KEEP_FILES`, `NKB_LOG_LEVEL`, `NKB_RECONNECT`, `NKB_WORKER_ADDRESS`, `NKB_EXIT_WITH_PID` (exit when that process is gone; the tests use it).
Stop it with Ctrl+C or by creating `<NKB_HOME>/worker.stop` (it logs `event=worker_stopping reason=...`). The stats of the cache tool include `worker_connections`, the number of workers that ever connected. It reconnects when the daemon restarts.

**Running the Natron test against the snap** (two separate commands; the test takes 10-30 s and the output is written to a file so a
long run never looks frozen):
```bash
mkdir -p ~/nkb-test
NKB_TEST_BASE=$HOME/nkb-test NKB_TEST_TCP=1 bash tests/natron_e2e.sh build snap run natron > /tmp/natron-snap-test.out 2>&1 &
tail -f /tmp/natron-snap-test.out        # Ctrl+C stops only tail; the test ends with ALL ... PASSED or N ... FAILED
```
To abort the test, stop the script, its daemon and its worker together: `pkill -9 -f "natron_e2e.sh|natron-kdenlive-daemon|nkb_natron_worker"` (this also stops a daemon and worker you started yourself).

### Spike findings (Natron 2.5.0, Ubuntu 24.04, headless)
| Question | Result |
|---|---|
| Can a `natron -t` script run a blocking socket loop and render on demand? | Yes. `app1.render(writeNode, frame, frame)` works inside the loop. |
| Cost per 1080p frame, Read -> Invert -> Write, new input file each frame | about 225-320 ms Natron render, 6-12 ms writing the input, 11-22 ms reading the result: roughly 245-280 ms in total, about 4 frames per second. A heavier graph will be slower. |
| Load a saved `.ntp` | `app1.loadProject(path)`: 55-120 ms. Nodes are found by name afterwards. |
| File format | TGA chosen. In a Read -> Write test without changing input, BMP matched TGA, while TIFF, PNG and EXR were 3-6 times slower. BMP was not tried with changing input. |
| Several comps at once | One project is loaded per worker process. Switching comps reloads (60-120 ms). Alternating between comps every frame would be slow; running one worker process per comp is future work. |
| Natron's interpreter at exit | Natron 2.5.0 segfaults while shutting down after a script ends. The worker therefore leaves with `os._exit` after logging. |

Behaviours of Natron 2.5.0 the worker has to work around (each has a test):
* The first render after `loadProject` returns wrong colours (up to 192 levels off); every later one is right. The worker renders a throw-away 16x16 frame
  after each load (about 20 ms). The cause is not understood.
* Natron re-derives the Read/Write colourspaces from the file type whenever a filename changes, and the "Set" flags do not stop this, so the
  worker sets the colourspaces after the filenames, for every job.
* `app1.render` does not raise when rendering fails; it prints a message. The worker checks that the output file exists and has the right size.
* The Write node's output size defaults to the project size; the worker sets it to follow the input.

Consequence for the design: at about 4 frames per second for a trivial 1080p graph, live playback of a Natron graph is not realistic
for most graphs. The cache-first design (first pass renders in the background, later passes play from the cache) is the right one.

## The MLT filter (milestone 2)

`natron_link` is an ordinary MLT filter, so Kdenlive can apply it to a clip, a track or the master. For every
frame it hashes the RGBA pixels together with the composition file, parameters and frame number into a content
key, probes the daemon cache (no pixels sent), and on a miss sends the frame and waits. Whatever happens (daemon
down, wrong token, slow render, worker error) the frame passes through unchanged and playback continues.

Properties (shown in Kdenlive's effect panel where marked):

| Property | Default | Meaning |
|---|---|---|
| `ntp` (panel) | empty | Natron project file. Its content hash is part of the key, so editing it invalidates old frames. The composition id defaults to its file name. When `ntp` and `comp` are both empty, the effect picks its own composition `comp-xxxxxx` on its first frame and sets `ntp` to `<data dir>/comps/comp-xxxxxx.ntp` (log event `comp_assigned`) |
| `mode` (panel) | `auto` | `auto`, `playback`, `export`. `auto` treats as export: a consumer with `real_time <= 0`, a consumer whose service name starts with `avformat`, or any non-display consumer in the `melt` process |
| `playback_timeout_ms` (panel) | 250 | Wait for a render during playback; on a timeout the frame passes through and the render finishes in the background |
| `export_timeout_ms` | 600000 | Wait during export. If no processed frame arrives, an `export_frame_unprocessed` error is logged and the frame is exported unprocessed |
| `comp`, `address`, `params` | empty | Composition id, daemon address override, extra string hashed into the key (used for parameters in a later milestone) |
| `open_natron` (panel) | 0 | Checkbox used as a button: every change of its value saves the frame the effect showed last as `<comp>_preview.tga` next to the `.ntp` and sends `open_natron <path>` to the daemon. The daemon starts `gui_command -c <text of natron/nkb_gui_open.py>` with `NKB_OPEN=<path>`; that script, running in the Natron GUI, loads the composition (or creates the pass-through graph), points `NKB_Input` at the preview and `NKB_Output` at `<comp>_output.tga` when it has no file name, and connects the viewer to `NKB_Output` (Natron has no option to run a script after loading a project given on its command line, so the script loads it). A composition already open in a GUI started this way is not opened again; its window is raised with `raise_command <file name>`. Changes in the first 1.5 s after the filter is created (creating the effect, loading a project) and changes in the `melt` process are ignored. Log events: `open_natron_clicked`, `preview_written`, `preview_skipped`, `open_natron_sent`, `open_natron_ignored`, `open_natron_failed` (filter); `natron_open_requested`, `natron_gui_started`, `natron_gui_exited`, `natron_open_skipped`, `natron_raised`, `natron_raise_failed`, `natron_open_failed` (daemon); `NKB_GUI ...` lines in `logs/natron-gui.log` (the script) |
| `nkb_auto_comp` | empty | Set by the filter: the effect's own `comp-xxxxxx`, used again whenever `ntp` is emptied. Saved with the project |

Environment: `NKB_FILTER_ADDRESS` overrides the daemon address, `NKB_LOG_LEVEL` the log level, `NKB_HOME` the data
directory. The filter appends to the same log file as the daemon with component `[filter]`.

### Try it with Kdenlive (AppImage), using the mock worker

```bash
# 1. install into the extracted AppImage (the squashfs-root folder), verifies by loading it in the AppImage's melt
tools/install-filter.sh install /path/to/squashfs-root
# 2. terminal 1 and 2
./build/natron-kdenlive-daemon
./build/nkb-mock-worker --mode invert --delay-ms 100
# 3. terminal 3: start Kdenlive from the extracted folder
/path/to/squashfs-root/AppRun
```
In Kdenlive add the effect "Natron Link" to a clip, play it, and the clip should show inverted colors (after the
first pass through each frame, because renders happen in the background during playback). Send back the log:
`tail -200 ~/NatronKdenliveLink/logs/natron-kdenlive.log`. Remove it again with
`tools/install-filter.sh uninstall /path/to/squashfs-root`.

### Milestone 2: verified and unverified
Verified here (Ubuntu 24.04, MLT 7.40.0 built from source, `tests/filter_e2e.sh`): the module loads through
`melt` and through the MLT API; frames are processed through the real daemon and mock worker; cache hits on the
second pass; export mode waits and playback mode passes through; `mode=auto` follows `real_time`; editing the `.ntp`
changes the keys; no daemon, wrong token and slow worker all pass frames through without hanging; the install and
uninstall script on an emulated AppImage layout.
Found on the third real Kdenlive run: a render through Kdenlive logged `mode=playback real_time=1` for every frame, so `mode=auto`
did not recognise the render and a slow Natron would have produced unprocessed frames silently. Export detection now also
uses the consumer service name and the process name (`melt`), and each `filter_frame` log line records `export_reason`,
`consumer`, `real_time` and `process`. Found on the second real Kdenlive run: with no worker connected every frame waited the full playback timeout (about 256 ms), so playback crawled.
The daemon now answers immediately while no worker is connected (`no_worker_wait_ms`). Also found on the first run: Kdenlive logged `Invalid metadata for natron_link` and did not list the effect, because the
module registered no MLT metadata. The module now provides it (`melt -query filter=natron_link` prints it, and the test checks that).
**Not verified at the time of milestone 2** (the first three questions were later answered on the author's machine, see the README): that a module built against MLT 7.40.0 loads into the AppImage's 7.41.0 (the
install script checks this and prints the melt output if it fails); that Kdenlive 26.08 accepts `natron_link.xml`
(the `url`, `list` and `constant` parameter types and the `<filter>` element were taken from other Kdenlive effect
files, not from the 26.08 schema); how Kdenlive's preview and render consumers set `real_time` (the log line shows
`mode=` and `real_time=` for every frame so this can be read off); behavior with the Kdenlive snap (its MLT directory
is read-only, so the module cannot be installed there). The module needs glibc 2.38 or newer (`__isoc23_*`
symbols); the AppImage runs on the host's glibc, which is 2.39 on Ubuntu 24.04.

## Install / uninstall

```bash
sudo cmake --install build         # daemon, cache tool, doctor -> /usr/local/bin
sudo cmake --build build --target uninstall
```
The data directory `~/NatronKdenliveLink/` (config, token, logs) is created by the daemon at run time and is not
touched by install or uninstall. Delete it by hand to reset everything: `rm -rf ~/NatronKdenliveLink`.

## How it works

Playback is **pull based**. The filter asks for frame N, identified by a 128 bit **content key** (hash of the
input identity, composition file, parameter values and frame number; for nested comps also the upstream key).
Because the key covers everything the output depends on, a cached frame can never be stale.

1. The filter sends a `FrameRequest` with `kCacheOnly` (no pixels). A hit returns the cached frame.
2. On a miss it sends the full request with a `timeout_ms`.
3. The daemon queues one job per key (identical concurrent requests share it) and waits up to the timeout.
4. A worker renders the job; the result goes into the cache and to the waiting requesters.
5. If the result is not ready in time, the reply carries no image (`timeout`, or `passthrough` for timeout 0).
   The filter then passes its own frame through. The render continues, and the next pull is a cache hit.

Statuses that carry an image: `ok`, `cache_hit`. Everything else means "use your own frame"; the payload is
a text reason for the logs.

### Input queue policy (`buffer_behavior`)
When the queue of frames waiting for Natron exceeds `input_queue_memory_mb`:
* `buffer_skip` (default) drops the oldest queued frame. Its waiters get status `skipped`.
* `pause` makes the requesting filter wait for free space, up to its timeout.
* `show_cached` rejects the new request (`rejected`); the filter passes its frame through.

A single frame larger than the whole queue limit is still accepted when the queue is empty.

### Wire protocol
Fixed 104 byte little-endian header plus payload. The header carries width, height, pixel format
(RGBA8 / RGBA16 / RGBAF32), alpha mode and colorspace on every frame, so other resolutions and formats need no
protocol change. Only RGBA8 frames are exercised end to end in milestone 1; RGBA16 and RGBAF32 are covered by the unit
tests for size computation only. The layout is documented in `include/nkb/protocol.h`.

### Transport and security
Each of the two ports (filters and tools on `filter_address`, workers on `worker_address`) accepts
`unix:/path` or `tcp:127.0.0.1:PORT`. Only loopback TCP is allowed. Loopback TCP is the default because
sandboxed apps (snap, flatpak) cannot see each other's socket files. Because any local process could connect
to a TCP port, every connection starts with a token handshake; the token is in `~/NatronKdenliveLink/token`.
Reading that file requires the app to be able to see your home directory (the snap `home` interface does).

## Configuration

File: `~/NatronKdenliveLink/config.ini` (or `$NKB_HOME/config.ini`). Written with comments on first daemon
start. Changes need a daemon restart. Whole-line `#` comments only. Invalid values stop the daemon with the key
name and the allowed range.

| Key | Default | Meaning |
|---|---|---|
| `[daemon] filter_address` | `tcp:127.0.0.1:47801` | Filters and tools connect here |
| `worker_address` | `tcp:127.0.0.1:47802` | The Natron worker connects here |
| `cache_memory_mb` | 1024 | Output cache target, clamped into the min/max below |
| `cache_memory_min_mb` / `cache_memory_max_mb` | 256 / 5120 | Clamp for the cache size |
| `input_queue_memory_mb` | 512 | Frames waiting for Natron |
| `buffer_behavior` | `buffer_skip` | `pause`, `buffer_skip`, `show_cached` |
| `natron_timeout_seconds` | 10 | A worker needing longer per frame is dropped |
| `default_request_timeout_ms` | 1000 | Wait time when the filter does not specify one |
| `no_worker_wait_ms` | 0 | While no worker is connected, a request waits at most this long; frames are still queued for when a worker connects |
| `max_frame_mb` | 1024 | Largest accepted frame |
| `stats_log_interval_seconds` | 5 | Periodic `event=stats` log line, 0 = off |
| `[natron] gui_command` | `snap run natron` | Starts the Natron GUI; `-c <text of nkb_gui_open.py>` is added (the text, not the path, so a snap needs no access to the file). Words split at spaces, no quoting |
| `raise_command` | `wmctrl -a` | Brings an already open Natron window to the front; the `.ntp` file name (part of Natron's window title) is added. Empty = off |
| `scripts_dir` | empty | Folder of `nkb_gui_open.py`. Empty: `<daemon dir>/../natron`, then the source tree the daemon was built from, then the installed `share/natron-kdenlive-link/natron` |
| `[logging] level` | `debug` | `trace`, `debug`, `info`, `warn`, `error` |
| `log_file` | empty | Empty means `~/NatronKdenliveLink/logs/natron-kdenlive.log` |
| `console` | `true` | Also log to the terminal |

Differences from earlier plan wording: `socket_kdenlive`/`socket_processor` became `filter_address`/`worker_address`
(they may also be TCP), `debug` became `level`, and parameter definitions will use a separate TOML/JSON file in a
later milestone, not the INI.

## Logs

One line per event, ISO-8601 UTC timestamp, level, component, thread id, then `event=<name> key=value ...`:
```
2026-10-02T10:15:23.456Z [debug] [daemon] [t=1234] event=job_done job=7 key=... frame=42 render_ms=292.4 queued_ms=3.1 ...
```
Every failure carries `reason="..."`. Useful events: `cache_hit`, `cache_miss`, `job_queued`, `job_sent`,
`job_done`, `job_failed`, `job_skipped`, `request_not_ready`, `handshake_failed`, `worker_disconnected`, and a
periodic `stats` line (queue depth, cache size, hit/miss counts). To diagnose a problem, give the log (and the
output of `natron-kdenlive-doctor`) to someone who can read it, or to an AI.

## Not yet implemented
Disk cache tier, a companion tool that creates/opens comps from the Kdenlive project (`comps/` is managed by hand for now), `.ntp` composition management, parameters and keyframes,
nested-comp wiring, the `natron-kdenlive-doctor` checks for Kdenlive/Natron install types. The Natron
project format is `.ntp` (not `.nk`, which is Nuke's).

## Test coverage
`tests/unit_tests.cpp`: keys (including upstream chaining), header geometry and overflow protection, cache LRU
and clearing, config parsing/validation/defaults, address rules, message framing.
`tests/filter_e2e.sh` (needs `MLT_ROOT`): see the verified list above.
`tests/natron_e2e.sh` (needs `NATRON_COMMAND`): the real worker inside Natron through the daemon: default comp creation, pass-through accuracy, Invert graph in raw and sRGB mode, reload on file change, broken comp, 1080p timing.
`tests/e2e.sh`: real daemon with the mock tools over Unix sockets and loopback TCP: render, cache hit, key change,
cache clear, bad token, no worker, slow worker, queue-only playback with `buffer_skip`, `show_cached`, `pause`, worker
error on one frame, stuck worker timeout, log format.
The `pause` test covers only the timeout path (queue full, no space freed); the path where a worker frees space during the wait is untested.
