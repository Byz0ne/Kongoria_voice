#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ZIG="${ZIG_EXE:-zig}"
TARGET="${TARGET:-x86_64-linux-gnu}"
DIST="$ROOT/dist/linux"

mkdir -p "$DIST"

"$ZIG" c++ \
  -target "$TARGET" \
  -shared \
  -fPIC \
  -O2 \
  -std=c++17 \
  -Wall \
  -Wextra \
  -Wno-nullability-completeness \
  -o "$DIST/kongoria_voice.so" \
  "$ROOT/src/kongoria_voice_plugin.cpp" \
  -pthread \
  -ldl \
  -lm

cp "$ROOT/config/kongoria_voice.example.ini" "$DIST/kongoria_voice.ini"

echo "Built Linux plugin: $DIST/kongoria_voice.so"
echo "Copied config: $DIST/kongoria_voice.ini"
