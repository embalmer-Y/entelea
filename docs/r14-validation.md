# r14 验证记录：TF 脚位对拍定案 + estop 真机绑定（GPIO20）

日期：2026-11-09。范围：①TF SDMMC dts 脚位问题全链定案（含我的第一轮
误报更正）；②安全通道 estop 真机落地面（owner 裁决 GPIO20=J4-31）。
板级安全纪律不变：estop 后端唯一写路径 drivers.c（source-policy 保护门）。

## 1. TF dts 事件全记录（诚实链）

- **起因（误报）**：查 estop 空闲 IO 时顺带读底板 TF 区，判定 dts
  `clk-pin=44/cmd-pin=43` 与原理图反了（600DPI 直读 pin3 CMD←GPIO44、
  pin5 CLK←GPIO43），核心板 88-pin 表（GPIO43=SD1_CCLK_PAD/GPIO44=
  SD1_CCMD_PAD）看似佐证。
- **真机对拍（推翻误报）**：tf_probe 应用三轮真机（同一 app 仅 dts 差，
  卡=16GB SD/SDMMC ver4，DET 先判在位）：
  1. v1 换前值（clk=44/cmd=43）：**PASS**
  2. v2 互换值（clk=43/cmd=44）：**PASS**
  3. v3 六属性全删（NC 复现 overlay）：**INIT_FAIL rc=-134(ENOTSUP)**
  4. 终版恢复（43/44/39-42）：**DET=1 + PASS**
- **定案（驱动源码+三轮互证）**：P4 SDMMC slot0 无 GPIO matrix
  （esp_hal_sd sdmmc_ll：仅 slot1 支持）→ `configure_pin_iomux` 对
  属性给出的焊盘集合套 SDMMC func0；**焊盘角色由 func0 硬连线决定**
  （CLK=GPIO43/CMD=GPIO44/D3..D0=GPIO42..39，与原理图、焊盘复用表
  一致）。即：**属性决定"使能哪组焊盘"（删=ENOTSUP），clk/cmd 语义
  标签不路由信号（互换=同组焊盘=同行为）**。原 dts 从无功能错误；
  语义标签已改与物理一致并写明机制（防后人再踩）。
- **DET=GPIO45 极性实证**：插卡=开关断开=外部 10K 上拉为高=逻辑 1
  （ACTIVE_HIGH）。binding 无 cd-gpios 面，应用层读（tf_probe）。
- 板级终态：`sdhc0` 六属性保留（43/44/39-42）+ 机制注释；TF 从未上真
  机的历史缺口就此补上（含卡在位/初始化正路径）。

## 2. estop 真机绑定（GPIO20）

- **引脚裁决**：owner 2026-11-09 选定 **GPIO20 = J4-31**（排针 2×17P
  J4 左列第 16 针）。零干涉排查：原理图全网络唯一消费者=J4-31；复用
  仅 ADC1_CH4；非 strap（P4 strap=GPIO34-38）/非 JTAG/非 USB PHY/
  非 SD/非触摸/无板上消费者；全库 grep 零占用。
- **绑定面迁移（附实测依据）**：原 aliases `ts-estop-gpio`→裸节点方案
  在 v4.5 不生成无 compatible 节点的 `gpios` phandle-array 宏（构建
  实证）；改绑 `/zephyr,user` 具名属性 `estop-gpios`（类型推断，零
  binding 依赖）。contract.md 同步。
- **drivers.c**：estop init 增补 `gpio_pin_configure_dt(INPUT)` 前置
  （pull-up 经 overlay dt_flags：ACTIVE_LOW|PULL_UP——按下=逻辑上升
  沿=缺省边沿映射触发）；观测面 getter `entelea_safety_estop_latched()/
  _latched_at()`（原子读；clear 释放闩、保留最近触发时刻=事实记录，
  safety 用例已断言）。
- **minimal 应用**：overlay 绑 GPIO20；5s 心跳打印安全汇总+estop 闩
  （闸判据行 `[entelea] safety: ... estop=LATCHED at=<ms>`）。
- **当前板级状态**：minimal 常驻镜像（estop=clear 心跳，证据
  build/r14-evidence/minimal-estop-gpio20-heartbeat.log）。**按压实测
  待 owner 接线后执行**（见 §4）。

## 3. 复现

```sh
# native_sim 全量（15 ztest + wdt_hardware 常驻 + CTest 6/6×2 + host）
for t in affect assist core delivery media memory net pkg r04 r13 safety store wasm wdt_start; do
  python3 tools/build_zephyr.py --app platform/zephyr/tests/$t \
    --board native_sim --zephyr-base ../entelea-legacy/.deps/zephyr \
    --python ../entelea-legacy/.venv/bin/python3 --build-dir build/r14-$t --build
  ./build/r14-$t/zephyr/zephyr.exe
done
ctest --test-dir build/r01 --output-on-failure
ctest --test-dir build/r01-gcc-sanitized --output-on-failure
sh build-support/build-linux-host.sh

# 板级 TF 对拍（对拍 overlay 见本节注）
python3 tools/build_zephyr.py --app platform/zephyr/apps/tf_probe \
  --board esp32p4c5_core_board/esp32p4/hpcore --board-root platform/zephyr \
  --zephyr-base ../entelea-legacy/.deps/zephyr \
  --python ../entelea-legacy/.venv/bin/python3 \
  --esptool-python ../entelea-legacy/.venv/bin/python3 \
  --sdk /home/emb/zephyr-sdk-1.0.1 --sdk-version 1.0.1 \
  --toolchain riscv64-zephyr-elf --gcc-version 14.3.0 \
  --module hal_espressif=../entelea-legacy/.deps/hal_espressif \
  --cmake-arg=-DESP_IDF_PATH=/home/emb/project/entelea-legacy/.deps/hal_espressif \
  --build-dir build/r14-tf-final --build
build/.toolbin/esptool --chip esp32p4 -p /dev/ttyACM0 -b 921600 \
  --before default_reset --after hard_reset write_flash \
  --flash_mode dio --flash_freq 80m --flash_size 16MB \
  0x2000 build/r14-tf-final/zephyr/zephyr.bin
```

对拍 overlay（v1 互换值：`&sdhc0 { clk-pin=<44>; cmd-pin=<43>; };`；
v3 NC：六属性各 `<0xffffffff>`；均经 `--cmake-arg=-DEXTRA_DTC_OVERLAY_FILE=`
注入）。证据：build/r14-evidence/tf-v{1,2,3nc}-*.log、tf-final-DET1-PASS.log。

回归面（本批全绿）：15 ztest native_sim + wdt_hardware 常驻（banner+
timeout）+ CTest 6/6×2 + Linux host + check_migration(800 sources) +
check_dependencies + source-policy（69→70，tf_probe/main.c 入清单）。

## 4. 待 owner 到场项（estop 按压实测）

接线：动合按键（或导线碰触）一端 **J4-31（GPIO20）**，另一端 **GND**
（J4-1/2/3/4 任一）。接好并告知后：监听串口（115200，EN 复位），按压
即应出现 `estop=LATCHED at=<ms>` 心跳行；松开保持 LATCHED（闩语义，
clear 命令面归后续批次）。
