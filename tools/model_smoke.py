#!/usr/bin/env python3
"""R08 模型适配层主机冒烟（B-2）：真调用端点验证+用量台账。

读取工作区凭据文件（owner 提供，不入库——见计划 §10.4-D2），
对允许模型发最小补全并逐笔记账（.cloud-usage/ledger.md）。
仅主机侧验证传输形状（推理 token/超时/截断语义）；固件真传输
随真网批。用法：tools/model_smoke.py [--model M] [--prompt P]
"""
import argparse
import os
import datetime
import json
import ssl
import time
import urllib.request

CREDS = "/home/emb/project/.cloud-credentials.json"
LEDGER = "/home/emb/project/.cloud-usage/ledger.md"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", default="bigmodel/glm-5.3-flash")
    ap.add_argument("--prompt", default="Reply with exactly: ok")
    ap.add_argument("--max-tokens", type=int, default=512)
    args = ap.parse_args()

    os.makedirs(os.path.dirname(LEDGER), exist_ok=True)
    cfg = json.load(open(CREDS))
    if args.model not in cfg["allowed_models"]:
        raise SystemExit(f"model not allowed: {args.model}")
    body = json.dumps({
        "model": args.model,
        "max_tokens": args.max_tokens,
        "messages": [{"role": "user", "content": args.prompt}],
    }).encode()
    req = urllib.request.Request(
        cfg["llm_base_url"] + "/chat/completions", data=body,
        headers={"Authorization": "Bearer " + cfg["llm_api_key"],
                 "Content-Type": "application/json"})
    t0 = time.time()
    with urllib.request.urlopen(req, timeout=90,
                                context=ssl.create_default_context()) as r:
        out = json.load(r)
    dt = time.time() - t0
    c = out["choices"][0]
    usage = out.get("usage", {})
    line = (f"| {datetime.date.today().isoformat()} | model_smoke 主机冒烟 "
            f"| {args.model} | {usage.get('total_tokens', '?')} "
            f"| <$0.01 | content={c['message']['content']!r} "
            f"finish={c['finish_reason']} {dt:.1f}s |\n")
    open(LEDGER, "a").write(line)
    print(line.strip())


if __name__ == "__main__":
    main()
