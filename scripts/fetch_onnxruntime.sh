#!/usr/bin/env bash
# Downloads the ONNX Runtime CPU release into third_party/onnxruntime and
# prints the path to pass as -DONNXRUNTIME_ROOT.
# Tries GitHub releases first, then falls back to NuGet (api.nuget.org).
set -euo pipefail
version="${ONNXRUNTIME_VERSION:-1.22.0}"
case "$(uname -s)-$(uname -m)" in
    Linux-x86_64)  asset="onnxruntime-linux-x64-${version}"; rid="linux-x64" ;;
    Linux-aarch64) asset="onnxruntime-linux-aarch64-${version}"; rid="linux-arm64" ;;
    Darwin-arm64)  asset="onnxruntime-osx-arm64-${version}"; rid="osx-arm64" ;;
    *) echo "unsupported platform $(uname -s)-$(uname -m)" >&2; exit 1 ;;
esac
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$SCRIPT_DIR/.." && pwd)/third_party"
mkdir -p "$root"
if [[ ! -d "$root/$asset" ]]; then
    gh_url="https://github.com/microsoft/onnxruntime/releases/download/v${version}/${asset}.tgz"
    if curl -fsSL "$gh_url" | tar -xz -C "$root" 2>/dev/null; then
        :
    else
        rm -rf "$root/$asset"
        nupkg="$(mktemp "${TMPDIR:-/tmp}/ort.XXXXXX.nupkg")"
        trap 'rm -f "$nupkg"' EXIT
        nuget_url="https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime/${version}/microsoft.ml.onnxruntime.${version}.nupkg"
        curl -fsSL "$nuget_url" -o "$nupkg"
        python3 - "$nupkg" "$root/$asset" "$rid" << 'PY'
import os, sys, zipfile
nupkg, dest, rid = sys.argv[1], sys.argv[2], sys.argv[3]
inc_prefix = "build/native/include/"
lib_prefix = f"runtimes/{rid}/native/"
with zipfile.ZipFile(nupkg) as zf:
    for info in zf.infolist():
        if info.is_dir():
            continue
        if info.filename.startswith(inc_prefix):
            rel = info.filename[len(inc_prefix):]
            out = os.path.join(dest, "include", rel)
        elif info.filename.startswith(lib_prefix):
            rel = info.filename[len(lib_prefix):]
            out = os.path.join(dest, "lib", rel)
        else:
            continue
        os.makedirs(os.path.dirname(out), exist_ok=True)
        with zf.open(info) as src, open(out, "wb") as dst:
            dst.write(src.read())
PY
    fi
fi
ln -sfn "$asset" "$root/onnxruntime"
echo "$root/onnxruntime"
