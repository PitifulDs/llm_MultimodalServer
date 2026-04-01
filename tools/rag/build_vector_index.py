#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import math
import struct
from pathlib import Path
from typing import Iterable, List, Sequence

from build_index import VALID_KBS, build_chunks, parse_kbs


def tokenize(text: str) -> List[str]:
    chars = []
    for ch in text:
        if ch.isalnum() or ch in {"_", "/", ":", ".", "-"}:
            chars.append(ch.lower())
        else:
            chars.append(" ")
    return [token for token in "".join(chars).split() if token]


def fnv1a_64(value: str) -> int:
    h = 1469598103934665603
    for ch in value.encode("utf-8"):
        h ^= ch
        h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def embed_text(text: str, dim: int) -> List[float]:
    vec = [0.0] * dim
    tokens = tokenize(text)
    for token in tokens:
        hashed = fnv1a_64(token)
        idx1 = hashed % dim
        idx2 = ((hashed // dim) + len(token)) % dim
        vec[idx1] += 1.0
        vec[idx2] += 0.5

    norm = math.sqrt(sum(v * v for v in vec))
    if norm > 0:
        vec = [v / norm for v in vec]
    return vec


def write_jsonl(path: Path, rows: Iterable[dict]) -> None:
    with path.open("w", encoding="utf-8") as f:
        for row in rows:
            f.write(json.dumps(row, ensure_ascii=False) + "\n")


def write_npy_float32(path: Path, matrix: Sequence[Sequence[float]]) -> None:
    rows = len(matrix)
    cols = len(matrix[0]) if rows else 0
    header = str(
        {
            "descr": "<f4",
            "fortran_order": False,
            "shape": (rows, cols),
        }
    )
    header += " " * (16 - ((10 + len(header) + 1) % 16))
    header += "\n"

    with path.open("wb") as f:
        f.write(b"\x93NUMPY")
        f.write(bytes([1, 0]))
        f.write(struct.pack("<H", len(header)))
        f.write(header.encode("latin1"))
        for row in matrix:
            f.write(struct.pack("<" + "f" * cols, *row))


def write_faiss_placeholder(path: Path, *, dim: int, rows: int, chunk_ids: Sequence[str]) -> None:
    manifest = {
        "type": "faiss_placeholder",
        "metric": "inner_product",
        "dimension": dim,
        "rows": rows,
        "chunk_ids": len(chunk_ids),
    }
    path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description="Build vector artifacts for RAG v2")
    parser.add_argument("--repo-root", default=str(Path(__file__).resolve().parents[2]))
    parser.add_argument("--rebuild", action="store_true")
    parser.add_argument("--kb", action="append", choices=(*VALID_KBS, "all"))
    parser.add_argument("--dim", type=int, default=256)
    parser.add_argument("--chunks-output", default="data/rag_chunks.jsonl")
    parser.add_argument("--embeddings-output", default="data/rag_embeddings.npy")
    parser.add_argument("--faiss-output", default="data/rag_faiss.index")
    parser.add_argument("--id-map-output", default="data/rag_id_map.json")
    args = parser.parse_args()

    repo_root = Path(args.repo_root).resolve()
    selected_kbs = parse_kbs(args.kb or [])

    chunks_output = Path(args.chunks_output)
    embeddings_output = Path(args.embeddings_output)
    faiss_output = Path(args.faiss_output)
    id_map_output = Path(args.id_map_output)
    outputs = [chunks_output, embeddings_output, faiss_output, id_map_output]
    outputs = [p if p.is_absolute() else repo_root / p for p in outputs]
    chunks_output, embeddings_output, faiss_output, id_map_output = outputs

    for path in outputs:
        path.parent.mkdir(parents=True, exist_ok=True)
        if path.exists() and not args.rebuild:
            raise SystemExit(f"output already exists: {path} (use --rebuild)")

    chunks, _, chunk_counts = build_chunks(repo_root, selected_kbs)
    matrix = [embed_text(f"{chunk.get('path', '')}\n{chunk.get('symbol', '')}\n{chunk.get('text', '')}", args.dim) for chunk in chunks]
    chunk_ids = [chunk["chunk_id"] for chunk in chunks]

    write_jsonl(chunks_output, chunks)
    write_npy_float32(embeddings_output, matrix)
    id_map_output.write_text(json.dumps({"chunk_ids": chunk_ids}, ensure_ascii=False, indent=2), encoding="utf-8")
    write_faiss_placeholder(faiss_output, dim=args.dim, rows=len(matrix), chunk_ids=chunk_ids)

    print(f"chunks_path={chunks_output}")
    print(f"embeddings_path={embeddings_output}")
    print(f"faiss_index_path={faiss_output}")
    print(f"id_map_path={id_map_output}")
    print(f"selected_kbs={','.join(selected_kbs)}")
    print(f"total_chunks={len(chunks)}")
    for kb_name in VALID_KBS:
        print(f"{kb_name}_chunks={chunk_counts[kb_name]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
