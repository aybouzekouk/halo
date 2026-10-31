#!/usr/bin/env bash
# Configure and build the Qt application.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
QT="${QT_ROOT:-/home/ayoub/Qt/6.10.2/gcc_64}"
BUILD="$HERE/build"
mkdir -p "$BUILD"
cmake -S "$HERE" -B "$BUILD" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$QT" \
  -DHALO_GL_INCLUDE="$HERE/.sysroot/usr/include"
cmake --build "$BUILD"
