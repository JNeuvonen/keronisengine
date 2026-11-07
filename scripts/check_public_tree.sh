#!/usr/bin/env bash
# Fails if the tree holds anything that must not be published: personal
# paths, private keys, prebuilt binaries or GPL code. Maintainers also keep a
# private list of identifiers (hosts, wallets, account and strategy names) in
# PRIVATE_PATTERNS_FILE, one extended regex per line, outside the repo so the
# list itself is never published.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

status=0
fail() { echo "check_public_tree: $1" >&2; status=1; }

grep_tree() {
    git grep --untracked -nIE "$1" -- . ':!scripts/check_public_tree.sh'
}

generic_patterns=(
    '/home/[a-z][a-z0-9_-]*/'
    '/Users/[A-Za-z][A-Za-z0-9_-]*/'
    'BEGIN [A-Z ]*PRIVATE KEY'
)
for pattern in "${generic_patterns[@]}"; do
    if matches=$(grep_tree "$pattern"); then
        fail "forbidden pattern '$pattern':"$'\n'"$matches"
    fi
done

patterns_file="${PRIVATE_PATTERNS_FILE:-$HOME/.config/keronisengine/private_patterns.txt}"
if [[ -f "$patterns_file" ]]; then
    while IFS= read -r pattern; do
        [[ -z "$pattern" || "$pattern" == \#* ]] && continue
        if matches=$(git grep --untracked -nIiE "$pattern" -- .); then
            fail "private identifier matched (see $patterns_file):"$'\n'"$matches"
        fi
    done < "$patterns_file"
elif [[ -z "${CI:-}" ]]; then
    echo "check_public_tree: no private patterns file at $patterns_file" >&2
fi

if matches=$(git grep --untracked -lIE 'GNU (Lesser |Affero )?General Public License' -- . \
        ':!scripts/check_public_tree.sh'); then
    fail "GPL-licensed files:"$'\n'"$matches"
fi

if binaries=$(git ls-files --cached --others --exclude-standard |
        grep -E '\.(so|dylib|dll|a|o|bin|onnx|pkl)$'); then
    fail "committed binaries:"$'\n'"$binaries"
fi

exit $status
