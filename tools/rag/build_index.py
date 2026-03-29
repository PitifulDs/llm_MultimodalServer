#!/usr/bin/env python3
from __future__ import annotations

import argparse
import os
import sqlite3
from collections import Counter
from pathlib import Path
from typing import Dict, Iterable, List, Sequence, Tuple

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


def build_chunks(repo_root: Path) -> Tuple[List[Dict], Counter, Counter]:
    chunk_counts = Counter()
    file_counts = Counter()
    all_chunks: List[Dict] = []

    for path in iter_docs_files(repo_root):
        text = read_text_file(path)
        if text is None:
            continue
        rel = path.relative_to(repo_root).as_posix()
        chunks = chunk_markdown_file(rel, text, kb_name="docs")
        if not chunks:
            continue
        file_counts["docs"] += 1
        chunk_counts["docs"] += len(chunks)
        all_chunks.extend(chunks)

    for path in iter_repo_code_files(repo_root):
        text = read_text_file(path)
        if text is None:
            continue
        rel = path.relative_to(repo_root).as_posix()
        chunks = chunk_code_file(rel, text, kb_name="repo_code")
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
            token_estimate INTEGER
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


def write_index(conn: sqlite3.Connection, chunks: Sequence[Dict]) -> None:
    conn.execute("DELETE FROM chunks")
    conn.execute("DELETE FROM chunks_fts")
    conn.executemany(
        """
        INSERT INTO chunks (
            chunk_id, kb_name, doc_id, path, title, symbol,
            start_line, end_line, language, text, token_estimate
        ) VALUES (
            :chunk_id, :kb_name, :doc_id, :path, :title, :symbol,
            :start_line, :end_line, :language, :text, :token_estimate
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
    args = parser.parse_args()

    repo_root = Path(args.repo_root).resolve()
    output_path = Path(args.output)
    if not output_path.is_absolute():
        output_path = repo_root / output_path

    output_path.parent.mkdir(parents=True, exist_ok=True)
    if output_path.exists():
        if not args.rebuild:
            raise SystemExit(f"index already exists: {output_path} (use --rebuild)")
        output_path.unlink()

    chunks, file_counts, chunk_counts = build_chunks(repo_root)
    conn = sqlite3.connect(output_path)
    try:
        create_schema(conn)
        write_index(conn, chunks)
    finally:
        conn.close()

    total_files = sum(file_counts.values())
    total_chunks = len(chunks)
    print(f"index_path={output_path}")
    print(f"total_files={total_files}")
    print(f"total_chunks={total_chunks}")
    for kb_name in ("docs", "repo_code"):
        print(f"{kb_name}_files={file_counts[kb_name]}")
        print(f"{kb_name}_chunks={chunk_counts[kb_name]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
