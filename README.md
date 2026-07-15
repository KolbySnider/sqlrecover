# sqlrecover

Command-line tool that recovers deleted records from SQLite databases.
SQLite backs most of the data on an Android device (SMS, contacts, chat
apps, browser history), so that's mostly what this is aimed at.

When SQLite deletes a row, the bytes usually just sit there until something
overwrites them. The library just stops showing them to you. sqlrecover
parses the file format itself instead of linking libsqlite3, so it can read
what's still physically on disk.

## What it recovers

- **Live B-tree cells** (with `--live`): reachable, current rows. Origin tag `live`.
- **Freelist pages**: whole pages SQLite returned to the free pool without zeroing. Origin tag `freelist`.
- **Freeblocks & slack**: deleted cells still sitting inside leaf pages. Origin tag `slack`.
- **WAL frames**: prior versions of rows from the write-ahead log. Origin tag `wal_prior`.

Every record keeps its provenance: source file, origin, page number, byte
offset, and (for WAL hits) the frame index. In this kind of work, where a
byte came from matters about as much as the byte itself.

`--image` lets the input be a raw partition image instead of a single `.db`
file. sqlrecover carves out every SQLite database by signature first, then
runs the normal pipeline on each one. `--carve-files` additionally carves
plain deleted files (JPEG, PNG, GIF, BMP, PDF, MP4/MOV, WAV) out of the same
image by their file-type signatures.

## Known-artifact recognition

Rows recovered from slack space or the WAL come back as anonymous arrays of
typed values. A deleted row doesn't have a `CREATE TABLE` sitting next to
it. sqlrecover matches each one against a built-in catalog of Android
schemas by type fingerprint, so instead of a value array you get something
like:

```json
{
  "artifact": "android_sms",
  "provenance": { "origin": "slack", "page_no": 4, "byte_offset": 12442 },
  "columns": {
    "address": "+15550000043",
    "body": "text message number 43: meet me at the usual place",
    "date": 1700000043000,
    "read": 1,
    "type": 2
  }
}
```

Right now the catalog covers SMS (`android_sms`, plus the GMS-messaging
variant `android_sms_gms`), MMS (`android_mms`), call log (`android_calllog`),
contacts (`android_contact`), browser history (`android_browser_history`),
MediaStore images (`android_media_images`), and WhatsApp messages
(`android_whatsapp_message`). Matching tolerates NULL columns (routine on
Android) and needs at least two type-constrained columns to agree before it
labels anything, so it doesn't tag noise. Adding an artifact is a few lines
in `src/artifact/artifact.cpp`.

### Field decoding

Matched artifacts also get their fields decoded into something readable,
while the raw values stick around too. Android stores timestamps as
milliseconds since the epoch and message/call kinds as bare integers, so
the decoder turns those into ISO-8601 UTC and enum labels:

```json
{
  "artifact": "android_calllog",
  "columns": { "number": "+15550000001", "date": 1700000005000,
               "duration": 7, "type": 2, "name": "Caller 1" },
  "decoded": { "date": "2023-11-14T22:13:25Z", "duration": "7s",
               "type": "outgoing" }
}
```

Covers epoch-millis/epoch-seconds timestamps, call-type and SMS-type enums,
bool flags, and durations. The timestamp decoder range-checks its input
(roughly 1990 to 2100) so a random integer column doesn't get dressed up as
a date. Codes it doesn't recognize get left alone instead of guessed at.

## Cross-artifact timeline

`--timeline` merges every dated event (SMS, calls, live and recovered) into
one chronological view instead of a per-table dump:

```
2023-11-14T22:14:03Z  [R] android_sms      +15550000043  sent  text message number 43…
2023-11-14T22:13:25Z      android_calllog  outgoing  +15550000001  7s  Caller 1
2023-11-14T22:13:30Z      android_calllog  missed    +15550000002  14s
```

`[R]` means recovered from slack, the freelist, or a prior WAL version, i.e.
data no longer visible in the live database. Written to both `timeline.txt`
and `timeline.json` (the latter with full provenance). Events that show up
both live and as a WAL prior version get collapsed, keeping the live copy.

Timestamps are UTC, since that's what Android stores. If you're
cross-referencing against screenshots or anything else showing local time,
remember to apply the device's offset.

## Building

C++20 compiler, CMake 3.16+.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

