#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
#
# Decompress the system linux-firmware QCA6174 hw3.0 images into build/fixtures/
# so the host tests can parse them. This only READS from /lib/firmware and
# WRITES into the repo's build/ dir; it never modifies system firmware.
set -euo pipefail

SRC="${QCA6174_FW_SRC:-/lib/firmware/ath10k/QCA6174/hw3.0}"
OUT="${1:-build/fixtures}"

mkdir -p "$OUT"

if ! command -v zstd >/dev/null 2>&1; then
    echo "zstd not found; cannot decompress fixtures" >&2
    exit 2
fi

if [ ! -d "$SRC" ]; then
    echo "fixture source $SRC not present on this host; skipping" >&2
    exit 0
fi

shopt -s nullglob
copied=0
for z in "$SRC"/*.zst; do
    base="$(basename "$z" .zst)"
    zstd -q -d -f "$z" -o "$OUT/$base"
    copied=$((copied + 1))
    echo "decompressed $base"
done

if [ "$copied" -eq 0 ]; then
    echo "no .zst fixtures found in $SRC" >&2
fi
echo "fixtures ready in $OUT ($copied file(s))"
