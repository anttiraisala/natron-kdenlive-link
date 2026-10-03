# natron-kdenlive-link

Use Natron compositions as an effect in Kdenlive, in the style of Adobe Dynamic Link ( as used to link Adobe Premiere with After Effects ). Add the **Natron Link** effect to a clip, build the graph in Natron, and see the result in Kdenlive's preview and in rendered files.

> **Status: early development (version 0.3.2, milestone 4 of 6).** The whole chain works on the author's machine: Kdenlive (extracted AppImage) sends frames through the daemon to the real worker inside the Natron snap, and the processed frames come back in Kdenlive's preview and in rendered files. Parameters, nested compositions and other pixel formats are not implemented yet. See [Status and verification](#status-and-verification) for exactly what has and has not been tested.

## What it does

```
Kdenlive (AppImage)                      host                         Natron (snap or tarball)
 MLT filter "natron_link"  --frames-->  natron-kdenlive-daemon  --jobs-->  nkb_natron_worker.py
 (libmltnatron.so)         <--results-- content-hash frame cache <--frames-- renders your .ntp graph
```

* An **MLT filter** inside Kdenlive hashes each frame and asks a local **daemon** whether that frame has already been rendered.
* On a miss, the daemon sends the frame to a **worker** running inside Natron. The worker renders it through your composition (a Natron `.ntp` project) and returns the result.
* Results are cached under a content key (pixels, composition file, parameters, frame number), so a cached frame can never be out of date.
* **Playback stays responsive.** If Natron is slow or not running, the filter shows the original frame, and the render finishes in the background. The next pass over the same frame comes from the cache.
* **Exports wait** for processed frames. If a frame cannot be processed, the log says so (`export_frame_unprocessed`).
* Everything is logged as one structured line per event, so a log can be read by a person or an AI to find out what went wrong.

At about 4 frames per second for a trivial 1080p graph, Natron is the limit, not the bridge. Live playback of heavy graphs is not realistic, which is why the design is cache-first.

## Requirements

| Component | Tested with |
|---|---|
| OS | Ubuntu 24.04 (x86_64) |
| Compiler and tools | g++ 13.3, CMake 3.28, Ninja, `libspdlog-dev` 1.12, `libfmt-dev`, `libxml2-dev`, git |
| Kdenlive | AppImage 26.08.1, **extracted** (the filter is copied into it) |
| Natron | 2.5.0 snap (`snap run natron`), and the 2.5.0 tarball headless |
| MLT headers | MLT 7.40.0, built from source (the AppImage ships 7.41.0, and the filter built against 7.40.0 loads in it) |

**Not supported:** the Kdenlive snap. Its MLT folder is read-only, so the filter cannot be installed there.

## Quick start

### 1. Build and run the tests (no Kdenlive or Natron needed)

```bash
sudo apt update
sudo apt install -y build-essential cmake ninja-build git pkg-config libspdlog-dev libfmt-dev libxml2-dev
git clone https://github.com/anttiraisala/natron-kdenlive-link.git
cd natron-kdenlive-link
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

### 2. Build the MLT filter

The filter needs MLT 7.40.0 headers. These commands build a minimal MLT into `third-party/` (ignored by git):

```bash
git clone --branch v7.40.0 --depth 1 https://github.com/mltframework/mlt.git third-party/mlt-src
cmake -S third-party/mlt-src -B third-party/mlt-src/build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PWD/third-party/mlt-7.40.0" \
  -DBUILD_TESTING=OFF -DGPL=OFF -DGPL3=OFF \
  -DMOD_AVFORMAT=OFF -DMOD_DECKLINK=OFF -DMOD_FREI0R=OFF -DMOD_GDK=OFF -DMOD_GLAXNIMATE_QT6=OFF \
  -DMOD_JACKRACK=OFF -DMOD_KDENLIVE=OFF -DMOD_MOVIT=OFF -DMOD_NDI=OFF -DMOD_NORMALIZE=OFF \
  -DMOD_OLDFILM=OFF -DMOD_OPENCV=OFF -DMOD_OPENFX=OFF -DMOD_PLUS=OFF -DMOD_PLUSGPL=OFF -DMOD_QT6=OFF \
  -DMOD_RESAMPLE=OFF -DMOD_RTAUDIO=OFF -DMOD_RUBBERBAND=OFF -DMOD_RNNOISE=OFF -DMOD_SDL1=OFF \
  -DMOD_SDL2=OFF -DMOD_SOX=OFF -DMOD_SPATIALAUDIO=OFF -DMOD_VIDSTAB=OFF -DMOD_VORBIS=OFF -DMOD_XINE=OFF
cmake --build third-party/mlt-src/build
cmake --install third-party/mlt-src/build
```

Then rebuild this project with the filter enabled:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DMLT_ROOT="$PWD/third-party/mlt-7.40.0"
cmake --build build
ctest --test-dir build --output-on-failure        # now includes the filter tests
```

### 3. Install the filter into an extracted Kdenlive AppImage

```bash
chmod +x kdenlive-26.08.1-x86_64.AppImage
./kdenlive-26.08.1-x86_64.AppImage --appimage-extract          # creates ./squashfs-root
tools/install-filter.sh install /path/to/squashfs-root
```

The script copies `libmltnatron.so` and the effect description into the extracted tree and checks that the AppImage's own `melt` can load the filter. It must print `OK: the AppImage's MLT loaded libmltnatron.so`. Remove it again with `tools/install-filter.sh uninstall /path/to/squashfs-root`.

### 4. Try it with the test worker (inverts colors)

Run each command in its own terminal:

```bash
./build/natron-kdenlive-daemon
./build/nkb-mock-worker --mode invert --delay-ms 100
/path/to/squashfs-root/AppRun
```

In Kdenlive, search the Effects tab for **Natron Link** (listed under *Misc*), drag it onto a clip, and play. The clip should show inverted colors, after the first pass over each frame.

### 5. Use the real Natron worker

Stop the test worker, then start the Natron worker instead:

```bash
snap run natron -t natron/nkb_natron_worker.py        # Natron snap
NatronRenderer -t natron/nkb_natron_worker.py         # Natron tarball
./build/natron-kdenlive-cache --status | grep -E "^workers="     # should print workers=1
```

The first time a composition id is used, the worker creates a default pass-through graph `~/NatronKdenliveLink/comps/<comp id>.ntp` (Read node `NKB_Input` connected to Write node `NKB_Output`). Open that file in Natron, add nodes between the two, and save. The worker reloads the file when it changes.

The composition id is the filter's `comp` property, or the file name of its `ntp` property. The easiest way to pick a composition is the **Natron project (.ntp)** file field in the effect panel: choose a file in `~/NatronKdenliveLink/comps/`, and the worker uses it from the next frame on.

### Stopping things

```bash
pkill -f natron-kdenlive-daemon; pkill -f nkb-mock-worker; pkill -f nkb_natron_worker
```

`-f` is required: `pkill` without it compares only the first 15 characters of a process name and does not match `natron-kdenlive-daemon`. Add `-9` for a process that ignores the signal (Natron and `snap run` can).

## Configuration

The daemon writes a fully commented `~/NatronKdenliveLink/config.ini` on first start. Changes need a daemon restart.

| Key | Default | Purpose |
|---|---|---|
| `[daemon] filter_address` | `tcp:127.0.0.1:47801` | Where Kdenlive filters and tools connect (`tcp:127.0.0.1:PORT` or `unix:/path`) |
| `worker_address` | `tcp:127.0.0.1:47802` | Where the Natron worker connects |
| `cache_memory_mb` | 1024 | Output cache size, kept between `cache_memory_min_mb` (256) and `cache_memory_max_mb` (5120) |
| `input_queue_memory_mb` | 512 | Frames waiting for Natron |
| `buffer_behavior` | `buffer_skip` | When the queue is full: `buffer_skip`, `pause` or `show_cached` |
| `natron_timeout_seconds` | 10 | A worker needing longer for one frame is dropped |
| `no_worker_wait_ms` | 0 | How long a request waits while no worker is connected |
| `[logging] level` | `debug` | `trace`, `debug`, `info`, `warn`, `error` |

Only loopback addresses are accepted. Every connection starts with a token handshake; the token is in `~/NatronKdenliveLink/token` (mode 0600). Loopback TCP is the default because a snap cannot see another snap's Unix sockets.

Filter properties (set in Kdenlive's effect panel or the project file): `ntp`, `comp`, `mode` (`auto`, `playback`, `export`), `playback_timeout_ms` (250), `export_timeout_ms` (600000). In `auto` mode a frame is treated as an export when it is rendered by the `melt` process, which is how Kdenlive renders.

The complete reference, the wire protocol and the Natron findings are in [docs/DETAILS.md](docs/DETAILS.md).

## Logs and diagnostics

All components append to `~/NatronKdenliveLink/logs/natron-kdenlive.log`, one line per event:

```
2026-10-02T21:55:21.208Z [debug] [filter] [t=395393] event=filter_frame comp=comp frame=391 size=1920x1080 mode=playback status=timeout result=passthrough total_ms=259.4 key=61fd22c7...
```

```bash
tail -f ~/NatronKdenliveLink/logs/natron-kdenlive.log
./build/natron-kdenlive-doctor          # configuration, token, addresses, daemon reachability
./build/natron-kdenlive-cache --status  # cache and queue statistics
./build/natron-kdenlive-cache --clear   # drop all cached frames
```

When something fails, the log and the output of `natron-kdenlive-doctor` are enough for a person, or an AI, to diagnose it.

## Troubleshooting

| Symptom | Cause and fix |
|---|---|
| Daemon prints `Address already in use` | An old daemon is still running. Stop it with `pkill -f natron-kdenlive-daemon` |
| "Natron Link" is not in the effects list | Kdenlive needs the module and the effect XML in the extracted AppImage. Re-run `tools/install-filter.sh install ...`; start Kdenlive from `squashfs-root/AppRun`, not the original `.AppImage`. A log line `Invalid metadata for natron_link` means an old module without MLT metadata |
| Clip looks unprocessed | Check `./build/natron-kdenlive-cache --status`. `workers=0` means no worker is connected, and frames pass through |
| Export is unprocessed | Look for `export_frame_unprocessed` in the log. Start the worker before rendering, or raise `no_worker_wait_ms` |
| Natron snap cannot reach files | The snap sees only non-hidden folders in your home. Keep the exchange folder visible (`NKB_EXCHANGE_DIR`) and use `tcp:` addresses |

## Status and verification

Verified on the author's machine (Ubuntu 24.04.4):

* Build and all tests of the daemon, protocol, cache and tools.
* The filter built against MLT 7.40.0 loads in the Kdenlive 26.08.1 AppImage (MLT 7.41.0), appears in the effects list, processes playback and renders with the test worker, and passes frames through immediately when no worker is connected.
* Natron 2.5.0 snap: the worker connects over loopback TCP, reads and writes its exchange files, creates the default composition, and the first test section (pass-through, cache) passes.
* **Milestone 4, the real chain:** Kdenlive 26.08.1 (AppImage) with the real worker in the Natron 2.5.0 snap, 1080p. In one session, 1365 preview frames went through the filter (1099 from the cache, the rest rendered by Natron or passed through while a render was still running) and a Kdenlive render of 78 frames was recognised as an export and fully processed (no `export_frame_unprocessed`). 338 Natron jobs, none failed, about 180 ms per frame.
* Choosing a composition with the effect panel's **Natron project (.ntp)** field, switching between compositions, and editing a composition in the Natron GUI while the worker runs (the worker reloads the saved file and the new result shows up).

Verified only in the author's development container:

* The full Natron worker test suite against the headless Natron 2.5.0 tarball: pass-through accuracy, Invert graph, sRGB and raw color modes, reload on file change, broken composition, 1080p timing.

Not verified yet:

* The complete worker test on the Natron snap with the 0.3.2 test script (an earlier script had a timing race).
* Other Kdenlive, MLT or Natron versions, the Kdenlive snap, and other distributions.

### Known limits

* One Natron project is loaded per worker; alternating compositions every frame is slow (a reload takes 60 to 120 ms).
* Only RGBA 8-bit frames are supported so far. The protocol already carries other formats.
* Natron works on premultiplied data. For semi-transparent pixels, use Unpremult, the effect, then Premult. Color precision drops for very low alpha.
* Natron 2.5.0 quirks the worker works around: a wrong first render after loading a project (it renders a small throw-away frame), color spaces reset whenever a filename changes (it sets them for every job), failed renders that do not raise errors, and a crash at interpreter exit (the worker exits with `os._exit`).
* A Kdenlive filter attaches to a clip, a track or the master. It cannot add menus, create tracks or act as a true adjustment layer.

## Roadmap

1. Daemon, protocol, cache, tools, test clients (done)
2. MLT filter and Kdenlive effect (done)
3. Natron worker (done)
4. Real end-to-end in Kdenlive with the Natron worker (done); creating and opening compositions from the Kdenlive project, with the compositions next to the project file (not started)
5. Natron parameters and keyframes through Kdenlive's effect panel
6. Nested compositions with cascading invalidation, more pixel formats, disk cache

## Project layout

```
include/nkb/      protocol, sockets, cache, config, client headers
src/              daemon, protocol, network, cache, config, logging
src/filter/       natron_link.cpp (the MLT module)
natron/           nkb_natron_worker.py, make_test_comp.py (run inside Natron)
tools/            cache tool, doctor, test clients, install-filter.sh
data/kdenlive/    effect description for Kdenlive
tests/            unit, end-to-end, filter and Natron worker tests
docs/             detailed reference
dev-memos/        the developer's own notes and example compositions (see below)
```

## Developer memos

The folder [`dev-memos/`](dev-memos/) holds the developer's own working notes. They are kept in the repository so that they are available to everybody, but they are memos, not maintained documentation, and they may be out of date or specific to the author's machine:

* [`dev-memos/rd.txt`](dev-memos/rd.txt): the commands the author used to install the build packages and to build MLT 7.40.0 into `~/projects-own/kdenlive-natron-bridge/third-party` (the project's old folder name).
* [`dev-memos/comps/`](dev-memos/comps/): example Natron compositions from the author's tests. `comp.ntp` is the default pass-through graph the worker creates (`NKB_Input` -> `NKB_Output`); `invert_rgb.ntp` inverts the colors (Read -> Invert -> Write, with a disabled Ramp node); `invert_rgb2.ntp` is the same graph with the Ramp node enabled, so a gradient is drawn over the inverted image. To try one, copy it to `~/NatronKdenliveLink/comps/` and select it in the effect panel. Natron's project-path setting in these files is written as `~/NatronKdenliveLink/comps`; the bridge does not use it, because the worker sets the input and output file paths itself for every frame.

## Uninstall

```bash
tools/install-filter.sh uninstall /path/to/squashfs-root
sudo cmake --build build --target uninstall      # only if you ran: sudo cmake --install build
rm -rf ~/NatronKdenliveLink                      # config, token, logs, compositions
```

## License

GPL-3.0-or-later, see [LICENSE](LICENSE). You may use, study, modify and redistribute this software, including commercially. If you distribute it, or a modified version, you must do so under the same license and make the source code available.

This project builds on other software that keeps its own licenses: Kdenlive and Natron (GPL), MLT (the framework library is LGPL), spdlog and fmt (MIT). Nothing from Kdenlive or Natron is copied into this repository; the project talks to them through MLT's plugin interface and Natron's Python scripting.
