#!/bin/bash
# Build the vrdesk bridge (host side).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
mkdir -p "$HERE/bin"
gcc -O2 -Wall -Wextra -Wno-unused-parameter \
    -o "$HERE/bin/vrdesk-bridge" \
    "$HERE/src/bridge.c" \
    "$HERE/proto/wlr-screencopy-unstable-v1-protocol.c" \
    "$HERE/proto/wlr-virtual-pointer-unstable-v1-protocol.c" \
    -I"$HERE/proto" \
    $(pkg-config --cflags --libs wayland-client) -lm
echo "built $HERE/bin/vrdesk-bridge"
