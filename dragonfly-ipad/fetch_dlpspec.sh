#!/usr/bin/env bash
# Downloads TI's "DLP Spectrum Library" (dlpspec) C sources, which turn the raw scan data from the
# spectrometer into a wavelength / intensity spectrum.
#
# The library is (c) Texas Instruments and ships with TI's NIRscan Nano GUI source code
# (DLPNIRNANOSW, https://www.ti.com/tool/DLPNIRNANOSW). It is not committed to this repository
# because those files carry no explicit redistribution licence. This script pulls the copy that is
# included in the open-source NIRScanner-Python project, pinned to a known commit.
#
# If you prefer, download DLPNIRNANOSW from TI instead and copy the same files (listed below)
# from its "Spectrum Library/src" folder into ThirdParty/dlpspec/.
set -euo pipefail

REPO="https://github.com/HighTemplar-wjiang/NIRScanner-Python.git"
COMMIT="c6c314dcf9d409fc902777b52afb0718ae180c8a"

HERE="$(cd "$(dirname "$0")" && pwd)"
DEST="$HERE/ThirdParty/dlpspec"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

git clone --quiet "$REPO" "$TMP/repo"
git -C "$TMP/repo" checkout --quiet "$COMMIT"
SRC="$TMP/repo/src"

rm -rf "$DEST"
mkdir -p "$DEST/pre-compile"
for f in dlpspec.c dlpspec.h dlpspec_calib.c dlpspec_calib.h dlpspec_helper.c dlpspec_helper.h \
         dlpspec_scan.c dlpspec_scan.h dlpspec_scan_col.c dlpspec_scan_col.h \
         dlpspec_scan_had.c dlpspec_scan_had.h dlpspec_had_defs.h dlpspec_setup.h \
         dlpspec_types.h dlpspec_util.c dlpspec_util.h dlpspec_version.h tpl.c tpl.h; do
    cp "$SRC/$f" "$DEST/$f"
done
cp "$SRC"/pre-compile/*.h "$DEST/pre-compile/"

echo "dlpspec copied to $DEST"
