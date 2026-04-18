#!/usr/bin/env bash
# UCI entrypoint: build if needed, then exec the engine binary.
set -euo pipefail
# Move to workspace root (parent of this script's dir).
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
WORKSPACE="$(dirname "$SCRIPT_DIR")"
cd "$WORKSPACE"

ENGINE_BIN="./engine/engine"
SRC="./src/engine.cpp"

# Build if the binary is missing or the source is newer than the binary.
need_build=0
if [ ! -x "$ENGINE_BIN" ]; then
    need_build=1
elif [ "$SRC" -nt "$ENGINE_BIN" ]; then
    need_build=1
fi

if [ "$need_build" -eq 1 ]; then
    mkdir -p engine
    # Build quietly to stderr so UCI stdout stays clean.
    g++ -O3 -march=native -std=c++17 -DNDEBUG -flto -pthread \
        -o "$ENGINE_BIN" "$SRC" 1>&2
fi

exec "$ENGINE_BIN" "$@"
