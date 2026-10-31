#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
export ZEPHYR_BASE="${ZEPHYR_BASE:-$ROOT/.deps/zephyr}"
export TS_WAMR_DIR="${TS_WAMR_DIR:-$ROOT/.deps/wamr}"
export ZEPHYR_TOOLCHAIN_VARIANT=host
export PATH="$ROOT/.venv/bin:$PATH"

for file in "$ZEPHYR_BASE/CMakeLists.txt" "$TS_WAMR_DIR/CMakeLists.txt"; do
	if [ ! -f "$file" ]; then
		echo "Missing source: $file; run bash scripts/setup-firmware.sh" >&2
		exit 1
	fi
done

case "${1:-}" in
	build)
		cmake -S "$ROOT/firmware/app" -B "$ROOT/build/firmware" -G Ninja \
			-DBOARD=native_sim/native/64 \
			-DZEPHYR_MODULES="$ROOT/firmware/module/tessera"
		cmake --build "$ROOT/build/firmware"
		;;
	test)
		python "$ZEPHYR_BASE/scripts/twister" -p native_sim/native/64 \
			-T "$ROOT/firmware/tests" --outdir "$ROOT/build/twister" \
			--inline-logs -j 2 \
			--extra-args="ZEPHYR_MODULES=$ROOT/firmware/module/tessera"
		;;
	*)
		echo "Usage: $0 build|test" >&2
		exit 2
		;;
esac
