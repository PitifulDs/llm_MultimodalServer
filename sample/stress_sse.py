#!/usr/bin/env python3
import argparse
import json
import os
import random
import socket
import ssl
import threading
import time
from urllib.parse import urlparse

def now():
    return time.strftime("%H:%M:%S")

def http_post_stream(url, payload, timeout=10.0, abort_after=None, out_path=None):
    """
    Minimal HTTP/1.1 POST client that reads streaming response.
    abort_after: seconds after which we close the socket early (simulate client disconnect)
    """
    u = urlparse(url)
    host = u.hostname
    port = u.port or (443 if u.scheme == "https" else 80)
    path = u.path + (("?" + u.query) if u.query else "")

    body = payload.encode("utf-8")
    req = (
        f"POST {path} HTTP/1.1\r\n"
        f"Host: {host}:{port}\r\n"
        "Content-Type: application/json\r\n"
        f"Content-Length: {len(body)}\r\n"
        "Connection: close\r\n"
        "\r\n"
    ).encode("utf-8") + body

    s = socket.create_connection((host, port), timeout=timeout)
    if u.scheme == "https":
        ctx = ssl.create_default_context()
        s = ctx.wrap_socket(s, server_hostname=host)

    s.settimeout(timeout)

    started = time.time()
    bytes_read = 0
    first_byte_t = None
    aborted = False
    ok = True
    err = ""

    try:
        s.sendall(req)

        while True:
            if abort_after is not None and (time.time() - started) >= abort_after:
                aborted = True
                try:
                    s.shutdown(socket.SHUT_RDWR)
                except Exception:
                    pass
                s.close()
                break

            chunk = s.recv(4096)
            if not chunk:
                break
            if first_byte_t is None:
                first_byte_t = time.time()
            bytes_read += len(chunk)
            if out_path:
                with open(out_path, "ab") as f:
                    f.write(chunk)

    except Exception as e:
        ok = False
        err = repr(e)
    finally:
        try:
            s.close()
        except Exception:
            pass

    dur = time.time() - started
    ttfb = (first_byte_t - started) if first_byte_t else None
    return {
        "ok": ok,
        "aborted": aborted,
        "bytes": bytes_read,
        "dur_s": dur,
        "ttfb_s": ttfb,
        "err": err,
    }

def worker(idx, args, results, lock):
    # 随机是否 abort
    abort_after = None
    if random.random() < args.abort_ratio:
        abort_after = random.uniform(args.abort_min, args.abort_max)

    payload = {
        "model": args.model,
        "max_tokens": args.max_tokens,
        "messages": [{"role": "user", "content": "Output many short tokens/words continuously. Keep going."}],
    }
    out_path = None
    if args.save_logs:
        os.makedirs(args.out_dir, exist_ok=True)
        out_path = os.path.join(args.out_dir, f"req_{idx}.bin")
        # 清空
        open(out_path, "wb").close()

    r = http_post_stream(args.url, json.dumps(payload), timeout=args.timeout, abort_after=abort_after, out_path=out_path)
    r["idx"] = idx
    r["abort_after_s"] = abort_after

    with lock:
        results.append(r)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", default="http://127.0.0.1:8080/v1/chat/completions?stream=true")
    ap.add_argument("--model", default="llama")
    ap.add_argument("--concurrency", type=int, default=20)
    ap.add_argument("--rounds", type=int, default=200)
    ap.add_argument("--abort-ratio", type=float, default=0.6)
    ap.add_argument("--abort-min", type=float, default=0.2)
    ap.add_argument("--abort-max", type=float, default=3.0)
    ap.add_argument("--timeout", type=float, default=10.0)
    ap.add_argument("--max-tokens", type=int, default=64)
    ap.add_argument("--save-logs", action="store_true")
    ap.add_argument("--out-dir", default="/tmp/sse_stress_py")
    args = ap.parse_args()

    results = []
    lock = threading.Lock()

    print(f"[{now()}] url={args.url}")
    print(f"[{now()}] model={args.model} concurrency={args.concurrency} rounds={args.rounds} abort_ratio={args.abort_ratio}")
    print()

    in_flight = []
    next_idx = 1

    while next_idx <= args.rounds or in_flight:
        # 补满并发
        while next_idx <= args.rounds and len(in_flight) < args.concurrency:
            t = threading.Thread(target=worker, args=(next_idx, args, results, lock), daemon=True)
            t.start()
            in_flight.append(t)
            next_idx += 1

        # 清理已结束线程
        alive = []
        for t in in_flight:
            if t.is_alive():
                alive.append(t)
        in_flight = alive
        time.sleep(0.02)

    # 统计
    ok = sum(1 for r in results if r["ok"])
    aborted = sum(1 for r in results if r["aborted"])
    failed = sum(1 for r in results if not r["ok"])
    avg_bytes = sum(r["bytes"] for r in results) / max(1, len(results))
    avg_dur = sum(r["dur_s"] for r in results) / max(1, len(results))

    print("===== Summary =====")
    print(f"total={len(results)} ok={ok} failed={failed} aborted={aborted}")
    print(f"avg_bytes={avg_bytes:.1f} avg_dur_s={avg_dur:.3f}")

    # 列出失败样本
    bad = [r for r in results if not r["ok"]]
    if bad:
        print("\nFailures (show up to 10):")
        for r in bad[:10]:
            print(f"  idx={r['idx']} err={r['err']}")
        print("\nIf failures are mostly timeouts or connection reset during abort, that's expected.")
        print("But if the server crashes, you'll see widespread connection refused / cannot connect.")

if __name__ == "__main__":
    main()
