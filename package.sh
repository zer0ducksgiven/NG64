#!/bin/bash
# Builds the helper and packs the BeamNG mod zip + release folder into dist/.
set -e
cd "$(dirname "$0")"
./tools/lint_lua.sh $(find mod server -name "*.lua")
./helper/build.sh
rm -rf dist && mkdir -p dist/NG64
(cd mod && /c/msys64/usr/bin/zip -qr ../dist/ng64.zip . -x '*.git*')
cp helper/dist/ng64helper.exe dist/NG64/
cp dist/ng64.zip dist/NG64/
cp -r server/NG64 dist/NG64/beammp_server_plugin
cp README.md dist/NG64/ 2>/dev/null || true
# the installer (Inno Setup 6: winget install JRSoftware.InnoSetup)
ISCC=""
for c in "$LOCALAPPDATA/Programs/Inno Setup 6/ISCC.exe" "/c/Program Files (x86)/Inno Setup 6/ISCC.exe" "/c/Program Files/Inno Setup 6/ISCC.exe"; do
  [ -f "$c" ] && ISCC="$c" && break
done
if [ -n "$ISCC" ]; then
  MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL="*" "$ISCC" /Q installer/ng64.iss
  echo "packaged: dist/ng64.zip, dist/NG64/, dist/NG64-Setup.exe"
else
  echo "packaged: dist/ng64.zip, dist/NG64/ (no installer: Inno Setup 6 isn't installed)"
fi
