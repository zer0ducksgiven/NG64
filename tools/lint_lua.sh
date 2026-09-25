#!/bin/bash
# Flags Lua that reads a name as a global which the same file defines as a local: a function used above its own
# definition (it's nil at runtime). Syntax checks can't see this.
status=0
for f in "$@"; do
  locals=$(grep -oE "^\s*local function [A-Za-z_][A-Za-z0-9_]*|^\s*local [A-Za-z_][A-Za-z0-9_]*" "$f" | awk '{print $NF}' | sort -u)
  globals=$(/c/msys64/mingw64/bin/luajit -bl "$f" | grep -oE 'GGET .*"[A-Za-z_][A-Za-z0-9_]*"' | grep -oE '"[^"]+"' | tr -d '"' | sort -u)
  bad=$(comm -12 <(echo "$locals") <(echo "$globals"))
  if [ -n "$bad" ]; then echo "LINT $f: used as a global before its local definition: $bad"; status=1; fi
done
exit $status
