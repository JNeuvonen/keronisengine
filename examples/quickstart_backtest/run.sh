#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
RUNNER="${1:-$REPO_ROOT/build/bin/backtest_runner}"
WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/keronis_quickstart.XXXXXX")"
trap 'rm -rf "$WORK_DIR"' EXIT

mkdir -p "$WORK_DIR/results"
PYTHON="${PYTHON:-python3}"
"$PYTHON" "$SCRIPT_DIR/generate_sample_data.py" "$WORK_DIR"

DATA_DIR_ROOT="$WORK_DIR" "$RUNNER" "$WORK_DIR/config.json"

"$PYTHON" - "$WORK_DIR/results" << 'PY'
import json, sys
from pathlib import Path

res = Path(sys.argv[1])
summary_path = res / "summary.json"
trades_path = res / "trades.json"
if summary_path.exists():
    summary = json.loads(summary_path.read_text())
    print("\n=== Quickstart Backtest Summary (summary.json) ===")
    for k in ("total_pnl", "total_fees", "total_trades", "winning_trades", "max_drawdown", "total_volume"):
        if k in summary:
            print(f"  {k:16s}: {summary[k]}")
if trades_path.exists():
    trades = json.loads(trades_path.read_text())
    print(f"  emitted_trades  : {len(trades)} (written to trades.json + equity_curve.json)")
PY
