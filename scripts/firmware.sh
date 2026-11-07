#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
export ZEPHYR_BASE="${ZEPHYR_BASE:-$ROOT/.deps/zephyr}"
export TS_WAMR_DIR="${TS_WAMR_DIR:-$ROOT/.deps/wamr}"
export PATH="$ROOT/.venv/bin:$PATH"

if [ "$#" -ne 1 ] ||
   [[ "$1" != build && "$1" != test && "$1" != build-p4 &&
      "$1" != build-p4-store && "$1" != build-p4-net && "$1" != build-p4-tls &&
      "$1" != build-p4-sd && "$1" != build-p4-sd-detect &&
      "$1" != build-p4-i2s && "$1" != build-p4-i2s-candidate &&
      "$1" != build-p4-i2s-candidate32 &&
      "$1" != build-p4-i2s-stop && "$1" != build-p4-i2s-stop-candidate &&
      "$1" != build-p4-touch-id && "$1" != build-p4-touch &&
      "$1" != build-p4-display && "$1" != build-p4-boot-trace ]]; then
	echo "Usage: $0 build|test|build-p4|build-p4-store|build-p4-net|build-p4-tls|build-p4-sd|build-p4-sd-detect|build-p4-i2s|build-p4-i2s-candidate|build-p4-i2s-candidate32|build-p4-i2s-stop|build-p4-i2s-stop-candidate|build-p4-touch-id|build-p4-touch|build-p4-display|build-p4-boot-trace" >&2
	exit 2
fi

for file in "$ZEPHYR_BASE/CMakeLists.txt" "$TS_WAMR_DIR/CMakeLists.txt"; do
	if [ ! -f "$file" ]; then
		echo "Missing source: $file; run bash scripts/setup-firmware.sh" >&2
		exit 1
	fi
done

check_zephyr_patch() {
	if ! git -C "$ZEPHYR_BASE" apply --reverse --check \
		"$ROOT/patches/zephyr-p4-dsi-frame-size.patch" \
		"$ROOT/patches/zephyr-esp32-gpio-base.patch" \
		"$ROOT/patches/zephyr-cst8xx-report.patch" \
		"$ROOT/patches/zephyr-c5-scan.patch" \
		"$ROOT/patches/zephyr-p4-ram-digest.patch" \
		"$ROOT/patches/zephyr-task-wdt-setup.patch"; then
		echo "Missing or modified approved Zephyr patches; run bash scripts/setup-firmware.sh" >&2
		exit 1
	fi
}

