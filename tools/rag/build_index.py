#!/usr/bin/env python3
from __future__ import annotations

import argparse
import os
import sqlite3
from collections import Counter
from pathlib import Path
from typing import Counter as CounterType, Dict, Iterable, List, Sequence, Tuple

from chunk_code import chunk_code_file
from chunk_docs import chunk_markdown_file


DOC_FILES = (
    "README.md",
    "serving/http/使用说明.md",
)
DOC_GLOB = "docs/**/*.md"
CODE_EXTS = {".h", ".hpp", ".cc", ".cpp", ".c", ".py", ".sh", ".json", ".md"}
EXCLUDED_DIRS = {".git", "build"}
MAX_FILE_SIZE = 512 * 1024
VALID_KBS = ("docs", "repo_code")


def parse_kbs(values: Sequence[str]) -> Tuple[str, ...]:
    if not values:
        return VALID_KBS
    out: List[str] = []
    for value in values:
        if value == "all":
            return VALID_KBS
        if value not in VALID_KBS:
            raise SystemExit(f"unsupported kb: {value}")
        if value not in out:
            out.append(value)
    return tuple(out)


def iter_docs_files(repo_root: Path) -> List[Path]:
    files = {repo_root / rel for rel in DOC_FILES}
    files.update(repo_root.glob(DOC_GLOB))
    return sorted(path for path in files if path.is_file())


def iter_repo_code_files(repo_root: Path) -> List[Path]:
    result: List[Path] = []
    for root, dirnames, filenames in os.walk(repo_root, topdown=True):
        dirnames[:] = [name for name in dirnames if name not in EXCLUDED_DIRS]
        base = Path(root)
        for filename in filenames:
            path = base / filename
            if path.suffix.lower() not in CODE_EXTS:
                continue
            if path.stat().st_size > MAX_FILE_SIZE:
                continue
            result.append(path)
    return sorted(result)


def read_text_file(path: Path) -> str | None:
    try:
        raw = path.read_bytes()
    except OSError:
        return None
    if b"\x00" in raw:
        return None
    try:
        return raw.decode("utf-8")
    except UnicodeDecodeError:
        return None


def assign_neighbors(chunks: List[Dict]) -> List[Dict]:
    for idx, chunk in enumerate(chunks):
        chunk["prev_chunk_id"] = chunks[idx - 1]["chunk_id"] if idx > 0 else ""
        chunk["next_chunk_id"] = chunks[idx + 1]["chunk_id"] if idx + 1 < len(chunks) else ""
    return chunks


def normalize_file_chunks(chunks: List[Dict]) -> List[Dict]:
    deduped: List[Dict] = []
    seen = set()
    for chunk in chunks:
        chunk_id = chunk.get("chunk_id", "")
        if not chunk_id or chunk_id in seen:
            continue
        seen.add(chunk_id)
        deduped.append(chunk)
    return assign_neighbors(deduped)


def build_chunks(repo_root: Path, selected_kbs: Sequence[str]) -> Tuple[List[Dict], CounterType[str], CounterType[str]]:
    chunk_counts: CounterType[str] = Counter()
    file_counts: CounterType[str] = Counter()
    all_chunks: List[Dict] = []

    if "docs" in selected_kbs:
        for path in iter_docs_files(repo_root):
            text = read_text_file(path)
            if text is None:
                continue
            rel = path.relative_to(repo_root).as_posix()
            chunks = normalize_file_chunks(chunk_markdown_file(rel, text, kb_name="docs"))
            if not chunks:
                continue
            file_counts["docs"] += 1
            chunk_counts["docs"] += len(chunks)
            all_chunks.extend(chunks)

    if "repo_code" in selected_kbs:
        for path in iter_repo_code_files(repo_root):
            text = read_text_file(path)
            if text is None:
                continue
            rel = path.relative_to(repo_root).as_posix()
            chunks = normalize_file_chunks(chunk_code_file(rel, text, kb_name="repo_code"))
            if not chunks:
                continue
            file_counts["repo_code"] += 1
            chunk_counts["repo_code"] += len(chunks)
            all_chunks.extend(chunks)

    return all_chunks, file_counts, chunk_counts