No external deps. Signature carving for `--image` is all hand-rolled too.

## Usage

```
sqlrecover <input.db> [options]

  --image              treat input as a raw partition image; carve .db files
                       out of it by signature
  --carve-files        also carve generic files (jpeg/png/gif/bmp/pdf/mp4/wav)
                       out of the image by signature; only with --image
  --wal <path>         explicit -wal file (default: <input>-wal if present)
  --output <dir>       output directory (default: ./out)
  --format json|jsonl|csv  output format for records (default: json)
  --live               include live records too (default: deleted only)
  --table <name>       restrict output to records labelled <name>
  --report             also write a human-readable report
  --timeline           build a unified chronological timeline (timeline.txt/json)
  -j, --jobs N         worker threads (default: one per CPU core)
  -v, --verbose        per-stage progress on stderr
  -h, --help           show help
```

Exit codes: `0` ok, `1` parse error, `2` bad arguments. stdout is just the
clean machine-readable data; progress and reports go to stderr.

### Examples

```sh
# deleted rows from an extracted db + its WAL sidecar
sqlrecover messages.db --report

# carve and analyse every database in a raw image, as CSV
sqlrecover userdata.img --image --format csv --output ./case01

# recovered contacts only, plus live rows for comparison
sqlrecover contacts2.db --live --table contacts
```

## How it works

A SQLite database is a sequence of fixed-size pages. Table rows live in the
cells of B-tree leaf pages, and each cell is a length-prefixed record whose
header lists one serial type per column. Roughly:

1. Parse the 100-byte database header (page size, encoding, freelist root).
2. Walk each table's B-tree from the roots named in `sqlite_master`, decoding
   live records and following overflow pages so big TEXT/BLOB values come
   back whole.
3. Recover residual data three ways: scan freelist pages, walk the freeblock
   chain and unallocated gap inside each leaf page, and reconstruct prior row
   versions from WAL frames.
4. Decode each record's serial-type body into typed values and emit them with
   provenance, plus an optional summary report.

Residual recovery is heuristic. Overwritten or coalesced freeblocks
sometimes give you partial rows. Weak matches get kept but flagged
`suspect` rather than thrown away, so triage is up to you. Recovered text
that isn't valid UTF-8 comes back as hex (`$text_raw` / `0x…`) instead of
getting mangled or dropped.

## Project layout

```
include/, src/
  core/       byte-level primitives: types, util, varint, serial decoding
  format/     SQLite file-format model: mapped file, pages, database, btree, schema, WAL
  recovery/   the recovery pipeline: freelist/slack/WAL scanning, thread pool
  artifact/   known-schema recognition + cross-artifact timeline
  carve/      raw-image carving: database and generic file signatures
  main.cpp, output.hpp/.cpp   CLI orchestration + result formatting (top-level)
third_party/json.hpp  vendored nlohmann/json
tests/                ground-truth corpus generator + smoke test
```

## Testing

`tests/make_corpus.py` builds a database with a known set of deleted rows
(`secure_delete=OFF`, Android's default, so freed bytes actually survive)
plus an uncheckpointed WAL. `tests/smoke_test.sh` runs the tool against it
and checks the genuinely-deleted rows come back with zero false positives:

```sh
./tests/smoke_test.sh ./build/sqlrecover
```

`tests/make_android_corpus.py` builds a second corpus with real Android
schemas (`sms`, `calls`); `tests/artifact_test.sh` checks that recovered and
live records get recognised as `android_sms` / `android_calllog` with named
columns, and that the recovered deleted messages are actual deletions:

```sh
./tests/artifact_test.sh ./build/sqlrecover
```

`tests/timeline_test.sh` runs `--timeline` against the same corpus and
checks the merged timeline is sorted, actually cross-artifact, and flags
recovered events that match known deletions:

```sh
./tests/timeline_test.sh ./build/sqlrecover
```

## Scope & limitations

Table B-trees only; index B-trees don't hold row data so they're skipped.
WITHOUT ROWID tables decode fine but without a rowid surfaced. Signature
carving only gets contiguous databases, so a fragmented one won't carve
cleanly.

And to be upfront about it: this started as a learning/portfolio project,
not a validated evidentiary tool. Treat what it recovers, `suspect` rows
especially, as leads to go verify rather than as proof of anything.
