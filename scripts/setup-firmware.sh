#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

checkout() {
	local url="$1" rev="$2" dest="$3"

	if [ ! -e "$dest" ]; then
		git init --quiet "$dest"
		git -C "$dest" remote add origin "$url"
	fi
	if ! git -C "$dest" rev-parse --verify HEAD >/dev/null 2>&1; then
		git -C "$dest" fetch --depth 1 origin "$rev"
		git -C "$dest" checkout --detach "$rev"
	fi
	if [ "$(git -C "$dest" rev-parse HEAD)" != "$rev" ] ||
	   [ -n "$(git -C "$dest" status --porcelain)" ]; then
		echo "Dependency revision mismatch or dirty tree: $dest" >&2
		exit 1
	fi
}

mkdir -p "$ROOT/.deps"
checkout https://github.com/zephyrproject-rtos/zephyr.git \
	41d10b59730406bbab4ca9f1fbbe9e96aafb4966 "$ROOT/.deps/zephyr"
checkout https://github.com/bytecodealliance/wasm-micro-runtime.git \
	25bd7eb63e828e4bd242cc9b38d260b4b31c6605 "$ROOT/.deps/wamr"
echo "Pinned native_sim sources ready; no board modules or firmware were installed."
