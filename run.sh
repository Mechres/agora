#!/usr/bin/env bash
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [[ ! -x "$DIR/build/agora" ]]; then
    cmake -B "$DIR/build" -S "$DIR" -DCMAKE_BUILD_TYPE=Release
    cmake --build "$DIR/build" -j"$(nproc)"
fi

exec "$DIR/build/agora" "$@"