def create_schema(conn: sqlite3.Connection) -> None:
    conn.executescript(
        """
        CREATE TABLE IF NOT EXISTS chunks (
            chunk_id TEXT PRIMARY KEY,
            kb_name TEXT NOT NULL,
            doc_id TEXT NOT NULL,
            path TEXT NOT NULL,
            title TEXT,
            symbol TEXT,
            start_line INTEGER,
            end_line INTEGER,
            language TEXT,
            text TEXT NOT NULL,
            token_estimate INTEGER,
            prev_chunk_id TEXT,
            next_chunk_id TEXT
        );

        CREATE VIRTUAL TABLE IF NOT EXISTS chunks_fts USING fts5(
            chunk_id UNINDEXED,
            kb_name UNINDEXED,
            path,
            title,
            symbol,
            text
        );
        """
    )


def delete_existing_kbs(conn: sqlite3.Connection, selected_kbs: Sequence[str]) -> None:
    for kb_name in selected_kbs:
        conn.execute("DELETE FROM chunks WHERE kb_name = ?", (kb_name,))
        conn.execute("DELETE FROM chunks_fts WHERE kb_name = ?", (kb_name,))


def write_index(conn: sqlite3.Connection, chunks: Sequence[Dict], selected_kbs: Sequence[str], rebuild: bool) -> None:
    if rebuild:
        conn.execute("DELETE FROM chunks")
        conn.execute("DELETE FROM chunks_fts")
    else:
        delete_existing_kbs(conn, selected_kbs)

    conn.executemany(
        """
        INSERT INTO chunks (
            chunk_id, kb_name, doc_id, path, title, symbol,
            start_line, end_line, language, text, token_estimate,
            prev_chunk_id, next_chunk_id
        ) VALUES (
            :chunk_id, :kb_name, :doc_id, :path, :title, :symbol,
            :start_line, :end_line, :language, :text, :token_estimate,
            :prev_chunk_id, :next_chunk_id
        )
        """,
        chunks,
    )
    conn.executemany(
        """
        INSERT INTO chunks_fts (chunk_id, kb_name, path, title, symbol, text)
        VALUES (:chunk_id, :kb_name, :path, :title, :symbol, :text)
        """,
        chunks,
    )
    conn.commit()


def main() -> int:
    parser = argparse.ArgumentParser(description="Build SQLite FTS5 RAG index")
    parser.add_argument("--repo-root", default=str(Path(__file__).resolve().parents[2]))
    parser.add_argument("--output", default="data/rag_index.sqlite")
    parser.add_argument("--rebuild", action="store_true")
    parser.add_argument("--kb", action="append", choices=(*VALID_KBS, "all"))
    args = parser.parse_args()

    repo_root = Path(args.repo_root).resolve()
    output_path = Path(args.output)
    if not output_path.is_absolute():
        output_path = repo_root / output_path
    selected_kbs = parse_kbs(args.kb or [])

    output_path.parent.mkdir(parents=True, exist_ok=True)
    existed_before = output_path.exists()
    if args.rebuild and existed_before:
        output_path.unlink()
        existed_before = False
    chunks, file_counts, chunk_counts = build_chunks(repo_root, selected_kbs)

    conn = sqlite3.connect(output_path)
    try:
        create_schema(conn)
        write_index(conn, chunks, selected_kbs, rebuild=args.rebuild or not existed_before)
    finally:
        conn.close()

    total_files = sum(file_counts.values())
    total_chunks = len(chunks)
    print(f"index_path={output_path}")
    print(f"selected_kbs={','.join(selected_kbs)}")
    print(f"total_files={total_files}")
    print(f"total_chunks={total_chunks}")
    for kb_name in VALID_KBS:
        print(f"{kb_name}_files={file_counts[kb_name]}")
        print(f"{kb_name}_chunks={chunk_counts[kb_name]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
