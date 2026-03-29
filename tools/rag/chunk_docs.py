#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path
from typing import Iterable, List, Sequence


HEADING_RE = re.compile(r"^\s{0,3}(#{1,6})\s+(.*\S)\s*$")


def estimate_tokens(text: str) -> int:
    text = text.strip()
    if not text:
        return 0
    return max(1, (len(text) + 3) // 4)


def make_chunk_id(kb_name: str, rel_path: str, start_line: int, end_line: int, title: str) -> str:
    raw = f"{kb_name}|{rel_path}|{start_line}|{end_line}|{title}"
    return hashlib.sha1(raw.encode("utf-8")).hexdigest()


def split_markdown_sections(lines: Sequence[str], default_title: str) -> List[dict]:
    sections: List[dict] = []
    current = {"title": default_title, "start_line": 1, "lines": []}

    for lineno, line in enumerate(lines, start=1):
        match = HEADING_RE.match(line)
        if match:
            if current["lines"]:
                current["end_line"] = lineno - 1
                sections.append(current)
            current = {
                "title": match.group(2).strip(),
                "start_line": lineno,
                "lines": [line],
            }
        else:
            current["lines"].append(line)

    if current["lines"]:
        current["end_line"] = len(lines)
        sections.append(current)
    return sections


def chunk_markdown_file(path: str, text: str, kb_name: str = "docs", max_chars: int = 1800) -> List[dict]:
    rel_path = Path(path).as_posix()
    lines = text.splitlines()
    default_title = Path(rel_path).stem or rel_path
    sections = split_markdown_sections(lines, default_title)

    chunks: List[dict] = []
    for section in sections:
        paragraph_start = section["start_line"]
        paragraph_lines: List[str] = []

        def flush_paragraph(end_line: int) -> None:
            nonlocal paragraph_start, paragraph_lines
            content = "\n".join(paragraph_lines).strip()
            if not content:
                paragraph_lines = []
                paragraph_start = end_line + 1
                return

            current_start = paragraph_start
            current_text = content
            while current_text:
                part = current_text[:max_chars].strip()
                if not part:
                    break
                approx_lines = max(1, part.count("\n") + 1)
                chunk_end = min(end_line, current_start + approx_lines - 1)
                chunks.append({
                    "chunk_id": make_chunk_id(kb_name, rel_path, current_start, chunk_end, section["title"]),
                    "kb_name": kb_name,
                    "doc_id": rel_path,
                    "path": rel_path,
                    "title": section["title"],
                    "symbol": "",
                    "start_line": current_start,
                    "end_line": chunk_end,
                    "language": "markdown",
                    "text": part,
                    "token_estimate": estimate_tokens(part),
                })
                current_text = current_text[len(part):].strip()
                current_start = chunk_end + 1

            paragraph_lines = []
            paragraph_start = end_line + 1

        for offset, line in enumerate(section["lines"]):
            absolute_line = section["start_line"] + offset
            if not paragraph_lines:
                paragraph_start = absolute_line
            paragraph_lines.append(line)
            if not line.strip():
                flush_paragraph(absolute_line)

        flush_paragraph(section["end_line"])

    return chunks


def _main(paths: Iterable[str]) -> int:
    all_chunks: List[dict] = []
    for item in paths:
        path = Path(item)
        text = path.read_text(encoding="utf-8")
        all_chunks.extend(chunk_markdown_file(path.as_posix(), text))
    print(json.dumps(all_chunks, ensure_ascii=False, indent=2))
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Chunk markdown files for RAG indexing")
    parser.add_argument("paths", nargs="+")
    args = parser.parse_args()
    return _main(args.paths)


if __name__ == "__main__":
    raise SystemExit(main())
