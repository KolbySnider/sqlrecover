#!/usr/bin/env bash
#
# Artifact-recognition test: build a corpus using real Android schemas, run
# sqlrecover, and assert that recovered/live records are correctly identified as
# known artifacts (android_sms, android_calllog) with named columns, and that a
# recovered deleted SMS is a genuine deletion.
#
# Usage: tests/artifact_test.sh <path-to-sqlrecover-binary>
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

echo "[*] running sqlrecover (with --live to exercise both paths)"
"$BIN" "$DB" --live --output "$OUT"

echo "[*] verifying artifact recognition"
python3 - "$WORK/truth.json" "$OUT/records.json" <<'PY'
import json, re, sys
truth   = json.load(open(sys.argv[1]))
records = json.load(open(sys.argv[2]))

sms   = [r for r in records if r.get("artifact") == "android_sms"]
calls = [r for r in records if r.get("artifact") == "android_calllog"]

print(f"    android_sms matched      : {len(sms)}")
print(f"    android_calllog matched  : {len(calls)}")

assert sms,   "FAIL: no records recognised as android_sms"
assert calls, "FAIL: no records recognised as android_calllog"

# Named columns must be present and sensible on a matched SMS.
named = [r for r in sms if "columns" in r and "body" in r["columns"]]
assert named, "FAIL: matched SMS has no named 'body' column"

# A recovered (deleted) SMS must correspond to a genuine deletion.
deleted = set(truth["deleted_sms_ids"])
rec_ids = set()
for r in sms:
    if r["provenance"]["origin"] in ("slack", "freelist"):
        body = str(r.get("columns", {}).get("body", ""))
        m = re.search(r"message number (\d+)", body)
        if m:
            rec_ids.add(int(m.group(1)))

print(f"    deleted SMS recovered    : {sorted(rec_ids)}")
assert rec_ids, "FAIL: no deleted SMS recovered from residual space"
assert rec_ids <= deleted, f"FAIL: recovered a non-deleted SMS: {sorted(rec_ids - deleted)}"

# Decoded fields: a matched SMS should carry an ISO timestamp and a type label;
# a matched call should carry a duration and a call-type label.
import re as _re
iso = _re.compile(r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$")
sms_dec = next((r for r in sms if r.get("decoded")), None)
assert sms_dec, "FAIL: no SMS carried decoded fields"
dd = dict(sms_dec["decoded"])
assert "date" in dd and iso.match(dd["date"]), f"FAIL: SMS date not ISO: {dd.get('date')}"
assert dd.get("type") in {"inbox","sent","draft","outbox","failed","queued"}, \
    f"FAIL: SMS type not decoded: {dd.get('type')}"
print(f"    sample SMS decoded       : {dd}")

call_dec = next((r for r in calls if r.get("decoded")), None)
assert call_dec, "FAIL: no call carried decoded fields"
cd = dict(call_dec["decoded"])
assert cd.get("type") in {"incoming","outgoing","missed","voicemail","rejected","blocked"}, \
    f"FAIL: call type not decoded: {cd.get('type')}"
assert "duration" in cd, "FAIL: call duration not decoded"
print(f"    sample call decoded      : {cd}")

print("    PASS")
PY

echo "[+] artifact test passed"
