#!/usr/bin/env bash
# ============================================================================
#  vendor-emulators.sh - copy upstream emulator sources into cores/*/upstream
#
#  See vendor-emulators.ps1 for the full explanation. This is the POSIX
#  (Git Bash / WSL / Linux / macOS) equivalent.
#
#  Usage:
#     ./vendor-emulators.sh                 # vendor every available core
#     ./vendor-emulators.sh bsnes nestopia  # vendor only these cores
#     FORCE=1 ./vendor-emulators.sh         # overwrite existing vendored trees
#     SRC_ROOT=/path/to/consoles ./vendor-emulators.sh
# ============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SRC_ROOT="${SRC_ROOT:-$(cd "$PROJECT_ROOT/../../consoles" 2>/dev/null && pwd || echo "$PROJECT_ROOT/../../consoles")}"
FORCE="${FORCE:-0}"

# core:upstreamRelPath:destRelDir:markerRelPath
CORES=(
  "nestopia:nes/nestopia-master:cores/nestopia/upstream:source/core/NstCore.hpp"
  "bsnes:snes/bsnes-master:cores/bsnes/upstream:bsnes/emulator/emulator.cpp"
  "mgba:gameboyadvanced and gbc/mgba-master:cores/mgba/upstream:include/mgba/core/core.h"
  "desmume:Ds/desmume-master/desmume:cores/desmume/upstream:src/NDSSystem.cpp"
  "project64:n64/project64-develop:cores/project64/upstream:Source/Project64"
  "dolphin:gamecube and wii/dolphin-master:cores/dolphin/upstream:Source/Core/Core/Core.h"
  "azahar:3ds/azahar-master:cores/azahar/upstream:src/core/core.h"
)

want=("$@")
should_vendor() {
  [ ${#want[@]} -eq 0 ] && return 0
  local n; for n in "${want[@]}"; do [ "$n" = "$1" ] && return 0; done
  return 1
}

echo "Project    : $PROJECT_ROOT"
echo "Source root: $SRC_ROOT"
echo

vendored=0; skipped=0; missing=0

for entry in "${CORES[@]}"; do
  IFS=':' read -r name src_rel dst_rel marker_rel <<< "$entry"
  should_vendor "$name" || continue

  src="$SRC_ROOT/$src_rel"
  dst="$PROJECT_ROOT/$dst_rel"

  if [ ! -e "$src/$marker_rel" ]; then
    echo "WARN  [$name] source not found (looked for '$src/$marker_rel') - skipping"
    missing=$((missing + 1))
    continue
  fi

  if [ -e "$dst/$marker_rel" ] && [ "$FORCE" != "1" ]; then
    echo "SKIP  [$name] already vendored at $dst (FORCE=1 to overwrite)"
    skipped=$((skipped + 1))
    continue
  fi

  echo "COPY  [$name] $src"
  echo "        -> $dst"
  rm -rf "$dst"
  mkdir -p "$(dirname "$dst")"
  cp -R "$src" "$dst"
  vendored=$((vendored + 1))
done

echo
echo "Done. vendored=$vendored skipped=$skipped missing=$missing"
[ "$vendored" -gt 0 ] && echo "Re-run CMake so the build picks up the new paths."
exit 0
