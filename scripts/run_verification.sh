#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
#
# Run the full host verification suite and save every real output under
# artifacts/verification/. This NEVER touches system firmware, EFI, NVRAM, or
# any remote machine -- it only reads /lib/firmware and writes into the repo.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

OUT="artifacts/verification"
mkdir -p "$OUT"

# Reproducible-ish header without leaking a volatile timestamp into tracked
# files beyond a single line.
STAMP="$(date -u '+%Y-%m-%dT%H:%M:%SZ')"

say() { echo "=== $* ==="; }

# 1) Environment -----------------------------------------------------------
{
    echo "QCA6174 macOS driver -- host verification"
    echo "generated: $STAMP"
    echo
    say "host"
    uname -a
    echo
    say "compilers"
    clang --version | head -3
    echo
    say "target hardware presence on THIS host"
    echo "(the QCA6174 is 168c:003e; this build host may differ)"
    lspci -nn 2>/dev/null | grep -iE "network|wireless|atheros|qualcomm|168c|8086:51f0" \
        || echo "lspci: no matching line / lspci unavailable"
} > "$OUT/00_environment.txt" 2>&1
say "wrote $OUT/00_environment.txt"

# 2) Fixture evidence (hexdumps proving the ABI we parse) ------------------
{
    echo "First 64 bytes of each decompressed fixture (ABI grounding evidence)."
    echo
    ./scripts/prepare_fixtures.sh build/fixtures 2>&1 || true
    echo
    for f in firmware-6.bin board-2.bin; do
        p="build/fixtures/$f"
        if [ -f "$p" ]; then
            echo "--- $p ---"
            head -c 64 "$p" | xxd
            echo
        fi
    done
} > "$OUT/10_fixture_headers.txt" 2>&1
say "wrote $OUT/10_fixture_headers.txt"

# 3) Build + unit tests ----------------------------------------------------
make clean > /dev/null 2>&1
make unit         > "$OUT/20_unit_tests.txt" 2>&1
UNIT_RC=$?
echo "exit=$UNIT_RC" >> "$OUT/20_unit_tests.txt"
say "unit tests exit=$UNIT_RC -> $OUT/20_unit_tests.txt"

# 4) Fixture probe (real firmware parse) -----------------------------------
make fixture-probe > "$OUT/30_fixture_probe.txt" 2>&1
PROBE_RC=$?
echo "exit=$PROBE_RC" >> "$OUT/30_fixture_probe.txt"
say "fixture probe exit=$PROBE_RC -> $OUT/30_fixture_probe.txt"

# 5) C++ glue boundary smoke ----------------------------------------------
make cxx-smoke > "$OUT/40_cxx_glue_smoke.txt" 2>&1
CXX_RC=$?
echo "exit=$CXX_RC" >> "$OUT/40_cxx_glue_smoke.txt"
say "cxx smoke exit=$CXX_RC -> $OUT/40_cxx_glue_smoke.txt"

# 6) Static analysis (clang analyzer) on common sources --------------------
{
    echo "clang --analyze on src/common (static analysis)"
    echo
    rc=0
    for c in src/common/*.c; do
        echo "--- $c ---"
        clang --analyze -Xclang -analyzer-output=text \
            -std=c11 -Isrc/common "$c" 2>&1
        rc=$((rc | $?))
    done
    rm -f *.plist 2>/dev/null
    echo
    echo "analyzer aggregate exit=$rc (no output above == no warnings)"
} > "$OUT/50_static_analysis.txt" 2>&1
say "wrote $OUT/50_static_analysis.txt"

# 7) Info.plist validation -------------------------------------------------
{
    echo "Info.plist well-formedness + personality sanity"
    python3 - <<'PY'
import plistlib
p = plistlib.load(open("kext/QCA6174Wifi/Info.plist","rb"))
per = p["IOKitPersonalities"]["QCA6174Wifi"]
print("CFBundleIdentifier:", p["CFBundleIdentifier"])
print("IOClass          :", per["IOClass"])
print("IOProviderClass  :", per["IOProviderClass"])
print("IOPCIPrimaryMatch:", per["IOPCIPrimaryMatch"])
assert not p["CFBundleIdentifier"].startswith("com.apple"), "must not impersonate Apple"
assert per["IOPCIPrimaryMatch"] == "0x003e168c", "must match only 168c:003e"
print("OK: non-Apple id, matches only 168c:003e")
PY
} > "$OUT/60_info_plist.txt" 2>&1
say "wrote $OUT/60_info_plist.txt"

# 8) Summary ---------------------------------------------------------------
TOTAL_RC=$((UNIT_RC | PROBE_RC | CXX_RC))
{
    echo "verification summary ($STAMP)"
    echo "unit_tests   exit=$UNIT_RC"
    echo "fixture_probe exit=$PROBE_RC"
    echo "cxx_smoke    exit=$CXX_RC"
    echo "aggregate    exit=$TOTAL_RC"
    echo
    echo "NOTE: the macOS kext itself is NOT built here -- Linux has no Xcode /"
    echo "macOS kernel SDK. Only host-testable logic + the C/C++ boundary are"
    echo "exercised. See docs/BUILD.md and docs/STATUS.md."
} > "$OUT/99_summary.txt" 2>&1
cat "$OUT/99_summary.txt"

exit $TOTAL_RC
