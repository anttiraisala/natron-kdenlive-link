#!/usr/bin/env bash
# Installs (or removes) the natron_link MLT filter and its Kdenlive effect XML in an
# EXTRACTED Kdenlive AppImage (the folder created by "<appimage> --appimage-extract").
#
#   tools/install-filter.sh install   /path/to/squashfs-root [path/to/libmltnatron.so]
#   tools/install-filter.sh uninstall /path/to/squashfs-root
#
# Run Kdenlive afterwards with  /path/to/squashfs-root/AppRun
# Nothing outside the given folder is touched.
set -euo pipefail

ACTION="${1:-}"; ROOT="${2:-}"
HERE="$(cd "$(dirname "$0")" && pwd)"
MODULE="${3:-$HERE/../build/libmltnatron.so}"
XML="$HERE/../data/kdenlive/effects/natron_link.xml"

if [[ ( "$ACTION" != install && "$ACTION" != uninstall ) || -z "$ROOT" ]]; then
  sed -n '2,10p' "$0" | sed 's/^# \{0,1\}//'; exit 2
fi
MLT_DIR="$ROOT/usr/lib/mlt-7"
FX_DIR="$ROOT/usr/share/kdenlive/effects"
for d in "$MLT_DIR" "$FX_DIR"; do
  [[ -d "$d" ]] || { echo "error: $d does not exist. Is $ROOT an extracted Kdenlive AppImage?"; exit 1; }
done

if [[ "$ACTION" == uninstall ]]; then
  rm -fv "$MLT_DIR/libmltnatron.so" "$FX_DIR/natron_link.xml"
  echo "uninstalled"; exit 0
fi

[[ -f "$MODULE" ]] || { echo "error: $MODULE not found. Build with -DMLT_ROOT=... first."; exit 1; }
cp -v "$MODULE" "$MLT_DIR/libmltnatron.so"
cp -v "$XML" "$FX_DIR/natron_link.xml"

# Verification: load the module in the AppImage's own melt and check that the filter starts.
# The filter writes event=filter_created to the log; a temporary data dir keeps your real log clean.
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
NKB_HOME="$TMP" LD_LIBRARY_PATH="$ROOT/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  MLT_REPOSITORY="$MLT_DIR" MLT_DATA="$ROOT/usr/share/mlt-7" \
  "$ROOT/usr/bin/melt" color:red out=1 -filter natron_link -consumer null >"$TMP/melt.out" 2>&1 || true
if grep -q "event=filter_created" "$TMP/logs/natron-kdenlive.log" 2>/dev/null; then
  echo "OK: the AppImage's MLT loaded libmltnatron.so and created the filter"
else
  echo "FAILED: the filter did not load into the AppImage's MLT. melt output:"; cat "$TMP/melt.out"
  echo "(please send this output; likely cause: MLT version or library mismatch)"; exit 1
fi
