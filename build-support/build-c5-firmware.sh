#!/bin/bash
# C5 协处理器 slave 固件官方自编译（F-1；复现链=build-support/c5-firmware.json）。
# 已验证流程（2026-11-09 真机 ALL PASS）：
#  1) 源/工具链 pin 校验（dependencies.json esp_hosted_mcu；IDF v5.5.4）
#  2) 官方 patch-idf（SDIO 4092 上限解除——SW 聚合前提；幂等）
#  3) cp 工程构建（EXTRA_COMPONENT_DIRS 经命名符号链接 .eh-comp/esp_hosted
#     桥接——IDF 组件发现要求目录名=组件名；transport=SDIO 为 c5 缺省）
#  4) 四件套产物→deps/c5-firmware-dist/ + sha256 回写 c5-firmware.json
# 烧录（switch key C5 侧；见 c5-firmware.json flash_offsets_note）。
set -e
DEPS=/home/emb/project/deps
EH=$DEPS/esp-hosted-mcu
IDF=$DEPS/esp-idf
DIST=$DEPS/c5-firmware-dist
PROJ=$EH/examples/wifi/sta/cp
BRIDGE=$DEPS/.eh-comp/esp_hosted

cd "$(dirname "$0")/.."
python3 tools/check_dependencies.py > /dev/null
ACTUAL=$(git -C "$EH" rev-parse HEAD)
WANT=$(python3 -c "import json;print([d['commit'] for d in json.load(open('build-support/dependencies.json'))['dependencies'] if d['id']=='esp_hosted_mcu'][0])")
[ "$ACTUAL" = "$WANT" ] || { echo "FAIL: esp_hosted_mcu checkout drifted ($ACTUAL)"; exit 1; }

# 官方 SDIO cap patch（幂等——已打则 no-op）
/home/emb/.espressif/python_env/idf5.5_py3.14_env/bin/python \
  "$EH/tools/eh.py" patch-idf --idf-path "$IDF" 2>&1 | grep -q "successful\|already" \
  || { echo "FAIL: patch-idf"; exit 1; }

mkdir -p "$(dirname "$BRIDGE")"
ln -sfn "$EH" "$BRIDGE"

. "$IDF/export.sh" > /dev/null
cd "$PROJ"
rm -rf build sdkconfig dependencies.lock
idf.py -DEXTRA_COMPONENT_DIRS="$(dirname "$BRIDGE")" set-target esp32c5 > /dev/null
idf.py -DEXTRA_COMPONENT_DIRS="$(dirname "$BRIDGE")" build

mkdir -p "$DIST"
for f in bootloader/bootloader.bin partition_table/partition-table.bin \
         ota_data_initial.bin eh_cp_wifi_sta.bin; do
  cp "build/$f" "$DIST/"
done
cd - > /dev/null
python3 - << 'PYEOF'
import hashlib, json
from pathlib import Path
dist = Path("/home/emb/project/deps/c5-firmware-dist")
m = json.loads(Path("build-support/c5-firmware.json").read_text())
for key, fname in [("bootloader","bootloader.bin"),("partition-table","partition-table.bin"),
                   ("ota_data_initial","ota_data_initial.bin"),("eh_cp_wifi_sta_app","eh_cp_wifi_sta.bin")]:
    m["firmware"]["artifacts"][key]["sha256"] = hashlib.sha256((dist/fname).read_bytes()).hexdigest()
Path("build-support/c5-firmware.json").write_text(
    json.dumps(m, indent=2, ensure_ascii=False) + "\n")
print("[c5-fw] artifacts hashed -> c5-firmware.json")
PYEOF
echo "[c5-fw] built in $DIST"
