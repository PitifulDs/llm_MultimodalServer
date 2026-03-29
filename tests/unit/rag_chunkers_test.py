#!/usr/bin/env python3
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "rag"))

from chunk_code import chunk_code_file
from chunk_docs import chunk_markdown_file


def main() -> int:
    doc_text = "# Title\n\nParagraph one.\n\n## Usage\n\nParagraph two.\n"
    doc_chunks = chunk_markdown_file("docs/sample.md", doc_text, kb_name="docs")
    assert len(doc_chunks) >= 2
    assert doc_chunks[0]["kb_name"] == "docs"
    assert doc_chunks[0]["title"] == "Title"

    code_text = "class Demo {\n};\n\nint helper() {\n  return 1;\n}\n"
    code_chunks = chunk_code_file("serving/demo.cc", code_text, kb_name="repo_code")
    assert len(code_chunks) >= 1
    assert code_chunks[0]["kb_name"] == "repo_code"
    assert code_chunks[0]["path"] == "serving/demo.cc"
    assert any(chunk["symbol"] in {"Demo", "helper"} for chunk in code_chunks)

    print("rag_chunkers_test passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
