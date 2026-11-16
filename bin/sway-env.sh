#!/bin/bash
# Source to run rootless sway/grim from ~/vrdesk/root
export ROOT="$HOME/vrdesk/root"
export LD_LIBRARY_PATH="$ROOT/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export PATH="$ROOT/usr/bin:$PATH"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
export WLR_RENDERER_ALLOW_SOFTWARE=1
