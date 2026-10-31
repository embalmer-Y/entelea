# SPDX-License-Identifier: Apache-2.0
"""L3 端到端联调客户端（LLD-ts-net §8-L3；M3a.2 记录，DEC-40/41/42 批扩展）。

前置：
  1. zenohd 运行：~/project/tools/zenohd --listen tcp/0.0.0.0:7447
  2. 固件运行：build-l3/zephyr/zephyr.exe --eth-if=zeth（TAP host 侧 192.0.2.2）
用法：agent-venv python firmware/tests/netlab/netlab_client.py
退出码 0 = 全部 L3 验证点通过；输出 JSON 结论。

验证点（LLD §8）：
  ① 发现：cube→host 心跳（sys/hb）与遥测（l3led/telemetry，信封 ver=1/kind=96）到达
  ② 命令-回执：v1 get-info 共存 + v2 信封（rid 回带/kind=16）；estop-clear
     错误令牌 = status=TS_E_PARAM(-1)（确认令牌硬点）
  ③ 幂等（DEC-40）：同 idem 不同参数重发 = 字节级同回执且未重执行（墙钟不变）；
     to=6000 = TS_E_PARAM 拒绝
  ④ 租约（DEC-41）：acquire → 续期幂等（同 id）→ 他人获取拒绝（TS_E_STATE=-4）
     → 他人归还拒绝 → 本人归还 OK → get 无租约
  ⑤ 心跳保持与断链判定：持续 hb-host → link_up=true（通道恢复 ACTIVE 可写）；
     停发 >6 周期（10s）→ link_up=false 且通道进 SAFE_LINKLOSS（get-safety 计数）
"""
from __future__ import annotations

import json
import sys
import time

import cbor2
import zenoh

Z = "tessera/l3n/l3c"
TS_E_PARAM = -1
TS_E_STATE = -4


def tstr(s: str) -> bytes:
    b = s.encode()
    assert len(b) < 24
    return bytes([0x60 | len(b)]) + b


def uint(v: int) -> bytes:
    """cbor_min 确定性子集 uint 编码（<24 单字节；1/2/4/8B 定长映射 24/25/26/27）."""
    if v < 24:
        return bytes([v])
    for n, ai in ((1, 24), (2, 25), (4, 26), (8, 27)):
        if v < (1 << (8 * n)):
            return bytes([ai]) + v.to_bytes(n, "big")
    raise ValueError(v)


def req(op: str) -> bytes:
    """v1 请求 map{"op": op}（弃用期共存验证）。"""
    return bytes([0xA1]) + tstr("op") + tstr(op)


def req2(op: str, rid: str, arg: tuple[str, str | int] | None = None,
         idem: str | None = None, to: int | None = None) -> bytes:
    """v2 请求 map{ver,kind,rid,src,op,args?{arg?,idem?,to?}}（DEC-40；
    args 内定序：arg → idem → to，与固件 cbor_min 解码序无关但编码确定性）。"""
    args_items: list[tuple[bytes, bytes]] = []
    if arg is not None:
        k, v = arg
        args_items.append((tstr(k), tstr(v) if isinstance(v, str) else uint(v)))
    if idem is not None:
        args_items.append((tstr("idem"), tstr(idem)))
    if to is not None:
        args_items.append((tstr("to"), uint(to)))
    out = bytes([0xA5 if not args_items else 0xA6]) \
        + tstr("ver") + uint(1) + tstr("kind") + uint(1) \
        + tstr("rid") + tstr(rid) + tstr("src") + tstr("l3-client") + tstr("op") + tstr(op)
    if args_items:
        out += tstr("args") + bytes([0xA0 | len(args_items)])
        for k, v in args_items:
            out += k + v
    return out


def query(s, cmd: str, payload: bytes) -> tuple[int, dict, dict]:
    """sys 命令 query → (status, data, reply_map)。回执 v1/v2 由键集判别。"""
    replies = s.get(f"{Z}/sys/{cmd}", payload=payload)
    for r in replies:
        if r.ok is None:
            raise RuntimeError(f"query {cmd} replied ERR: {r.err_payload}")
        m = cbor2.loads(bytes(r.ok.payload))
        data = m.get("data") if isinstance(m.get("data"), dict) else {}
        return int(m["status"]), data, m
    raise RuntimeError(f"query {cmd}: no reply")


