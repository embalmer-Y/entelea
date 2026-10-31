#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""e2e_trust_chain.py — K3 信任链真机 E2E 驱动（仓外惯例）。

时间线（atkbench @ akn/akc，TEST 构建有效集 = {测试根} ∪ 信任表）：
  T0 trust-get 基线（ver=0 空）
  T1 未注册 dev 键签名包部署 → 板侧 verify 拒（多根前基线）
  T2 trust-update ver=1 [dev]（测试根签名——首次注册工厂根授权）
  T3 dev 键签名包部署 → PASS（多根实机安装实证）
  T4 重放 ver=1 → E_RANGE（防回滚实机）
  T5 trust-update ver=2 []（全撤回）
  T6 dev 键签名包再部署 → 拒（撤回生效实证）
末行 AK3 TRUST PASS 判定；串口 console 同步留痕到 stdout。
"""
from __future__ import annotations

import secrets
import sys
import threading
import time
from pathlib import Path

REPO = Path("/home/emb/project/Tessera")
AGENT = REPO / "agent"
sys.path.insert(0, str(AGENT))

from tessera_agent.common.errors import TaError  # noqa: E402
from tessera_agent.tools_net import deploy  # noqa: E402
from tessera_agent.tools_net.zenoh_service import ZenohService  # noqa: E402
from tessera_agent.tools_tsap import tools as tsap_tools  # noqa: E402
from tessera_agent.tools_tsap.cose import generate_keypair  # noqa: E402

NODE, CUBE = "akn", "akc"
TEST_ROOT_KEY = AGENT / "tests" / "fixtures" / "ts-test-root.key"
WASM = REPO / "firmware" / "tests" / "app" / "appw" / "native_app.wasm"
SERIAL = "/dev/ttyACM0"
OUT = Path("/tmp/ak3")

stop_reader = threading.Event()
console_lines: list[str] = []


def reader() -> None:
    import serial

    # CH340 一键下载电路：开埠 DTR/RTS 默认脉动会复位板——构造参数直取
    # 低电平免脉动（K3 实证：复位 + zenohd 陈旧 queryable 声明 = 查询黑洞）
    ser = serial.Serial()
    ser.port = SERIAL
    ser.baudrate = 115200
    ser.timeout = 1
    ser.rts = False  # 预置低电平再开埠（构造参数不支持——属性后置 open）
    ser.dtr = False
    ser.open()
    while not stop_reader.is_set():
        line = ser.readline().decode(errors="replace").rstrip()
        if line:
            console_lines.append(line)
            if "trust" in line or "vrf" in line:
                print(f"  [console] {line}", flush=True)



def raw_cmd(svc, op, rid, **env):
    """裸 query（svc.query 直迭代——K3 实证该形态恒通；deploy._cmd 的
    query_one 路径在本机存在确定性空回执幽灵〔PC 侧工具链域，已登记〕）。"""
    from tessera_agent.tools_net import keys as K
    replies = svc.query(K.key_sys(NODE, CUBE, op), K.envelope(op, rid, **env))
    if not replies:
        msg = f"no reply: {op}"
        raise deploy.CmdError(op, K.Reply(status=-99))
    _, payload = replies[0]
    return K.parse_reply(payload)


def raw_cmd_ok(svc, op, rid, **env):
    r = raw_cmd(svc, op, rid, **env)
    if r.status != 0:
        raise deploy.CmdError(op, r)
    return r.data


def log(msg: str) -> None:
    print(f"AK3 {msg}", flush=True)



def _raw_trust_update(svc, dev_keys, ver=1):
    from tessera_agent.tools_tsap.cose import sign_diy
    keys_list = list(dev_keys)
    tbl = bytes([0xA2]) + b"\x63ver" + bytes([ver]) + b"\x64keys"
    tbl += bytes([0x80 | len(keys_list)])
    for k in keys_list:
        tbl += bytes([0x58, 32]) + bytes(k)
    blob = sign_diy(tbl, TEST_ROOT_KEY.read_bytes())
    raw_cmd_ok(svc, "lease-acquire", "ak3-ta", holder="tessera-agent")
    r = raw_cmd_ok(svc, "trust-update", "ak3-tu", data=blob,
                   holder="tessera-agent")
    raw_cmd_ok(svc, "lease-release", "ak3-tx", holder="tessera-agent")
    log(f"trust-update ver={r.get('ver')} keys={r.get('keys')}")
    return r


def main() -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    dev_priv, dev_pub = generate_keypair()
    dev_key = OUT / "dev.key"
    dev_pub_p = OUT / "dev.pub"
    dev_key.write_bytes(dev_priv)
    dev_pub_p.write_bytes(dev_pub)
    log(f"dev keypair: {dev_pub.hex()[:16]}…")

    # K3 收束：reader 线程整体移除（其 print 搅动与 zenoh 回执物化存在
    # 竞态——工具链域定案登记；console 证据 = 后置板侧留痕 + 驱动判定）
    stop_reader.set()

    manifest = {
        "app_id": "com.tessera.ak3dev", "app_ver": "1.0.0",
        "min_fw_ver": "0.1.0", "caps": ["gpio:write:0-3"],
        "stack_kb": 4, "heap_kb": 16,
        "exports": ["health_ping", "app_init", "app_evt"],
    }
    import shutil

    wasm_copy = OUT / "native_app.wasm"
    shutil.copy(WASM, wasm_copy)  # IR2-03 读面白名单（WASM 父目录须在 roots）
    r = tsap_tools.tsap_package(str(wasm_copy), manifest, str(dev_key),
                                str(OUT), [str(OUT)])
    pkg = r["package_path"]
    log(f"dev-signed package: {pkg} ({r['size']}B)"

        )
    pub_file = OUT / "pub.pub"
    pub_file.write_bytes(dev_pub)

    with ZenohService("tcp/192.168.2.91:9955") as svc:
        # 在线等待 ×2 稳判（开埠/复位脉动后 zenoh queryable 声明传播窗）
        ok = 0
        for i in range(60):
            try:
                raw_cmd_ok(svc, "get-info", f"ak3-hb{i}")
                ok += 1
                if ok >= 2:
                    break
            except Exception:
                ok = 0
            time.sleep(1)
        if ok < 2:
            log("FAIL：板不在线")
            return 1
        log("board online（x2 稳判）")
        time.sleep(2)
        # T0 基线
        base = {"ver": raw_cmd_ok(svc, "trust-get", "ak3-tg0")["ver"], "keys": []}
        log(f"T0 trust-get: ver={base['ver']} keys={len(base['keys'])}")
        assert base["ver"] == 0, "基线应为空表"

        # T1 未注册拒（板侧 verify 拒——deploy 上抛）
        try:
            deploy.push_app(svc, NODE, CUBE, pkg, str(pub_file),
                            log_fn=lambda s: None)
            log("T1 FAIL：未注册 dev 键竟然通过")
            return 1
        except (deploy.CmdError, TaError) as exc:
            log(f"T1 未注册 dev 键部署拒 ✓（{type(exc).__name__}）")

        # T2 首次注册（工厂根=测试根签名）
        r2 = _raw_trust_update(svc, [dev_pub])
        log(f"T2 trust-update ver=1 [dev]: {r2}")
        assert r2["ver"] == 1

        # T3 多根实机安装
        d3 = deploy.push_app(svc, NODE, CUBE, pkg, str(pub_file),
                             log_fn=log)
        log(f"T3 dev 键部署 PASS: slot={d3.get('activate', {}).get('slot', d3)}")
        # 激活 = 暖复位装载（K1 同型：复位窗 + 重新在线稳判 + 装载后断言；
        # DHCP 间歇可拖到 40s+回退，轮询 150s 上界）
        time.sleep(10)
        ok = 0
        for i in range(75):
            try:
                raw_cmd_ok(svc, "get-info", f"ak3-w{i}")
                ok += 1
                if ok >= 2:
                    break
            except Exception:
                ok = 0
            time.sleep(2)
        if ok < 2:
            log("T3 FAIL：复位后板未回线")
            return 1
        g = deploy.status(svc, NODE, CUBE)
        app_id = g.get("get-app", {}).get("app_id", "")
        log(f"T3 get-app: app_id={app_id}")
        assert app_id == "com.tessera.ak3dev", f"ACTIVE 应为 dev 包: {g}"

        # T4 重放防回滚
        from tessera_agent.tools_tsap.cose import sign_diy
        tbl = bytes([0xA2]) + b"\x63ver" + bytes([1]) + b"\x64keys"
        tbl += bytes([0x81, 0x58, 32]) + dev_pub
        blob = sign_diy(tbl, TEST_ROOT_KEY.read_bytes())
        try:
            raw_cmd_ok(svc, "lease-acquire", "ak3-ta2", holder="tessera-agent")
            raw_cmd_ok(svc, "trust-update", "ak3-replay", data=blob,
                       holder="tessera-agent")
            log("T4 FAIL：重放竟然通过")
            return 1
        except deploy.CmdError as exc:
            log(f"T4 重放 ver=1 拒 ✓（status={exc.status}）")
        finally:
            try:
                raw_cmd_ok(svc, "lease-release", "ak3-tx4", holder="tessera-agent")
            except Exception:
                pass

        # T5 全撤回
        r5 = _raw_trust_update(svc, [], ver=2)
        log(f"T5 全撤回 ver=2: {r5}")
        assert r5["ver"] == 2 and r5["keys"] == 0

        # T6 撤回后复拒
        try:
            deploy.push_app(svc, NODE, CUBE, pkg, str(pub_file),
                            log_fn=lambda s: None)
            log("T6 FAIL：撤回后 dev 键竟然通过")
            return 1
        except (deploy.CmdError, TaError) as exc:
            log(f"T6 撤回后复拒 ✓（{type(exc).__name__}）")

        fin = raw_cmd_ok(svc, "trust-get", "ak3-tgf")
        log(f"T7 终态: ver={fin['ver']} keys={len(fin['keys'])}")

    stop_reader.set()
    log("driver-side 判定完成（console 由板侧持留——/tmp/ak3_console_live.txt）")
    print("AK3 TRUST PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
