#!/usr/bin/env python3
"""Sync docs/*.md into website/src/content/docs/ with Starlight frontmatter and source drift checks."""

import re
import shutil
import sys
from pathlib import Path

# Map source docs/*.md -> (output filename under website/src/content/docs/, title, description)
# Note: docs/architecture.md is rendered by the standalone website/src/pages/architecture.astro page
# (and retained in docs/ for GitHub browsing and llms-full.txt).
PAGES = {
    "index.md": ("docs.md", "Documentation", "Documentation hub for KeronisEngine: quickstart, strategies, streaming feature state machines, ONNX inference, and live operations."),
    "quickstart.md": ("quickstart.md", "Quickstart", "Build KeronisEngine natively on Linux with CMake/vcpkg, run unit tests, and execute the example backtest."),
    "strategies.md": ("strategies.md", "Strategies", "Write and register out-of-tree C++20 strategies with KERONIS_REGISTER_STRATEGY and -DKERONIS_STRATEGY_DIRS."),
    "core-primitives.md": ("core-primitives.md", "Core Primitives & Invariants", "Ownership, threading contracts, locking rules, and sharp edges of OhlcvDataset, MarketDataStreams, Core::Engine, TradeTicker, StateUpdater, PaperExchange, and QuoteHolds."),
    "state-machines.md": ("state-machines.md", "State Machines & ONNX", "Build streaming C++ StrategyStateMachine feature engines and run sub-millisecond ONNX Runtime inference."),
    "custom-venues.md": ("custom-venues.md", "Custom Venues & Feeds", "Integrate new exchanges, brokers, and multi-outcome event markets via cpp_live_proxy, MarketDataStreams, or native C++ clients."),
    "live-and-paper.md": ("live-and-paper.md", "Live & Paper Trading", "Run live_infra with PostgreSQL, NATS, PaperExchange, QuoteHolds, and native venue clients."),
    "comparison.md": ("comparison.md", "Comparison", "How KeronisEngine compares to NautilusTrader and HftBacktest for predictive-model trading."),
    "faq.md": ("faq.md", "FAQ", "Frequently asked questions about KeronisEngine architecture, ONNX inference, venues, performance, and live operations."),
    "control-server.md": ("control-server.md", "Control Server API", "Embedded localhost HTTP introspection and gated control endpoints."),
    "configuration.md": ("configuration.md", "Configuration", "Environment variables for live_infra and export_strategy_config, plus backtest config.json reference."),
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


def rewrite_links(body: str) -> str:
    def repl(m: re.Match) -> str:
        slug = m.group(1)
        anchor = m.group(2) or ""
        if slug == "index":
            slug = "docs"
        return f"(/{slug}/{anchor})"

    return re.sub(r"\(([a-z0-9_-]+)\.md(#[^)]*)?\)", repl, body)


def verify_source_drift(root: Path, docs_dir: Path) -> None:
    """Fail fast if docs drift from real C++ source paths or control-server routes."""
    errors: list[str] = []

    # 1. Verify every GitHub blob/tree link into this repo resolves to an existing path on disk
    gh_re = re.compile(r"https://github\.com/JNeuvonen/keronisengine/(?:blob|tree)/main/([^)#\s]+)")
    for md in sorted(docs_dir.glob("*.md")):
        text = md.read_text(encoding="utf-8")
        for rel in gh_re.findall(text):
            if not (root / rel).exists():
                errors.append(f"{md.name}: broken GitHub source link -> {rel}")

    # 2. Verify every GET and POST route in engine_control_server/src/server.cpp is documented
    server_cpp = root / "src/cpp/packages/engine_control_server/src/server.cpp"
    ctrl_doc = (docs_dir / "control-server.md").read_text(encoding="utf-8")
    if server_cpp.exists():
        src = server_cpp.read_text(encoding="utf-8")
        get_routes = re.findall(r'server_\.Get\("(/control/[^"]+)"', src)
        post_routes = re.findall(r'register_mutating\("(/control/[^"]+)"', src)
        for route in get_routes + post_routes:
            if f"`{route}`" not in ctrl_doc:
                errors.append(f"control-server.md: missing route from server.cpp -> {route}")

    if errors:
        print("Documentation drift detected:", file=sys.stderr)
        for err in errors:
            print(f"  - {err}", file=sys.stderr)
        sys.exit(1)

    print("Source drift check passed (all GitHub source links and control-server routes verified).")


def main() -> None:
    root = Path(__file__).resolve().parent.parent
    docs_dir = root / "docs"
    out_dir = root / "website/src/content/docs"
    out_dir.mkdir(parents=True, exist_ok=True)

    verify_source_drift(root, docs_dir)

    # Remove stale architecture.md if present so website/src/pages/architecture.astro owns /architecture/
    stale_arch = out_dir / "architecture.md"
    if stale_arch.exists():
        stale_arch.unlink()

    for src_name, (out_name, title, desc) in PAGES.items():
        raw = (docs_dir / src_name).read_text(encoding="utf-8")
        # Strip leading H1 since Starlight renders title from frontmatter
        body = re.sub(r"\A# .+\n+", "", raw)
        body = convert_admonitions(body)
        body = rewrite_links(body)
        frontmatter = f'---\ntitle: "{title}"\ndescription: "{desc}"\n---\n\n'
        (out_dir / out_name).write_text(frontmatter + body, encoding="utf-8")

    shutil.copyfile(docs_dir / "llms.txt", root / "website/public/llms.txt")
    print(f"Synced {len(PAGES)} docs pages + llms.txt into website/")


if __name__ == "__main__":
    main()
