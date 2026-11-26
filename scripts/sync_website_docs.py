#!/usr/bin/env python3
"""Sync docs/*.md into website/src/content/docs/ with Starlight frontmatter."""

import re
import shutil
from pathlib import Path

PAGES = {
    "quickstart.md": ("Quickstart", "Build KeronisEngine via Docker or native CMake/vcpkg, run unit tests, and execute the example backtest."),
    "strategies.md": ("Strategies", "Write and register out-of-tree C++20 strategies with KERONIS_REGISTER_STRATEGY and -DKERONIS_STRATEGY_DIRS."),
    "state-machines.md": ("State Machines & ONNX", "Build streaming C++ StrategyStateMachine feature engines and run sub-millisecond ONNX Runtime inference."),
    "architecture.md": ("Architecture", "Module boundaries, live_infra StateUpdater runtime, standby handoff, offline Parquet pipeline, and RingBufferBook internals."),
    "live-and-paper.md": ("Live & Paper Trading", "Run live_infra with PostgreSQL, NATS, PaperExchange, QuoteHolds, and native venue clients."),
    "comparison.md": ("Comparison", "How KeronisEngine compares to NautilusTrader and HftBacktest for predictive-model trading."),
    "control-server.md": ("Control Server API", "Embedded localhost HTTP introspection and gated control endpoints."),
    "configuration.md": ("Configuration", "Environment variables for live_infra and export_strategy_config, plus backtest config.json reference."),
}


def convert_admonitions(text: str) -> str:
    # Convert MkDocs `!!! warning "Title"\n    body` to Starlight `:::caution[Title]\nbody\n:::`
    def repl(m: re.Match) -> str:
        kind = m.group(1).lower()
        starlight_kind = {"warning": "caution", "danger": "danger", "note": "note", "tip": "tip"}.get(kind, "note")
        title = m.group(2)
        body = re.sub(r"^    ", "", m.group(3), flags=re.M).rstrip()
        return f":::{starlight_kind}[{title}]\n{body}\n:::\n"

    return re.sub(r'^!!! (\w+) "([^"]+)"\n((?:    .*\n?)+)', repl, text, flags=re.M)


def main() -> None:
    root = Path(__file__).resolve().parent.parent
    docs_dir = root / "docs"
    out_dir = root / "website/src/content/docs"
    out_dir.mkdir(parents=True, exist_ok=True)

    for fname, (title, desc) in PAGES.items():
        raw = (docs_dir / fname).read_text(encoding="utf-8")
        # Strip leading H1 since Starlight renders title from frontmatter
        body = re.sub(r"\A# .+\n+", "", raw)
        body = convert_admonitions(body)
        # Rewrite relative .md links to Starlight slug links
        body = re.sub(r"\(([a-z0-9_-]+)\.md(#[^)]*)?\)", r"(/\1/\2)", body)
        frontmatter = f'---\ntitle: "{title}"\ndescription: "{desc}"\n---\n\n'
        (out_dir / fname).write_text(frontmatter + body, encoding="utf-8")

    shutil.copyfile(docs_dir / "llms.txt", root / "website/public/llms.txt")
    print(f"Synced {len(PAGES)} docs pages + llms.txt into website/")


if __name__ == "__main__":
    main()
