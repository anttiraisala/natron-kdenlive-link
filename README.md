# natron-kdenlive-link

Use Natron compositions as an effect in Kdenlive, in the style of Adobe Dynamic Link ( as used to link Adobe Premiere with After Effects ). Add the **Natron Link** effect to a clip, build the graph in Natron, and see the result in Kdenlive's preview and in rendered files.

> **Status: early development (version 0.4.0, milestone 4 of 6).** The whole chain works on the author's machine: Kdenlive (extracted AppImage) sends frames through the daemon to the real worker inside the Natron snap, and the processed frames come back in Kdenlive's preview and in rendered files. Parameters, nested compositions and other pixel formats are not implemented yet. See [Status and verification](#status-and-verification) for exactly what has and has not been tested.

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

## Folders used in these instructions

Every command block below starts with a `# Folder:` line that says where it runs, and with a `cd` into that folder, so a block can be pasted into any terminal. The instructions use this layout; if you choose other folders, change the paths in the commands.

| Folder | Contents |
|---|---|
| `~/projects-own/natron-kdenlive-link` | This repository (source, `build/`, and the MLT build in `third-party/`) |
| `~/apps/kdenlive` | The Kdenlive AppImage and its extracted copy `squashfs-root/` |
| `~/apps/natron` | Only if you use the Natron tarball instead of the snap |
| `~/NatronKdenliveLink` | Created by the programs at run time: `config.ini`, `token`, `logs/`, `comps/` (your Natron compositions), `exchange/` |

## Installing Kdenlive (the AppImage is required)

The filter is a shared library that must be copied into Kdenlive's own MLT module folder. That only works when this folder is writable, which is why the install method matters:

| Kdenlive install | Works with the filter? |
|---|---|
| **AppImage, extracted** | **Yes. This is the tested and supported way.** |
| Snap (`snap install kdenlive`) | No. Its MLT folder is read-only. |
| Flatpak | No. Its MLT folder is read-only inside the sandbox. |
| Ubuntu package (`apt install kdenlive`) | Not tested. Ubuntu 24.04 ships an old Kdenlive and MLT; the filter would have to be copied into a system folder with `sudo`. |

You can keep another Kdenlive installed next to the AppImage, but only the extracted AppImage started with `AppRun` has the filter. The Kdenlive in your application menu is the other one.

**Create the folder**
```bash
# Folder: any
mkdir -p ~/apps/kdenlive
```

**Download the AppImage.** If this link no longer works, download the Linux AppImage from https://kdenlive.org/download/ into `~/apps/kdenlive` instead, and use its file name in the next commands.
```bash
# Folder: ~/apps/kdenlive
cd ~/apps/kdenlive
wget https://download.kde.org/stable/kdenlive/26.08/linux/kdenlive-26.08.1-x86_64.AppImage
```

**Extract it.** This creates `~/apps/kdenlive/squashfs-root`, a normal writable folder with the whole application. The filter is installed there later.
```bash
# Folder: ~/apps/kdenlive
cd ~/apps/kdenlive
chmod +x kdenlive-26.08.1-x86_64.AppImage
./kdenlive-26.08.1-x86_64.AppImage --appimage-extract
```

**Check it.** This prints the MLT version inside the AppImage (7.41.0 for Kdenlive 26.08.1).
```bash
# Folder: any
ls ~/apps/kdenlive/squashfs-root/usr/lib/libmlt-7.so.*
```

**Start Kdenlive** always like this, never by running the `.AppImage` file itself (that starts a fresh read-only copy without the filter):
```bash
# Folder: any
~/apps/kdenlive/squashfs-root/AppRun
```

## Installing Natron

Natron needs no changes, so either install method works. The worker runs inside Natron's own Python with the command `natron -t`. The snap is what the author uses every day; the tarball was tested headless.

### Option A: snap (recommended)

```bash
# Folder: any
sudo snap install natron
```

```bash
# Folder: any
snap list natron
```

The second command must show version 2.5.0. A snap can only reach non-hidden folders in your home and cannot see other programs' Unix sockets. The defaults handle both: the data folder is `~/NatronKdenliveLink` and the connection is loopback TCP. The Natron GUI is in your application menu, or `snap run natron`.

### Option B: official tarball

```bash
# Folder: any
sudo apt install -y libglu1-mesa wget
```

```bash
# Folder: any
mkdir -p ~/apps/natron
```

```bash
# Folder: ~/apps/natron
cd ~/apps/natron
wget https://github.com/NatronGitHub/Natron/releases/download/v2.5.0/Natron-2.5.0-Linux-x86_64-no-installer.tar.xz
tar xJf Natron-2.5.0-Linux-x86_64-no-installer.tar.xz
```

The GUI is `~/apps/natron/Natron-2.5.0-Linux-x86_64-no-installer/Natron`. The worker uses `NatronRenderer` from the same folder. A message `Error while loading OpenGL: X11: Failed to open display` when running headless is harmless.

## Building

### 1. Install the build packages

```bash
# Folder: any
sudo apt update
sudo apt install -y build-essential cmake ninja-build git pkg-config libspdlog-dev libfmt-dev libxml2-dev
```

### 2. Get the source

```bash
# Folder: ~/projects-own
mkdir -p ~/projects-own
cd ~/projects-own
git clone https://github.com/anttiraisala/natron-kdenlive-link.git
```

Later, to get the newest version: `cd ~/projects-own/natron-kdenlive-link && git pull`, then build again (steps 3 and 5).

### 3. Build and run the tests (no Kdenlive or Natron needed)

```bash
# Folder: ~/projects-own/natron-kdenlive-link
cd ~/projects-own/natron-kdenlive-link
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Use `-DCMAKE_BUILD_TYPE=Debug` instead of `Release` while developing.

### 4. Build MLT 7.40.0 (needed once, for the filter)

The filter needs MLT 7.40.0 headers. This builds a minimal MLT into `third-party/` inside the repository (ignored by git). It takes a few minutes.

```bash
# Folder: ~/projects-own/natron-kdenlive-link
cd ~/projects-own/natron-kdenlive-link
git clone --branch v7.40.0 --depth 1 https://github.com/mltframework/mlt.git third-party/mlt-src
```

```bash
# Folder: ~/projects-own/natron-kdenlive-link
cd ~/projects-own/natron-kdenlive-link
cmake -S third-party/mlt-src -B third-party/mlt-src/build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$HOME/projects-own/natron-kdenlive-link/third-party/mlt-7.40.0" \
  -DBUILD_TESTING=OFF -DGPL=OFF -DGPL3=OFF \
  -DMOD_AVFORMAT=OFF -DMOD_DECKLINK=OFF -DMOD_FREI0R=OFF -DMOD_GDK=OFF -DMOD_GLAXNIMATE_QT6=OFF \
  -DMOD_JACKRACK=OFF -DMOD_KDENLIVE=OFF -DMOD_MOVIT=OFF -DMOD_NDI=OFF -DMOD_NORMALIZE=OFF \
  -DMOD_OLDFILM=OFF -DMOD_OPENCV=OFF -DMOD_OPENFX=OFF -DMOD_PLUS=OFF -DMOD_PLUSGPL=OFF -DMOD_QT6=OFF \
  -DMOD_RESAMPLE=OFF -DMOD_RTAUDIO=OFF -DMOD_RUBBERBAND=OFF -DMOD_RNNOISE=OFF -DMOD_SDL1=OFF \
  -DMOD_SDL2=OFF -DMOD_SOX=OFF -DMOD_SPATIALAUDIO=OFF -DMOD_VIDSTAB=OFF -DMOD_VORBIS=OFF -DMOD_XINE=OFF
```

```bash
# Folder: ~/projects-own/natron-kdenlive-link
cd ~/projects-own/natron-kdenlive-link
cmake --build third-party/mlt-src/build
cmake --install third-party/mlt-src/build
```

### 5. Build again with the filter

```bash
# Folder: ~/projects-own/natron-kdenlive-link
cd ~/projects-own/natron-kdenlive-link
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DMLT_ROOT="$HOME/projects-own/natron-kdenlive-link/third-party/mlt-7.40.0"
cmake --build build
ctest --test-dir build --output-on-failure
```

This now also builds `build/libmltnatron.so` and runs the filter tests (3 tests in total). To also run the Natron worker test, add `-DNATRON_COMMAND="snap run natron"` (or the path of `NatronRenderer`) to the first command; see [docs/DETAILS.md](docs/DETAILS.md) for running it by hand.

## Running

### 1. Install the filter into the extracted Kdenlive

Close Kdenlive first. Repeat this step after every rebuild that changes the filter.

```bash
# Folder: ~/projects-own/natron-kdenlive-link
cd ~/projects-own/natron-kdenlive-link
tools/install-filter.sh install ~/apps/kdenlive/squashfs-root
```

It copies `libmltnatron.so` and the effect description into the extracted tree and checks that the AppImage's own `melt` can load the filter. It must print `OK: the AppImage's MLT loaded libmltnatron.so`.

### 2. Try it with the test worker (inverts colors)

Run each block in its own terminal and leave it running.

**Terminal 1: the daemon**
```bash
# Folder: ~/projects-own/natron-kdenlive-link
cd ~/projects-own/natron-kdenlive-link
./build/natron-kdenlive-daemon
```

**Terminal 2: the test worker**
```bash
# Folder: ~/projects-own/natron-kdenlive-link
cd ~/projects-own/natron-kdenlive-link
./build/nkb-mock-worker --mode invert --delay-ms 100
```

**Terminal 3: Kdenlive**
```bash
# Folder: any
~/apps/kdenlive/squashfs-root/AppRun
```

In Kdenlive, search the Effects tab for **Natron Link** (listed under *Misc*), drag it onto a clip, and play. The clip shows inverted colors after the first pass over each frame.

### 3. Use the real Natron worker

Stop the test worker (Ctrl+C in terminal 2), then start the Natron worker in terminal 2 instead. Use the line for your Natron install.

**Terminal 2: Natron worker, snap**
```bash
# Folder: ~/projects-own/natron-kdenlive-link
cd ~/projects-own/natron-kdenlive-link
snap run natron -t natron/nkb_natron_worker.py
```

**Terminal 2: Natron worker, tarball**
```bash
# Folder: ~/projects-own/natron-kdenlive-link
cd ~/projects-own/natron-kdenlive-link
~/apps/natron/Natron-2.5.0-Linux-x86_64-no-installer/NatronRenderer -t natron/nkb_natron_worker.py
```

**Terminal 4: check that the worker is connected** (wait a few seconds after starting it; must print `workers=1`)
```bash
# Folder: ~/projects-own/natron-kdenlive-link
cd ~/projects-own/natron-kdenlive-link
./build/natron-kdenlive-cache --status | grep -E "^workers="
```

**Each effect gets its own composition.** When a newly added Natron Link effect renders its first frame, it picks a new composition name `comp-xxxxxx` (6 random letters and digits) and sets its **Natron project (.ntp)** field to `~/NatronKdenliveLink/comps/comp-xxxxxx.ntp`. The name is saved with the Kdenlive project. The worker creates that file as a default pass-through graph (Read node `NKB_Input` connected to Write node `NKB_Output`) when it renders the first frame for it. Open the file in the Natron GUI, add nodes between the two, and save. The worker reloads the file when it changes, and Kdenlive shows the new result.

The name is picked when the effect renders its first frame, so the timeline cursor has to be over the clip (or the clip has to be played) once. Kdenlive's effect panel does not show the new path right away, because the panel shows its own copy of the values. To see it, switch the effect off and on again with its enable button (observed with Kdenlive 26.08.1). The log also names it: `grep comp_assigned ~/NatronKdenliveLink/logs/natron-kdenlive.log`.

**Alpha matters.** The alpha channel that comes out of the graph is used as is. Several Natron nodes also change alpha by default; for example the **Invert** node inverts R, G, B **and A**, so an opaque clip comes back fully transparent and Kdenlive shows it as black (or shows the track below it). Untick **A** in the Invert node's channel checkboxes to invert only the colors. The same applies to any node whose output looks black: check what it does to alpha. Or tick **Keep original alpha** in the effect panel: the effect then takes the colors from Natron and the alpha from the clip (this hides alpha changes made on purpose, such as keying, so it is off by default).

**Open in Natron.** The effect panel has a checkbox **Open in Natron (click to open)**. Kdenlive effects cannot have push buttons, so this checkbox works as one: every click, ticking or unticking, opens the effect's composition (the file in the **Natron project (.ntp)** field, or the effect's own `comp-xxxxxx`) in the Natron GUI. If the file does not exist yet, a pass-through graph is created first. The daemon starts Natron, so it must be running; Natron's window appears on the display of the terminal the daemon was started from. A composition that is already open in a Natron started this way is not opened twice. The two nodes of a new composition lie on top of each other in Natron's Node Graph; drag `NKB_Output` aside once. The commands are set in `config.ini`, see [Configuration](#configuration); for the Natron tarball, change both to the tarball's `Natron` and `NatronRenderer`.

**To use another composition**, choose a different file in `~/NatronKdenliveLink/comps/` with the **Natron project (.ntp)** field; the worker uses it from the next frame on. Several effects can share one composition this way. If you empty the field, the effect goes back to its own `comp-xxxxxx`. For now the chosen file must be in `~/NatronKdenliveLink/comps/`: the worker loads `comps/<file name>`, not a file in another folder.

### Stopping everything

```bash
# Folder: any
pkill -9 -f natron-kdenlive-daemon; pkill -9 -f nkb-mock-worker; pkill -9 -f nkb_natron_worker; sleep 1; pgrep -af "natron-kdenlive-daemon|nkb-mock-worker|nkb_natron_worker" || echo "all stopped"
```

It must print `all stopped`. `-9` is used because Natron and `snap run` can ignore the normal stop signal; nothing is lost by it, because the daemon keeps its cache only in memory and the worker saves nothing. `-f` is required: `pkill` without it compares only the first 15 characters of a process name and does not match `natron-kdenlive-daemon`. Natron may leave a `<comp>.ntp.lock` file next to a composition; it is harmless.

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
| `[natron] gui_command` | `snap run natron` | Starts the Natron GUI for **Open in Natron**; the `.ntp` path is added at the end |
| `script_command` | `snap run natron` | Runs a Natron Python script headless (`-t script.py`), used to create a new composition |
| `scripts_dir` | empty | Folder of `nkb_new_comp.py`; empty = found automatically |
| `[logging] level` | `debug` | `trace`, `debug`, `info`, `warn`, `error` |

Only loopback addresses are accepted. Every connection starts with a token handshake; the token is in `~/NatronKdenliveLink/token` (mode 0600). Loopback TCP is the default because a snap cannot see another snap's Unix sockets.

Filter properties (set in Kdenlive's effect panel or the project file): `ntp`, `comp`, `mode` (`auto`, `playback`, `export`), `playback_timeout_ms` (250), `export_timeout_ms` (600000), `keep_alpha` (0), `open_natron` (the "button"). In `auto` mode a frame is treated as an export when it is rendered by the `melt` process, which is how Kdenlive renders.

The complete reference, the wire protocol and the Natron findings are in [docs/DETAILS.md](docs/DETAILS.md).

## Logs and diagnostics

All components append to `~/NatronKdenliveLink/logs/natron-kdenlive.log`, one line per event:

```
2026-10-02T21:55:21.208Z [debug] [filter] [t=395393] event=filter_frame comp=comp frame=391 size=1920x1080 mode=playback status=timeout result=passthrough total_ms=259.4 key=61fd22c7...
```

**Follow the log**
```bash
# Folder: any
tail -f ~/NatronKdenliveLink/logs/natron-kdenlive.log
```

**Diagnostics and cache**
```bash
# Folder: ~/projects-own/natron-kdenlive-link
cd ~/projects-own/natron-kdenlive-link
./build/natron-kdenlive-doctor          # configuration, token, addresses, daemon reachability
./build/natron-kdenlive-cache --status  # cache and queue statistics
./build/natron-kdenlive-cache --clear   # drop all cached frames
```

When something fails, the log and the output of `natron-kdenlive-doctor` are enough for a person, or an AI, to diagnose it.

## Troubleshooting

| Symptom | Cause and fix |
|---|---|
| Daemon prints `Address already in use` | An old daemon is still running. Stop it with `pkill -9 -f natron-kdenlive-daemon` |
| "Natron Link" is not in the effects list | Kdenlive needs the module and the effect XML in the extracted AppImage. Re-run `tools/install-filter.sh install ~/apps/kdenlive/squashfs-root`; start Kdenlive with `~/apps/kdenlive/squashfs-root/AppRun`, not the original `.AppImage`. A log line `Invalid metadata for natron_link` means an old module without MLT metadata |
| Clip looks unprocessed | Check `./build/natron-kdenlive-cache --status`. `workers=0` means no worker is connected, and frames pass through |
| Export is unprocessed | Look for `export_frame_unprocessed` in the log. Start the worker before rendering, or raise `no_worker_wait_ms` |
| Natron snap cannot reach files | The snap sees only non-hidden folders in your home. Keep the exchange folder visible (`NKB_EXCHANGE_DIR`) and use `tcp:` addresses |

## Status and verification

Verified on the author's machine (Ubuntu 24.04.4):

* Build and all tests of the daemon, protocol, cache and tools.
* The filter built against MLT 7.40.0 loads in the Kdenlive 26.08.1 AppImage (MLT 7.41.0), appears in the effects list, processes playback and renders with the test worker, and passes frames through immediately when no worker is connected.
* Natron 2.5.0 snap: the complete worker test (`tests/natron_e2e.sh` over loopback TCP) passes: pass-through accuracy, cache, Invert graph, reload on file change, warm-up, broken composition and recovery, sRGB mode.
* **Milestone 4, the real chain:** Kdenlive 26.08.1 (AppImage) with the real worker in the Natron 2.5.0 snap, 1080p. In one session, 1365 preview frames went through the filter (1099 from the cache, the rest rendered by Natron or passed through while a render was still running) and a Kdenlive render of 78 frames was recognised as an export and fully processed (no `export_frame_unprocessed`). 338 Natron jobs, none failed, about 180 ms per frame.
* Choosing a composition with the effect panel's **Natron project (.ntp)** field, switching between compositions, and editing a composition in the Natron GUI while the worker runs (the worker reloads the saved file and the new result shows up).

Verified only in the author's development container:

* The full Natron worker test suite against the headless Natron 2.5.0 tarball: pass-through accuracy, Invert graph, sRGB and raw color modes, reload on file change, broken composition, 1080p timing.

Not verified yet:

* Other Kdenlive, MLT or Natron versions, the Kdenlive snap, and other distributions.

### Known limits

* One Natron project is loaded per worker; alternating compositions every frame is slow (a reload takes 60 to 120 ms).
* Only RGBA 8-bit frames are supported so far. The protocol already carries other formats.
* Natron works on premultiplied data. For semi-transparent pixels, use Unpremult, the effect, then Premult. Color precision drops for very low alpha.
* Natron 2.5.0 quirks the worker works around: a wrong first render after loading a project (it renders a small throw-away frame), color spaces reset whenever a filename changes (it sets them for every job), failed renders that do not raise errors, and a crash at interpreter exit (the worker exits with `os._exit`).
* Copying an effect in Kdenlive may copy its composition name too, so both effects share one composition. Choose another file for one of them if that is not wanted.
* A Natron project file in a folder other than `~/NatronKdenliveLink/comps/` is not supported yet (see "To use another composition" above).
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

**Remove the filter from the extracted Kdenlive** (close Kdenlive first)
```bash
# Folder: ~/projects-own/natron-kdenlive-link
cd ~/projects-own/natron-kdenlive-link
tools/install-filter.sh uninstall ~/apps/kdenlive/squashfs-root
```

**Remove installed programs** (only if you ran `sudo cmake --install build`)
```bash
# Folder: ~/projects-own/natron-kdenlive-link
cd ~/projects-own/natron-kdenlive-link
sudo cmake --build build --target uninstall
```

**Remove all run-time data** (config, token, logs, and your compositions in `comps/`; back them up first)
```bash
# Folder: any
rm -rf ~/NatronKdenliveLink
```

## License

GPL-3.0-or-later, see [LICENSE](LICENSE). You may use, study, modify and redistribute this software, including commercially. If you distribute it, or a modified version, you must do so under the same license and make the source code available.

This project builds on other software that keeps its own licenses: Kdenlive and Natron (GPL), MLT (the framework library is LGPL), spdlog and fmt (MIT). Nothing from Kdenlive or Natron is copied into this repository; the project talks to them through MLT's plugin interface and Natron's Python scripting.
