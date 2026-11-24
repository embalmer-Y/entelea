#!/usr/bin/env bash
# D-3/D-4 工厂闭环闸驱动：native_sim 全链 → 板级烧录真机证据。
# 用法：[RUN=run004] [MODE=product|dev] gate_run.sh [nsim|board|all]（默认 nsim）
# board/all 另需 ENTELEA_HARDWARE_AUTH=1（使用前须获得所有者真机授权）。
# board 阶段烧录前会擦除设备的存储分区（0x120000 起 1 MB；owner 2026-11-24
# 规程，见 build-support/README.md“烧录与存储分区”）。
# RF1：product（默认）= 无自动确认，超时后槽持续受门拒；dev =
# FACTORY_DEV_AUTOCONFIRM=ON 执行夹具（无用户验收效力）。
# nsim 阶段两模式都跑；board 阶段按 MODE。
set -uo pipefail
ENTELEA="$(cd "$(dirname "$0")/../../.." && pwd)"
VENV=$ENTELEA/.venv/bin/python3
ZEPHYR=$ENTELEA/.deps/zephyrproject/zephyr
ESPTOOL=$ENTELEA/build/.toolbin/esptool
RUN="${RUN:-run004}"
MODE="${MODE:-product}"
PHASE="${1:-nsim}"
RC=0

expect_line() { # $1=mode
  [ "$1" = dev ] && echo "PASS: factory loop closed (DEV" ||
    echo "PASS: confirmation timed out; candidate remains gated"
}

mode_arg() { # $1=mode
  [ "$1" = dev ] && echo "-DFACTORY_DEV_AUTOCONFIRM=ON" ||
    echo "-DFACTORY_DEV_AUTOCONFIRM=OFF"
}

nsim_build() { # $1=build name $2=mode $3=stop-after-pending $4=log
  local build=$1 m=$2 stop=$3 log=$4
  cd "$ENTELEA" || exit 1
  python3 tools/build_zephyr.py \
    --app platform/zephyr/apps/hw_factory_intake --board native_sim \
    --zephyr-base "$ZEPHYR" --python "$VENV" \
    --build-dir "build/d3-gate-nsim-$build" --build \
    --cmake-arg="$(mode_arg "$m")" \
    --cmake-arg="-DFACTORY_TEST_STOP_AFTER_PENDING=$stop" \
    --cmake-arg=-DTEST_FLASH_BACKEND=ON \
    --cmake-arg=-DEXTRA_CONF_FILE=prj-flash.conf \
    --cmake-arg=-DCONF_FILE=$ENTELEA/platform/zephyr/apps/hw_factory_intake/prj.conf \
    --cmake-arg=-DFACTORY_PKG_HEADER=$ENTELEA/factory/forge/artifacts/$RUN/factory_pkg.h \
    > "$log" 2>&1 || { tail -20 "$log"; return 1; }
}

nsim() {
  nsim_mode product && nsim_mode dev && nsim_restart
}

nsim_mode() { # $1=mode
  local m=$1
  local run_dir
  run_dir="$(mktemp -d "${TMPDIR:-/tmp}/entelea-gate-${m}-XXXXXXXX")" || return 1
  echo "== [gate] native_sim sim-flash 全链 ($m) =="
  nsim_build "$m" "$m" OFF "$run_dir/build.log" || return 1
  local run_rc=0
  (cd "$run_dir" && timeout 60 "$ENTELEA/build/d3-gate-nsim-$m/zephyr/zephyr.exe" -stop_at=20) \
    > "$run_dir/run.log" 2>&1 || run_rc=$?
  local out
  out=$(cat "$run_dir/run.log")
  echo "[gate] evidence: $run_dir"
  echo "$out" | grep "\[hw-fact\]"
  if [ "$run_rc" -ne 0 ]; then
    echo "FAIL: simulator exited with status $run_rc" >&2
    return 1
  fi
  echo "$out" | grep -qF "$(expect_line "$m")" && \
    ! echo "$out" | grep -q "FAIL:" || return 1
}

nsim_restart() {
  local run_dir
  run_dir="$(mktemp -d "${TMPDIR:-/tmp}/entelea-gate-restart-XXXXXXXX")" || return 1
  echo "== [gate] native_sim pending candidate survives restart =="
  echo "[gate] evidence: $run_dir"
  nsim_build pending product ON "$run_dir/build.log" || return 1
  (cd "$run_dir" && timeout 60 "$ENTELEA/build/d3-gate-nsim-pending/zephyr/zephyr.exe" -stop_at=20) \
    > "$run_dir/first.log" 2>&1 || return 1
  grep -qF "TEST: stopping after persisted PENDING" "$run_dir/first.log" || return 1
  ! grep -qF "FAIL:" "$run_dir/first.log" || return 1
  local run_rc=0
  (cd "$run_dir" && timeout 60 "$ENTELEA/build/d3-gate-nsim-product/zephyr/zephyr.exe" -stop_at=20) \
    > "$run_dir/second.log" 2>&1 || run_rc=$?
  if [ "$run_rc" -ne 0 ] && {
    [ "$run_rc" -ne 1 ] ||
      ! grep -qF "sys_arch_reboot called with type 1" "$run_dir/second.log"
  }; then
    echo "FAIL: unexpected second-boot status $run_rc" >&2
    return 1
  fi
  grep -qF "FAIL: slot not occupied by a pending or active candidate" "$run_dir/second.log" || return 1
  [ "$(grep -cF '[hw-fact] FAIL:' "$run_dir/second.log")" -eq 1 ] || return 1
  ! grep -qF "stage factory package" "$run_dir/second.log" || return 1
  echo "PASS: persisted PENDING blocks staging on the next boot"
}

