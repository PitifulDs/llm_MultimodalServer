#!/usr/bin/env python3
import json
import os
import sys
import time
import urllib.error
import urllib.request


BASE_URL = os.environ.get("BASE_URL", "http://localhost:8080").rstrip("/")
MODEL = os.environ.get("MODEL", "qwen3.5-2b")
TIMEOUT = float(os.environ.get("TIMEOUT", "120"))
CONNECT_RETRIES = int(os.environ.get("CONNECT_RETRIES", "30"))
CONNECT_RETRY_INTERVAL = float(os.environ.get("CONNECT_RETRY_INTERVAL", "0.5"))


def fail(message: str) -> None:
    print(f"[FAIL] {message}", file=sys.stderr)
    raise SystemExit(1)


def passed(message: str) -> None:
    print(f"[PASS] {message}")


def request_json(method: str, path: str, payload=None, retries: int = 0):
    data = None
    headers = {}
    if payload is not None:
        data = json.dumps(payload).encode("utf-8")
        headers["Content-Type"] = "application/json"

    last_reason = None
    for attempt in range(retries + 1):
        req = urllib.request.Request(
            f"{BASE_URL}{path}",
            data=data,
            headers=headers,
            method=method,
        )
        try:
            with urllib.request.urlopen(req, timeout=TIMEOUT) as resp:
                body = resp.read().decode("utf-8")
                return resp.status, json.loads(body)
        except urllib.error.HTTPError as exc:
            body = exc.read().decode("utf-8")
            try:
                parsed = json.loads(body)
            except json.JSONDecodeError:
                parsed = {"raw": body}
            return exc.code, parsed
        except urllib.error.URLError as exc:
            last_reason = exc.reason
            if attempt < retries:
                time.sleep(CONNECT_RETRY_INTERVAL)
                continue

    fail(
        f"cannot connect to {BASE_URL}{path}: {last_reason}. "
        "Start the HTTP server first, wait until it is listening, or set BASE_URL to the server address."
    )


def expect_status(name: str, actual: int, expected: int) -> None:
    if actual != expected:
        fail(f"{name} expected HTTP {expected}, got {actual}")


def main() -> None:
    print(f"BASE_URL={BASE_URL}")
    print(f"MODEL={MODEL}")

    status, body = request_json("GET", "/v1/models", retries=CONNECT_RETRIES)
    expect_status("GET /v1/models", status, 200)
    if body.get("object") != "list":
        fail("GET /v1/models response is not a list")
    print("models:", ", ".join(item.get("id", "?") for item in body.get("data", [])))
    passed("GET /v1/models")

    status, body = request_json(
        "POST",
        "/v1/chat/completions",
        {
            "model": MODEL,
            "inference_backend": "local",
            "messages": [{"role": "user", "content": "Reply with one short sentence."}],
            "max_tokens": 8,
        },
    )
    expect_status("POST /v1/chat/completions local", status, 200)
    choice = body["choices"][0]
    print("chat:", choice.get("finish_reason"), repr(choice["message"]["content"][:80]))
    passed("POST /v1/chat/completions local")

    status, body = request_json(
        "POST",
        "/v1/chat/completions",
        {
            "model": MODEL,
            "inference_backend": "rpc",
            "messages": [{"role": "user", "content": "hello"}],
            "max_tokens": 4,
        },
    )
    expect_status("POST /v1/chat/completions rpc failure", status, 400)
    error = body.get("error", {})
    if error.get("code") not in {"backend_not_available", "capability_not_supported"}:
        fail(f"unexpected rpc failure code: {error.get('code')}")
    print("chat rpc failure:", error.get("code"), "-", error.get("message"))
    passed("POST /v1/chat/completions rpc failure")

    status, body = request_json(
        "POST",
        "/v1/embeddings",
        {"model": MODEL, "input": "hello embeddings"},
    )
    expect_status("POST /v1/embeddings", status, 200)
    vector = body["data"][0]["embedding"]
    print("embeddings:", len(body["data"]), "vector(s), dim", len(vector))
    passed("POST /v1/embeddings")

    status, body = request_json(
        "POST",
        "/v1/rerank",
        {
            "model": MODEL,
            "query": "hello",
            "documents": ["other", "hello world"],
            "top_n": 1,
        },
    )
    expect_status("POST /v1/rerank", status, 200)
    item = body["data"][0]
    print("rerank:", "index", item["index"], "score", item["relevance_score"])
    passed("POST /v1/rerank")

    print("API smoke test passed.")


if __name__ == "__main__":
    main()
