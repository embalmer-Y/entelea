#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

if [ "$#" -gt 1 ] ||
   [[ "${1:-native}" != native && "${1:-native}" != p4 &&
      "${1:-native}" != p4-net && "${1:-native}" != p4-tls &&
      "${1:-native}" != p4-sd ]]; then
	echo "Usage: $0 [native|p4|p4-net|p4-tls|p4-sd]" >&2
	exit 2
fi

checkout() {
	local url="$1" rev="$2" dest="$3" patch actual expected
	shift 3
	local patches=("$@") applied=() missing=()

	if [ ! -e "$dest" ]; then
		git init --quiet "$dest"
		git -C "$dest" remote add origin "$url"
	fi
	if ! git -C "$dest" rev-parse --verify HEAD >/dev/null 2>&1; then
		git -C "$dest" fetch --depth 1 origin "$rev"
		git -C "$dest" checkout --detach "$rev"
	fi
	if [ "$(git -C "$dest" rev-parse HEAD)" != "$rev" ]; then
		echo "Dependency revision mismatch or dirty tree: $dest" >&2
		exit 1
	fi
	if [ "${#patches[@]}" -gt 0 ]; then
		if ! git -C "$dest" diff --quiet --cached ||
		   [ -n "$(git -C "$dest" ls-files --others --exclude-standard)" ]; then
			echo "Dependency differs from approved patch: $dest" >&2
			exit 1
		fi
		for patch in "${patches[@]}"; do
			if git -C "$dest" apply --reverse --check "$patch" >/dev/null 2>&1; then
				applied+=("$patch")
			elif git -C "$dest" apply --check "$patch" >/dev/null 2>&1; then
				missing+=("$patch")
			else
				echo "Dependency patch cannot be verified: $dest ($patch)" >&2
				exit 1
			fi
		done
		actual="$(git -C "$dest" diff --no-ext-diff --no-color --no-textconv --unified=3 --binary HEAD |
			sed '/^index /d')"
		expected=""
		if [ "${#applied[@]}" -gt 0 ]; then
			expected="$(cat "${applied[@]}")"
		fi
		if [ "$actual" != "$expected" ]; then
			echo "Dependency differs from approved patches: $dest" >&2
			exit 1
		fi
		if [ "${#missing[@]}" -gt 0 ]; then
			git -C "$dest" apply --check "${missing[@]}"
			git -C "$dest" apply "${missing[@]}"
		fi
		actual="$(git -C "$dest" diff --no-ext-diff --no-color --no-textconv --unified=3 --binary HEAD |
			sed '/^index /d')"
		expected="$(cat "${patches[@]}")"
		if [ "$actual" != "$expected" ]; then
			echo "Dependency differs from approved patches: $dest" >&2
			exit 1
		fi
	elif [ -n "$(git -C "$dest" status --porcelain --ignore-submodules=none)" ]; then
		echo "Dependency revision mismatch or dirty tree: $dest" >&2
		exit 1
	fi
}

mkdir -p "$ROOT/.deps"
checkout https://github.com/zephyrproject-rtos/zephyr.git \
	41d10b59730406bbab4ca9f1fbbe9e96aafb4966 "$ROOT/.deps/zephyr" \
	"$ROOT/patches/zephyr-p4-dsi-frame-size.patch" \
	"$ROOT/patches/zephyr-esp32-gpio-base.patch" \
	"$ROOT/patches/zephyr-cst8xx-report.patch" \
	"$ROOT/patches/zephyr-c5-scan.patch" \
	"$ROOT/patches/zephyr-p4-ram-digest.patch" \
	"$ROOT/patches/zephyr-task-wdt-setup.patch"
checkout https://github.com/bytecodealliance/wasm-micro-runtime.git \
	25bd7eb63e828e4bd242cc9b38d260b4b31c6605 "$ROOT/.deps/wamr"
if [ "${1:-native}" != native ]; then
	checkout https://github.com/zephyrproject-rtos/hal_espressif.git \
		cc81db00233af6020f2fea9fbb0d0098fb3d0d86 "$ROOT/.deps/hal_espressif"
	if [ "$1" = p4-sd ]; then
		checkout https://github.com/zephyrproject-rtos/fatfs.git \
			4f60a12c58c3de4246857ad87c40239ecd93349b "$ROOT/.deps/fatfs"
		echo "Pinned FatFS sources ready; no SD card was accessed."
	fi
	if [[ "$1" = p4-net || "$1" = p4-tls ]]; then
		checkout https://github.com/nanopb/nanopb.git \
			0f6d088e777a33dd7b54297f137ef10d745c5da8 "$ROOT/.deps/nanopb"
		echo "Pinned C5 nanopb RPC sources ready; no device firmware was installed."
	fi
	if [ "$1" = p4-tls ]; then
		checkout https://github.com/zephyrproject-rtos/mbedtls.git \
			c43f34b93797e81fd0257c30004dcd0ae332ae51 "$ROOT/.deps/mbedtls"
		checkout https://github.com/zephyrproject-rtos/tf-psa-crypto.git \
			765af96a20a2b408474dcd950372ba9af4d26b62 "$ROOT/.deps/tf-psa-crypto"
		checkout https://github.com/zephyrproject-rtos/tf-psa-crypto-drivers.git \
			edb74a92e602cd21a218836aff41a0600229978c "$ROOT/.deps/tf-psa-crypto-drivers"
		echo "Pinned TLS sources ready; no credentials or shared SDK were changed."
	fi
	echo "Pinned P4 HAL ready; SDK, blobs and device firmware were not installed."
	exit 0
fi
echo "Pinned native_sim sources ready; no board modules or firmware were installed."
