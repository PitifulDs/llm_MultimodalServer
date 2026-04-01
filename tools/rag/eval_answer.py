#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import urllib.request
from pathlib import Path
from typing import Dict, List


def load_dataset(path: Path) -> List[Dict]:
    rows: List[Dict] = []
    with path.open("r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line:
                rows.append(json.loads(line))
    return rows


def post_json(url: str, payload: Dict) -> Dict:
    data = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(url, data=data, headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=120) as resp:
        return json.loads(resp.read().decode("utf-8"))


def main() -> int:
    parser = argparse.ArgumentParser(description="Run answer-side RAG eval through /v1/chat/completions")
    parser.add_argument("--base-url", default="http://127.0.0.1:8080")
    parser.add_argument("--model", default="llama")
    parser.add_argument("--dataset", default=str(Path(__file__).with_name("eval_dataset.jsonl")))
    parser.add_argument("--mode", default="hybrid")
    args = parser.parse_args()

    dataset = load_dataset(Path(args.dataset))
    rows = []
    for row in dataset:
        payload = {
            "model": args.model,
            "max_tokens": 192,
            "messages": [{"role": "user", "content": row["query"]}],
            "rag": {
                "enabled": True,
                "kb": row["kb"],
                "top_k": 4,
                "mode": args.mode,
                "return_references": True,
                "debug": True,
            },
        }
        result = post_json(args.base_url.rstrip("/") + "/v1/chat/completions", payload)
        answer = result.get("choices", [{}])[0].get("message", {}).get("content", "")
        refs = result.get("references", [])
        rows.append({
            "id": row["id"],
            "query": row["query"],
            "answer_preview": answer[:160],
            "reference_paths": [item.get("path") for item in refs],
            "expected_paths": row.get("expected_paths", []),
            "expected_symbols": row.get("expected_symbols", []),
        })

    print(json.dumps(rows, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