board() {
  if [ "${ENTELEA_HARDWARE_AUTH:-}" != 1 ]; then
    echo "FAIL: board access requires owner authorization and ENTELEA_HARDWARE_AUTH=1" >&2
    return 1
  fi
  local run_dir
  run_dir="$(mktemp -d "${TMPDIR:-/tmp}/entelea-gate-board-XXXXXXXX")" || return 1
  echo "== [gate] 板级构建+擦存储分区+烧录+监听 =="
  cd "$ENTELEA" || exit 1
  python3 tools/build_zephyr.py \
    --app platform/zephyr/apps/hw_factory_intake \
    --board esp32p4c5_core_board/esp32p4/hpcore \
    --board-root platform/zephyr \
    --zephyr-base "$ZEPHYR" --python "$VENV" \
    --esptool-python "$VENV" \
    --sdk "$ENTELEA/.deps/zephyr-sdk-1.0.1" --sdk-version 1.0.1 \
    --toolchain riscv64-zephyr-elf --gcc-version 14.3.0 \
    --module hal_espressif=$ENTELEA/.deps/zephyrproject/modules/hal/espressif \
    --cmake-arg=-DESP_IDF_PATH=$ENTELEA/.deps/zephyrproject/modules/hal/espressif \
    --build-dir build/d3-gate-board --build \
    --cmake-arg="$(mode_arg "$MODE")" \
    --cmake-arg=-DFACTORY_PKG_HEADER=$ENTELEA/factory/forge/artifacts/$RUN/factory_pkg.h \
    > "$run_dir/build.log" 2>&1 || { tail -20 "$run_dir/build.log"; return 1; }
  # 产品镜像落板之前先擦存储分区（owner 2026-11-24；规程见 build-support/README.md
  # “烧录与存储分区”）。分区里留着别的固件的数据时启动能过，但试用状态与 KV
  # 会按失效保护拒绝，本闸第一项检查就会失败。擦除清掉设备上的持久状态。
  "$ESPTOOL" --chip esp32p4 -p /dev/ttyACM0 -b 921600 \
    erase-region 0x120000 0x100000 \
    > "$run_dir/erase.log" 2>&1 || { tail -10 "$run_dir/erase.log"; return 1; }
  "$ESPTOOL" --chip esp32p4 -p /dev/ttyACM0 -b 921600 \
    --before default_reset --after hard_reset \
    write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB \
    0x2000 build/d3-gate-board/zephyr/zephyr.bin \
    > "$run_dir/flash.log" 2>&1 || { tail -10 "$run_dir/flash.log"; return 1; }
  # 监听 45s 抓闸证据行（EN 硬复位后）
  timeout 45 "$VENV" - << 'PYEOF' > "$run_dir/run.log" 2>&1 || true
import serial, time
s = serial.Serial("/dev/ttyACM0", 115200, timeout=1)
s.dtr = False; s.rts = False; time.sleep(0.1)
s.rts = True; time.sleep(0.1); s.rts = False   # EN 硬复位
t0 = time.time()
buf = b""
while time.time() - t0 < 40:
    buf += s.read(4096)
    if (b"factory loop closed" in buf or b"confirmation timed out" in buf
            or b"FAIL:" in buf):
        time.sleep(0.5); buf += s.read(4096)
        break
s.close()
print(buf.decode("utf-8", "replace"))
PYEOF
  local out
  out=$(cat "$run_dir/run.log")
  echo "[gate] evidence: $run_dir"
  echo "$out" | grep "\[hw-fact\]" | head -20
  echo "$out" | grep -qF "$(expect_line "$MODE")" && \
    ! echo "$out" | grep -q "FAIL:" || return 1
}

case "$PHASE" in
  nsim) nsim || RC=1 ;;
  board) board || RC=1 ;;
  all) nsim || RC=1; board || RC=1 ;;
  *) echo "usage: $0 [nsim|board|all]"; exit 2 ;;
esac
[ "$RC" = 0 ] && echo "== [gate] ALL PASS ==" || echo "== [gate] FAIL (rc=$RC) =="
exit $RC