def main() -> int:
    conf = zenoh.Config()
    conf.insert_json5("connect", json.dumps({"endpoints": ["tcp/127.0.0.1:7447"]}))
    s = zenoh.open(conf)

    seen: dict[str, int] = {}
    last_payloads: dict[str, bytes] = {}

    def on_sample(sample) -> None:
        k = str(sample.key_expr)
        seen[k] = seen.get(k, 0) + 1
        last_payloads[k] = bytes(sample.payload)

    sub = s.declare_subscriber("tessera/**", handler=on_sample)

    out: dict[str, object] = {}

    # ① 发现：等心跳与遥测到达（固件连接 + 200ms 快照 + 1s 心跳）
    deadline = time.time() + 15
    while time.time() < deadline and not (
        seen.get(f"{Z}/sys/hb") and seen.get(f"{Z}/l3led/telemetry")
    ):
        time.sleep(0.2)
    out["hb_arrived"] = seen.get(f"{Z}/sys/hb", 0)
    out["telem_arrived"] = seen.get(f"{Z}/l3led/telemetry", 0)
    # 遥测信封（DEC-42）：map{ver:1, kind:96, ...}
    telem_env_ok = False
    if last_payloads.get(f"{Z}/l3led/telemetry"):
        t = cbor2.loads(last_payloads[f"{Z}/l3led/telemetry"])
        telem_env_ok = t.get("ver") == 1 and t.get("kind") == 96
    out["telem_env_ok"] = telem_env_ok
    if not out["hb_arrived"] or not out["telem_arrived"] or not telem_env_ok:
        print(json.dumps({"FAIL": "discovery", **out}, ensure_ascii=False, default=str))
        return 1

    # ② 命令-回执：v1 共存 + v2 信封
    st_v1, d_v1, m_v1 = query(s, "get-info", req("get-info"))
    st_info, d_info, m_info = query(s, "get-info", req2("get-info", "rid-i1"))
    st_link, d_link, _ = query(s, "get-link", req2("get-link", "rid-i2"))
    st_safety, _, _ = query(s, "get-safety", req2("get-safety", "rid-i3"))
    bad = bytes([0xA2]) + tstr("op") + tstr("estop-clear") + tstr("args") + \
        bytes([0xA1]) + tstr("confirm") + tstr("stop")
    st_estop, _, m_estop = query(s, "estop-clear", req2("estop-clear", "rid-e1",
                                                        arg=("confirm", "stop")))
    out.update(
        v1_get_info_status=st_v1, v1_keys=sorted(m_v1.keys()),
        v2_rid_echo=m_info.get("rid"), v2_ver=m_info.get("ver"), v2_kind=m_info.get("kind"),
        get_info_status=st_info, fw=d_info.get("fw"), board=d_info.get("board"),
        get_link_status=st_link, session_state=d_link.get("state"),
        get_safety_status=st_safety, estop_clear_wrong_token=st_estop,
        estop_rid_echo=m_estop.get("rid"),
    )
    ok2 = (
        st_v1 == 0 and sorted(m_v1.keys()) == ["data", "status"]
        and m_info.get("rid") == "rid-i1" and m_info.get("ver") == 1
        and m_info.get("kind") == 16
        and st_info == 0 and st_link == 0 and st_safety == 0
        and st_estop == TS_E_PARAM and m_estop.get("rid") == "rid-e1"
    )
    if not ok2:
        print(json.dumps({"FAIL": "commands", **out}, ensure_ascii=False, default=str))
        return 1

    # ③ 幂等（DEC-40）：同 idem 不同 time_ms 重发 = 回放原回执，墙钟不变
    t1, t2 = 1700000000000, 1700000099999
    p1 = req2("set-time", "rid-s1", arg=("time_ms", t1), idem="idem-l3-1")
    p2 = req2("set-time", "rid-s1", arg=("time_ms", t2), idem="idem-l3-1")
    st_1, d_1, _ = query(s, "set-time", p1)
    _, _, m_2 = query(s, "set-time", p2)
    st_to, _, _ = query(s, "get-info", req2("get-info", "rid-t1", to=6000))
    st_1b, _, m_1 = query(s, "set-time", p1)  # 命中缓存 → 原样回放
    _, d_wall, _ = query(s, "get-info", req2("get-info", "rid-w1"))
    out.update(
        idem_first_status=st_1, idem_first_wall=d_1.get("wall_ms"),
        idem_replay_wall=m_2.get("data", {}).get("wall_ms"),
        idem_cached_wall=m_1.get("data", {}).get("wall_ms"),
        to_6000_status=st_to, wall_after_replay=d_wall.get("wall_ms"),
    )
    # 墙钟 = 基准 + 流逝单调时间：重放后应在 t1 附近小窗内（且绝非 t2——未重执行）
    wall_now = d_wall.get("wall_ms")
    ok3 = (
        st_1 == 0 and st_1b == 0 and d_1.get("wall_ms") == t1
        and m_2.get("data", {}).get("wall_ms") == t1
        and m_1.get("data", {}).get("wall_ms") == t1
        and wall_now is not None and t1 <= wall_now < t1 + 60000
        and st_to == TS_E_PARAM
    )
    if not ok3:
        print(json.dumps({"FAIL": "idempotency", **out}, ensure_ascii=False, default=str))
        return 1

    # ④ 租约（DEC-41）：获取/续期幂等/他人拒绝/他人归还拒/本人归还
    st_a, d_a, _ = query(s, "lease-acquire",
                         req2("lease-acquire", "rid-la", arg=("holder", "l3-agent")))
    st_a2, d_a2, _ = query(s, "lease-acquire",
                           req2("lease-acquire", "rid-la2", arg=("holder", "l3-agent")))
    st_b, d_b, _ = query(s, "lease-acquire",
                         req2("lease-acquire", "rid-lb", arg=("holder", "intruder")))
    st_rel_x, _, _ = query(s, "lease-release",
                           req2("lease-release", "rid-lx", arg=("holder", "intruder")))
    st_rel, _, _ = query(s, "lease-release",
                         req2("lease-release", "rid-lr", arg=("holder", "l3-agent")))
    st_get, d_get, _ = query(s, "lease-get", req2("lease-get", "rid-lg"))
    out.update(
        lease_acquire=st_a, lease_id=d_a.get("lease_id"),
        lease_renew_id=d_a2.get("lease_id"),
        lease_intruder=st_b, intruder_holder=d_b.get("holder"),
        lease_release_intruder=st_rel_x, lease_release=st_rel,
        lease_get_after=st_get, lease_valid_after=d_get.get("valid"),
    )
    ok4 = (
        st_a == 0 and d_a.get("lease_id", 0) >= 1
        and st_a2 == 0 and d_a2.get("lease_id") == d_a.get("lease_id")
        and st_b == TS_E_STATE and d_b.get("holder") == "l3-agent"
        and st_rel_x == TS_E_STATE and st_rel == 0
        and st_get == 0 and d_get.get("valid") in (False, 0, "False")
    )
    if not ok4:
        print(json.dumps({"FAIL": "lease", **out}, ensure_ascii=False, default=str))
        return 1

    # ⑤ 心跳保持 → link_up（滞回 2 次即恢复）
    for _ in range(8):  # 8 × 0.4s ≈ 3.2s
        s.put(f"{Z}/sys/hb-host", b"hb")
        time.sleep(0.4)
    time.sleep(0.5)
    _, d2, _ = query(s, "get-link", req2("get-link", "rid-h1"))
    out["link_up_after_hb"] = d2.get("link_up")
    _, ds2, _ = query(s, "get-safety", req2("get-safety", "rid-h2"))
    out["active_after_hb"] = ds2.get("active")

    # 断链：停发 10s（> 6 周期 × 1s）→ link_up=false + 通道 LINKLOSS
    time.sleep(10)
    _, d3, _ = query(s, "get-link", req2("get-link", "rid-h3"))
    _, ds3, _ = query(s, "get-safety", req2("get-safety", "rid-h4"))
    out["link_up_after_starve"] = d3.get("link_up")
    out["linkloss_after_starve"] = ds3.get("linkloss")
    # 断链迁移产生 SAFE_STATE_CHANGED 事件（DEC-42 信封 kind = 32+evt_id = 37）
    evt = last_payloads.get(f"{Z}/l3led/event")
    evt_kind = None
    if evt:
        e = cbor2.loads(evt)
        evt_kind = e.get("kind")
        out["evt_env"] = {"ver": e.get("ver"), "kind": e.get("kind")}
    out["evt_kind_is_state_changed"] = evt_kind == 32 + 5  # TS_EVT_SAFE_STATE_CHANGED

    def as_bool(v: object) -> bool:
        if isinstance(v, bool):
            return v
        return getattr(v, "value", None) == 1 or str(v) in ("1", "True", "true")

    ok5 = (
        as_bool(out["link_up_after_hb"])
        and out["active_after_hb"] == 1
        and not as_bool(out["link_up_after_starve"])
        and out["linkloss_after_starve"] == 1
        and out["evt_kind_is_state_changed"]
    )
    out["result"] = "PASS" if ok5 else "FAIL"
    print(json.dumps(out, ensure_ascii=False, default=str))
    sub.undeclare()
    s.close()
    return 0 if ok5 else 1


if __name__ == "__main__":
    sys.exit(main())
