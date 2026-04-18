#!/usr/bin/env bash
# Build script: compiles the engine into ./engine/engine
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p engine
g++ -O3 -march=native -std=c++17 -DNDEBUG -flto -o engine/engine src/engine.cpp
echo "Built engine/engine"
