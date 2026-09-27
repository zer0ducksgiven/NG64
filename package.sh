#!/bin/bash
# Builds the helper, the BeamNG mod zip, the installer and the download zip into dist/:
#   dist/ng64.zip            the BeamNG mod
#   dist/NG64/               helper + mod + BeamMP server plugin + docs (the by-hand install)
#   dist/NG64-Setup.exe      the installer (needs Inno Setup 6: winget install JRSoftware.InnoSetup)
#   dist/NG64-<version>.zip  what players download: the installer plus everything for a by-hand install
set -e
cd "$(dirname "$0")"
VERSION="$(tr -d ' \r\n' < VERSION)"
./tools/lint_lua.sh $(find mod server -name "*.lua")
./helper/build.sh
rm -rf dist && mkdir -p dist/NG64
(cd mod && /c/msys64/usr/bin/zip -qr ../dist/ng64.zip . -x '*.git*')
cp helper/dist/ng64helper.exe dist/NG64/
cp dist/ng64.zip dist/NG64/
cp -r server/NG64 dist/NG64/beammp_server_plugin
cp README.md LICENSE installer/INSTALL.txt dist/NG64/

ISCC=""
for c in "$LOCALAPPDATA/Programs/Inno Setup 6/ISCC.exe" "/c/Program Files (x86)/Inno Setup 6/ISCC.exe" "/c/Program Files/Inno Setup 6/ISCC.exe"; do
  [ -f "$c" ] && ISCC="$c" && break
done
if [ -z "$ISCC" ]; then
  echo "packaged: dist/ng64.zip, dist/NG64/ (no installer or download zip: Inno Setup 6 isn't installed)"
  exit 0
fi
MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL="*" "$ISCC" /Q "/DAppVersion=$VERSION" installer/ng64.iss
cp dist/NG64-Setup.exe dist/NG64/
(cd dist && rm -f "NG64-$VERSION.zip" && /c/msys64/usr/bin/zip -qr "NG64-$VERSION.zip" NG64)
echo "packaged $VERSION: dist/ng64.zip, dist/NG64/, dist/NG64-Setup.exe, dist/NG64-$VERSION.zip"
