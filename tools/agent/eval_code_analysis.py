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


def contains_any(items: List[str], expected: List[str]) -> bool:
    if not expected:
        return True
    text = "\n".join(items)
    return any(target in text for target in expected)


def score_row(row: Dict, result: Dict) -> Dict:
    evidence = result.get("evidence", [])
    final_answer = result.get("final_answer", {})
    if isinstance(final_answer, dict):
        answer_text = json.dumps(final_answer, ensure_ascii=False)
    else:
        answer_text = str(final_answer)

    paths = [item.get("path", "") for item in evidence]
    symbols = [item.get("symbol", "") for item in evidence]
    terms = row.get("expected_terms", [])

    path_hit = contains_any(paths + [answer_text], row.get("expected_paths", []))
    symbol_hit = contains_any(symbols + [answer_text], row.get("expected_symbols", []))
    evidence_hit = bool(evidence)
    term_hit = all(term in answer_text for term in terms) if terms else True

    return {
        "id": row["id"],
        "query": row["query"],
        "path_hit": path_hit,
        "symbol_hit": symbol_hit,
        "answer_term_hit": term_hit,
        "evidence_present": evidence_hit,
        "observed_paths": paths[:5],
        "observed_symbols": symbols[:5],
        "answer_preview": answer_text[:200],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="Run code analysis agent eval through /v1/agent/debug")
    parser.add_argument("--base-url", default="http://127.0.0.1:8080")
    parser.add_argument("--model", default="llama")
    parser.add_argument("--dataset", default=str(Path(__file__).with_name("eval_dataset.jsonl")))
    args = parser.parse_args()

    dataset = load_dataset(Path(args.dataset))
    rows = []
    for row in dataset:
        payload = {
            "model": args.model,
            "mode": "code_analysis",
            "debug": True,
            "agent_output_format": "structured",
            "query": row["query"],
        }
        result = post_json(args.base_url.rstrip("/") + "/v1/agent/debug", payload)
        rows.append(score_row(row, result))

    summary = {
        "total": len(rows),
        "path_hit": sum(1 for row in rows if row["path_hit"]),
        "symbol_hit": sum(1 for row in rows if row["symbol_hit"]),
        "answer_term_hit": sum(1 for row in rows if row["answer_term_hit"]),
        "evidence_present": sum(1 for row in rows if row["evidence_present"]),
    }
    print(json.dumps({"summary": summary, "rows": rows}, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
