#!/usr/bin/env bash
# Downloads the ONNX Runtime CPU release into third_party/onnxruntime and
# prints the path to pass as -DONNXRUNTIME_ROOT.
set -euo pipefail
version="${ONNXRUNTIME_VERSION:-1.22.0}"
case "$(uname -s)-$(uname -m)" in
    Linux-x86_64) asset="onnxruntime-linux-x64-${version}" ;;
    Linux-aarch64) asset="onnxruntime-linux-aarch64-${version}" ;;
    Darwin-arm64) asset="onnxruntime-osx-arm64-${version}" ;;
    *) echo "unsupported platform $(uname -s)-$(uname -m)" >&2; exit 1 ;;
esac
root="$(git rev-parse --show-toplevel)/third_party"
mkdir -p "$root"
if [[ ! -d "$root/$asset" ]]; then
    curl -fsSL "https://github.com/microsoft/onnxruntime/releases/download/v${version}/${asset}.tgz" \
        | tar -xz -C "$root"
fi
ln -sfn "$asset" "$root/onnxruntime"
echo "$root/onnxruntime"
