#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path
from typing import Iterable, List, Sequence, Tuple


PY_SYMBOL_RE = re.compile(r"^\s*(def|class)\s+([A-Za-z_][A-Za-z0-9_]*)")
SH_SYMBOL_RE = re.compile(r"^\s*(?:function\s+)?([A-Za-z_][A-Za-z0-9_]*)\s*\(\)\s*\{")
CXX_TYPE_RE = re.compile(r"^\s*(class|struct|enum|namespace)\s+([A-Za-z_][A-Za-z0-9_]*)")
CXX_FUNC_RE = re.compile(r"([A-Za-z_~][A-Za-z0-9_:~]*)\s*\(")
CONTROL_KEYWORDS = {"if", "for", "while", "switch", "catch"}


def estimate_tokens(text: str) -> int:
    text = text.strip()
    if not text:
        return 0
    return max(1, (len(text) + 3) // 4)


def make_chunk_id(kb_name: str, rel_path: str, start_line: int, end_line: int, symbol: str) -> str:
    raw = f"{kb_name}|{rel_path}|{start_line}|{end_line}|{symbol}"
    return hashlib.sha1(raw.encode("utf-8")).hexdigest()


def language_for_path(path: str) -> str:
    suffix = Path(path).suffix.lower()
    return {
        ".h": "cpp",
        ".hpp": "cpp",
        ".cc": "cpp",
        ".cpp": "cpp",
        ".c": "c",
        ".py": "python",
        ".sh": "shell",
        ".json": "json",
        ".md": "markdown",
    }.get(suffix, suffix.lstrip(".") or "text")


def detect_symbol_starts(lines: Sequence[str], suffix: str) -> List[Tuple[int, str]]:
    symbols: List[Tuple[int, str]] = []
    for idx, line in enumerate(lines):
        lineno = idx + 1
        if suffix == ".py":
            match = PY_SYMBOL_RE.match(line)
            if match:
                symbols.append((lineno, match.group(2)))
            continue

        if suffix == ".sh":
            match = SH_SYMBOL_RE.match(line)
            if match:
                symbols.append((lineno, match.group(1)))
            continue

        type_match = CXX_TYPE_RE.match(line)
        if type_match:
            symbols.append((lineno, type_match.group(2)))
            continue

        stripped = line.strip()
        if "(" not in stripped or stripped.startswith("#") or stripped.endswith(";"):
            continue

        has_body = stripped.endswith("{")
        if not has_body and idx + 1 < len(lines):
            has_body = lines[idx + 1].strip() == "{"
        if not has_body:
            continue

        match = CXX_FUNC_RE.search(stripped)
        if not match:
            continue
        symbol = match.group(1).split("::")[-1]
        if symbol in CONTROL_KEYWORDS:
            continue
        symbols.append((lineno, symbol))
    return symbols


def fixed_line_chunks(rel_path: str,
                      lines: Sequence[str],
                      kb_name: str,
                      symbol: str = "",
                      start_line: int = 1,
                      max_lines: int = 80,
                      overlap: int = 10) -> List[dict]:
    chunks: List[dict] = []
    if not lines:
        return chunks

    step = max(1, max_lines - overlap)
    for offset in range(0, len(lines), step):
        block = list(lines[offset:offset + max_lines])
        if not block:
            continue
        chunk_start = start_line + offset
        chunk_end = chunk_start + len(block) - 1
        text = "\n".join(block).strip()
        if not text:
            continue
        chunks.append({
            "chunk_id": make_chunk_id(kb_name, rel_path, chunk_start, chunk_end, symbol),
            "kb_name": kb_name,
            "doc_id": rel_path,
            "path": rel_path,
            "title": Path(rel_path).name,
            "symbol": symbol,
            "start_line": chunk_start,
            "end_line": chunk_end,
            "language": language_for_path(rel_path),
            "text": text,
            "token_estimate": estimate_tokens(text),
        })
    return chunks


def chunk_code_file(path: str,
                    text: str,
                    kb_name: str = "repo_code",
                    max_lines: int = 80,
                    overlap: int = 10) -> List[dict]:
    rel_path = Path(path).as_posix()
    lines = text.splitlines()
    suffix = Path(rel_path).suffix.lower()
    symbols = detect_symbol_starts(lines, suffix)

    if not symbols:
        return fixed_line_chunks(rel_path, lines, kb_name, "", 1, max_lines, overlap)

    chunks: List[dict] = []
    for idx, (start_line, symbol) in enumerate(symbols):
        end_line = len(lines)
        if idx + 1 < len(symbols):
            end_line = symbols[idx + 1][0] - 1
        block = lines[start_line - 1:end_line]
        if len(block) > max_lines:
            chunks.extend(fixed_line_chunks(rel_path, block, kb_name, symbol, start_line, max_lines, overlap))
            continue

        text_block = "\n".join(block).strip()
        if not text_block:
            continue
        chunks.append({
            "chunk_id": make_chunk_id(kb_name, rel_path, start_line, end_line, symbol),
            "kb_name": kb_name,
            "doc_id": rel_path,
            "path": rel_path,
            "title": Path(rel_path).name,
            "symbol": symbol,
            "start_line": start_line,
            "end_line": end_line,
            "language": language_for_path(rel_path),
            "text": text_block,
            "token_estimate": estimate_tokens(text_block),
        })
    return chunks


def _main(paths: Iterable[str]) -> int:
    all_chunks: List[dict] = []
    for item in paths:
        path = Path(item)
        text = path.read_text(encoding="utf-8")
        all_chunks.extend(chunk_code_file(path.as_posix(), text))
    print(json.dumps(all_chunks, ensure_ascii=False, indent=2))
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Chunk source files for RAG indexing")
    parser.add_argument("paths", nargs="+")
    args = parser.parse_args()
    return _main(args.paths)


if __name__ == "__main__":
    raise SystemExit(main())
