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
    with urllib.request.urlopen(req, timeout=60) as resp:
        return json.loads(resp.read().decode("utf-8"))


def reciprocal_rank(items: List[Dict], expected_paths: List[str], expected_symbols: List[str]) -> float:
    for idx, item in enumerate(items, start=1):
        if item.get("path") in expected_paths or item.get("symbol") in expected_symbols:
            return 1.0 / idx
    return 0.0


def main() -> int:
    parser = argparse.ArgumentParser(description="Evaluate retrieval quality through /v1/retrieval/search")
    parser.add_argument("--base-url", default="http://127.0.0.1:8080")
    parser.add_argument("--dataset", default=str(Path(__file__).with_name("eval_dataset.jsonl")))
    parser.add_argument("--top-k", type=int, default=5)
    parser.add_argument("--mode", default="hybrid")
    args = parser.parse_args()

    dataset = load_dataset(Path(args.dataset))
    recall_hits = 0
    mrr = 0.0
    file_hits = 0
    symbol_hits = 0

    for row in dataset:
        body = {
            "kb": row["kb"],
            "query": row["query"],
            "mode": args.mode,
            "top_k": args.top_k,
            "debug": True,
        }
        result = post_json(args.base_url.rstrip("/") + "/v1/retrieval/search", body)
        hits = result.get("hits", [])
        hit_paths = {item.get("path", "") for item in hits}
        hit_symbols = {item.get("symbol", "") for item in hits}
        if any(path in hit_paths for path in row.get("expected_paths", [])) or any(symbol in hit_symbols for symbol in row.get("expected_symbols", [])):
            recall_hits += 1
        if any(path in hit_paths for path in row.get("expected_paths", [])):
            file_hits += 1
        if any(symbol in hit_symbols for symbol in row.get("expected_symbols", [])):
            symbol_hits += 1
        mrr += reciprocal_rank(hits, row.get("expected_paths", []), row.get("expected_symbols", []))

    total = max(1, len(dataset))
    print(json.dumps({
        "queries": len(dataset),
        "Recall@k": recall_hits / total,
        "MRR": mrr / total,
        "file_hit_rate": file_hits / total,
        "symbol_hit_rate": symbol_hits / total,
    }, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