case "${1:-}" in
	build)
		check_zephyr_patch
		export ZEPHYR_TOOLCHAIN_VARIANT=host
		cmake -S "$ROOT/firmware/app" -B "$ROOT/build/firmware" -G Ninja \
			-DBOARD=native_sim/native/64 \
			-DZEPHYR_MODULES="$ROOT/firmware/module/tessera"
		cmake --build "$ROOT/build/firmware"
		;;
	test)
		check_zephyr_patch
		export ZEPHYR_TOOLCHAIN_VARIANT=host
		python "$ZEPHYR_BASE/scripts/twister" -p native_sim/native/64 \
			-T "$ROOT/firmware/tests" --outdir "$ROOT/build/twister" \
			--inline-logs -j 2 \
			--extra-args="ZEPHYR_MODULES=$ROOT/firmware/module/tessera"
		;;
	build-p4|build-p4-store|build-p4-net|build-p4-tls|build-p4-sd|build-p4-sd-detect|build-p4-i2s|build-p4-i2s-candidate|build-p4-i2s-candidate32|build-p4-i2s-stop|build-p4-i2s-stop-candidate|build-p4-touch-id|build-p4-touch|build-p4-display|build-p4-boot-trace)
		export TS_HAL_ESPRESSIF_DIR="${TS_HAL_ESPRESSIF_DIR:-$ROOT/.deps/hal_espressif}"
		if [ ! -f "$TS_HAL_ESPRESSIF_DIR/zephyr/module.yml" ]; then
			echo "Missing P4 HAL: $TS_HAL_ESPRESSIF_DIR; run bash scripts/setup-firmware.sh p4" >&2
			exit 1
		fi
		if [ ! -f "${ZEPHYR_SDK_INSTALL_DIR:-}/sdk_version" ]; then
			echo "Set ZEPHYR_SDK_INSTALL_DIR to an installed Zephyr SDK with the RISC-V toolchain" >&2
			exit 1
		fi
		check_zephyr_patch
		export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
		build_dir="$ROOT/build/firmware-p4"
		source_dir="$ROOT/firmware/app"
		conf_file="$ROOT/firmware/app/boards/esp32p4c5_core_board_esp32p4_hpcore.conf"
		modules="$ROOT/firmware/module/tessera;$TS_HAL_ESPRESSIF_DIR"
		board_args=()
		if [ "$1" = build-p4-store ]; then
			build_dir="$ROOT/build/firmware-p4-store"
			conf_file+=";$ROOT/firmware/app/boards/esp32p4c5_core_board_esp32p4_hpcore-store.conf"
			board_args+=("-DDTC_OVERLAY_FILE=$ROOT/firmware/app/boards/esp32p4c5_core_board_esp32p4_hpcore-store.overlay")
		elif [ "$1" = build-p4-display ]; then
			build_dir="$ROOT/build/firmware-p4-display"
			source_dir="$ROOT/firmware/tests/board/p4display"
			conf_file="$source_dir/prj.conf"
			board_args+=("-DKCONFIG_WARNING_AS_ERROR=ON")
		elif [ "$1" = build-p4-sd-detect ]; then
			build_dir="$ROOT/build/firmware-p4-sd-detect"
			source_dir="$ROOT/firmware/tests/board/p4sddetect"
			conf_file="$source_dir/prj.conf"
			board_args+=("-DKCONFIG_WARNING_AS_ERROR=ON")
		elif [[ "$1" = build-p4-i2s || "$1" = build-p4-i2s-candidate ||
			"$1" = build-p4-i2s-candidate32 || "$1" = build-p4-i2s-stop ||
			"$1" = build-p4-i2s-stop-candidate ]]; then
			build_dir="$ROOT/build/firmware-${1#build-}"
			source_dir="$ROOT/firmware/tests/board/p4i2sbench"
			conf_file="$source_dir/prj.conf"
			candidate=OFF
			word32=OFF
			lifecycle=OFF
			if [[ "$1" = build-p4-i2s-candidate || "$1" = build-p4-i2s-candidate32 ||
				"$1" = build-p4-i2s-stop-candidate ]]; then
				candidate=ON
			fi
			if [[ "$1" = build-p4-i2s-candidate32 || "$1" = build-p4-i2s-stop ||
				"$1" = build-p4-i2s-stop-candidate ]]; then
				word32=ON
			fi
			if [[ "$1" = build-p4-i2s-stop || "$1" = build-p4-i2s-stop-candidate ]]; then
				lifecycle=ON
			fi
			board_args+=("-DKCONFIG_WARNING_AS_ERROR=ON"
				"-DP04_I2S_GDMA_CANDIDATE=$candidate" "-DP04_I2S_WORD32=$word32"
				"-DP04_I2S_LIFECYCLE_TEST=$lifecycle")
		elif [ "$1" = build-p4-touch ]; then
			build_dir="$ROOT/build/firmware-p4-touch"
			source_dir="$ROOT/firmware/tests/board/p4touch"
			conf_file="$source_dir/prj.conf"
			board_args+=("-DKCONFIG_WARNING_AS_ERROR=ON")
		elif [[ "$1" = build-p4-touch-id || "$1" = build-p4-boot-trace ]]; then
			build_dir="$ROOT/build/firmware-${1#build-}"
			source_dir="$ROOT/firmware/tests/board/p4touchid"
			conf_file="$source_dir/prj.conf"
			if [ "$1" = build-p4-boot-trace ]; then
				conf_file+=";$source_dir/boottrace.conf"
			fi
			board_args+=("-DKCONFIG_WARNING_AS_ERROR=ON")
		elif [ "$1" = build-p4-sd ]; then
			if [ ! -f "$ROOT/.deps/fatfs/zephyr/module.yml" ]; then
				echo "Missing FatFS module: run bash scripts/setup-firmware.sh p4-sd" >&2
				exit 1
			fi
			build_dir="$ROOT/build/firmware-p4-sd"
			source_dir="$ROOT/firmware/tests/board/p4sdbench"
			conf_file="$source_dir/prj.conf"
			modules+=";$ROOT/.deps/fatfs"
			board_args+=("-DKCONFIG_WARNING_AS_ERROR=ON")
		elif [[ "$1" = build-p4-net || "$1" = build-p4-tls ]]; then
			export TS_NANOPB_DIR="${TS_NANOPB_DIR:-$ROOT/.deps/nanopb}"
			if [ ! -f "$TS_NANOPB_DIR/zephyr/module.yml" ]; then
				echo "Missing C5 nanopb: $TS_NANOPB_DIR; run bash scripts/setup-firmware.sh p4-net" >&2
				exit 1
			fi
			build_dir="$ROOT/build/firmware-p4-net"
			source_dir="$ROOT/firmware/tests/board/p4wifibench"
			conf_file="$source_dir/boards/esp32p4c5_core_board_esp32p4_hpcore.conf"
			modules+=";$TS_NANOPB_DIR"
			if [ "$1" = build-p4-tls ]; then
				for module in mbedtls tf-psa-crypto tf-psa-crypto-drivers; do
					if [ ! -f "$ROOT/.deps/$module/zephyr/module.yml" ]; then
						echo "Missing TLS module: $module; run bash scripts/setup-firmware.sh p4-tls" >&2
						exit 1
					fi
					modules+=";$ROOT/.deps/$module"
				done
				build_dir="$ROOT/build/firmware-p4-tls"
				conf_file+=";$source_dir/boards/esp32p4c5_core_board_esp32p4_hpcore-tls.conf"
				board_args+=("-DKCONFIG_WARNING_AS_ERROR=ON")
			fi
		fi
		cmake -S "$source_dir" -B "$build_dir" -G Ninja \
			-DBOARD=esp32p4c5_core_board/esp32p4/hpcore \
			-DBOARD_ROOT="$ROOT/firmware" \
			-DEXTRA_CONF_FILE="$conf_file" "${board_args[@]}" \
			-DZEPHYR_MODULES="$modules"
		cmake --build "$build_dir" --parallel 2
		;;
esac
