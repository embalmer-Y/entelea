# firmware/ · 构建与测试说明（M0）

> west 工作区在本仓库之外（`D:\Software\project\zephyrproject`，Zephyr **v4.4.0** 钉版，DEC-19）。
> 本模块经 `ZEPHYR_EXTRA_MODULES` 接入构建（后续迁入自管 west manifest，见 `docs/std/versioning.md` §4）。

## 构建（native_sim —— 需主机 gcc，Windows 原生暂缺，见 AGENTS.md 环境缺口）

```bash
cd D:/Software/project/zephyrproject
.venv/Scripts/west build -b native_sim D:/Software/project/Tessera/firmware/app \
  -d build-m0-app --pristine=always -- \
  -DZEPHYR_EXTRA_MODULES=D:/Software/project/Tessera/firmware/module/tessera
```

## 构建（真机板，SDK 交叉工具链，无需主机 gcc）

```bash
.venv/Scripts/west build -b nucleo_h743zi D:/Software/project/Tessera/firmware/app \
  -d build-m0-h7 --pristine=always -- \
  -DZEPHYR_EXTRA_MODULES=D:/Software/project/Tessera/firmware/module/tessera
```

## twister 冒烟测试

```bash
.venv/Scripts/west twister -p native_sim -T D:/Software/project/Tessera/firmware/tests \
  --extra-args=ZEPHYR_EXTRA_MODULES=D:/Software/project/Tessera/firmware/module/tessera
# 真机板构建验证（不运行）：-p nucleo_h743zi --build-only
```

## pytest 仓库检查（编码/无 BOM）

```bash
python -m pytest firmware/tests/pytest -v
```
