#!/usr/bin/env bash
#
# Timeline test: build the Android-schema corpus, run sqlrecover --timeline, and
# assert that the unified timeline is chronologically sorted, mixes artifact
# types, and flags recovered (deleted) events — with at least one recovered SMS
# corresponding to a genuine deletion.
#
# Usage: tests/timeline_test.sh <path-to-sqlrecover-binary>
#
set -euo pipefail

BIN="${1:-./build/sqlrecover}"
HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

DB="$WORK/android.db"
OUT="$WORK/out"

echo "[*] generating Android-schema corpus"
python3 "$HERE/make_android_corpus.py" "$DB" > "$WORK/truth.json"

echo "[*] running sqlrecover --timeline"
"$BIN" "$DB" --timeline --output "$OUT"

echo "[*] verifying timeline"
python3 - "$WORK/truth.json" "$OUT/timeline.json" <<'PY'
import json, re, sys
truth = json.load(open(sys.argv[1]))
tl    = json.load(open(sys.argv[2]))

assert tl, "FAIL: timeline is empty"

# (1) Chronologically sorted by epoch_ms.
times = [e["epoch_ms"] for e in tl]
assert times == sorted(times), "FAIL: timeline is not sorted by time"

# (2) Mixes artifact types (proves it's genuinely cross-artifact).
kinds = {e["artifact"] for e in tl}
assert "android_sms" in kinds and "android_calllog" in kinds, \
    f"FAIL: timeline not cross-artifact, saw only {kinds}"

# (3) ISO timestamps are well-formed.
iso = re.compile(r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$")
assert all(iso.match(e["time_utc"]) for e in tl), "FAIL: malformed timestamp"

# (4) Recovered events are flagged, and at least one recovered SMS is a genuine
# deletion from the ground-truth set.
deleted = set(truth["deleted_sms_ids"])
recovered = [e for e in tl if e["recovered"]]
assert recovered, "FAIL: no recovered events flagged in timeline"

rec_sms = set()
for e in tl:
    if e["recovered"] and e["artifact"] == "android_sms":
        m = re.search(r"message number (\d+)", e["summary"])
        if m:
            rec_sms.add(int(m.group(1)))

genuine = rec_sms & deleted
print(f"    total events             : {len(tl)}")
print(f"    artifact kinds           : {sorted(kinds)}")
print(f"    recovered events flagged : {len(recovered)}")
print(f"    recovered deleted SMS    : {sorted(genuine)}")
assert genuine, "FAIL: no recovered SMS matched a genuine deletion"
print("    PASS")
PY

echo "[+] timeline test passed"
