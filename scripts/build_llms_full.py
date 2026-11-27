#!/usr/bin/env python3
"""Concatenate KeronisEngine markdown docs into a single llms-full.txt bundle."""

import re
import sys
from pathlib import Path

PAGES = [
    "index.md",
    "quickstart.md",
    "architecture.md",
    "core-primitives.md",
    "comparison.md",
    "faq.md",
    "strategies.md",
    "state-machines.md",
    "data-and-alt-data.md",
    "custom-venues.md",
    "live-and-paper.md",
    "control-server.md",
    "configuration.md",
]


def strip_frontmatter(text: str) -> str:
    return re.sub(r"\A---\n.*?\n---\n+", "", text, flags=re.S)


def normalize_markdown(text: str) -> str:
    # Convert MkDocs admonitions to plain markdown blockquotes
    def repl_adm(m: re.Match) -> str:
        title = m.group(2)
        body = re.sub(r"^    ", "", m.group(3), flags=re.M).rstrip()
        quoted = "\n".join(f"> {line}" if line else ">" for line in body.splitlines())
        return f"> **{title}:**\n{quoted}\n"

    text = re.sub(r'^!!! (\w+) "([^"]+)"\n((?:    .*\n?)+)', repl_adm, text, flags=re.M)

    # Rewrite relative .md links to full engine.keronis.ai URLs
    def repl_link(m: re.Match) -> str:
        slug = "docs" if m.group(1) == "index" else m.group(1)
        anchor = m.group(2) or ""
        return f"(https://engine.keronis.ai/{slug}/{anchor})"

    return re.sub(r"\(([a-z0-9_-]+)\.md(#[^)]*)?\)", repl_link, text)


def main() -> None:
    repo_root = Path(__file__).resolve().parent.parent
    docs_dir = repo_root / "docs"
    out_path = Path(sys.argv[1]) if len(sys.argv) > 1 else repo_root / "site/llms-full.txt"
    out_path.parent.mkdir(parents=True, exist_ok=True)

    parts = [
        "# KeronisEngine — Complete Documentation\n",
        "Source: https://engine.keronis.ai/ | Repo: https://github.com/JNeuvonen/keronisengine\n",
    ]
    for name in PAGES:
        path = docs_dir / name
        if path.exists():
            parts.append(f"\n---\n<!-- {name} -->\n\n")
            cleaned = normalize_markdown(strip_frontmatter(path.read_text(encoding="utf-8")).strip())
            parts.append(cleaned + "\n")

    out_path.write_text("\n".join(parts), encoding="utf-8")
    print(f"Wrote {out_path} ({out_path.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
