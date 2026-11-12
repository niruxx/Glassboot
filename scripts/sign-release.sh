#!/usr/bin/env bash
# A maintainer must supply their own trusted signing identity. Never creates a key.
set -euo pipefail
[[ $# -ge 1 && $# -le 2 ]] || { echo "usage: $0 GPG_KEY_ID [DIST_DIR]" >&2; exit 2; }
ROOT=$(cd "$(dirname "$0")/.." && pwd)
key="$1"
cd "${2:-$ROOT/dist}"
sha256sum --strict -c SHA256SUMS
gpg --local-user "$key" --armor --detach-sign --output SHA256SUMS.asc SHA256SUMS
gpg --armor --export "$key" > signing-key.asc
gpg --verify SHA256SUMS.asc SHA256SUMS
