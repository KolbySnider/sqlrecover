#!/usr/bin/env bash
#
# End-to-end smoke test: build a corpus with known deleted rows, run sqlrecover,
# and assert that (a) recovery finds genuinely-deleted rows, (b) there are zero
# false positives among labelled message rows, and (c) the tool exits cleanly.
#
# Usage: tests/smoke_test.sh <path-to-sqlrecover-binary>
#
set -euo pipefail

BIN="${1:-./build/sqlrecover}"
HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

DB="$WORK/test.db"
OUT="$WORK/out"

echo "[*] generating corpus"
python3 "$HERE/make_corpus.py" "$DB" > "$WORK/truth.json"

echo "[*] running sqlrecover"
"$BIN" "$DB" --output "$OUT" --report --verbose

echo "[*] verifying results"
python3 - "$WORK/truth.json" "$OUT/records.json" <<'PY'
import json, re, sys
truth   = json.load(open(sys.argv[1]))
records = json.load(open(sys.argv[2]))
deleted = set(truth["deleted_message_ids"])

found = set()
fp = []
for r in records:
    origin = r["provenance"]["origin"]
    if origin not in ("slack", "freelist"):
        continue
    for v in r["values"]:
        if isinstance(v, str):
            m = re.search(r"message body number (\d+)", v)
            if m:
                n = int(m.group(1))
                found.add(n)
                if n not in deleted:
                    fp.append(n)

print(f"    recovered deleted bodies : {sorted(found)}")
print(f"    false positives          : {sorted(fp)}")

assert found, "FAIL: no deleted rows recovered at all"
assert not fp, f"FAIL: false positives recovered: {sorted(fp)}"
assert found <= deleted, "FAIL: recovered a body that was never deleted"

# WAL recovery should also have produced prior-version records.
wal = [r for r in records if r["provenance"]["origin"] == "wal_prior"]
assert wal, "FAIL: no WAL prior-version records recovered"

print(f"    wal prior records        : {len(wal)}")
print("    PASS")
PY

echo "[+] smoke test passed"
