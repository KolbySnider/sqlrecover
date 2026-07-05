# sqlrecover

A command-line forensic tool that recovers deleted records from SQLite
databases. SQLite is the storage format behind most Android application data:
SMS, contacts, chat apps, browser history, and so on.

When SQLite deletes a row, its bytes usually stay on disk until the space gets
reused, but the library stops exposing them. `sqlrecover` parses the SQLite file
format directly rather than linking `libsqlite3`, so it can read the residual
data the library no longer shows.

## What it recovers

| Source | What it is | Origin tag |
|---|---|---|
| Live B-tree cells | Reachable, current rows (with `--live`) | `live` |
| Freelist pages | Whole pages returned to the free pool, content intact | `freelist` |
| Freeblocks & slack | Deleted cells lingering inside leaf pages | `slack` |
| WAL frames | Prior versions of rows from the write-ahead log | `wal_prior` |

Every recovered record carries full provenance: source file, origin, page
number, byte offset, and (for WAL hits) the frame index. In forensic work, where
a byte came from matters as much as the byte itself.

## Known-artifact recognition

Records recovered from slack space or the WAL come back as anonymous arrays of
typed values, since a deleted row has no `CREATE TABLE` attached to it.
`sqlrecover` matches each record against a built-in catalog of known Android
schemas by type fingerprint, turning a recovered row into a labelled artifact
with named columns:

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

The catalog currently covers Android SMS (`android_sms`), call log
(`android_calllog`), and a common contact projection (`android_contact`).
Matching tolerates NULL columns, which are routine on Android, and requires at
least two type-constrained columns to agree before it labels anything, so it
won't tag noise. Adding a new artifact takes a few lines in `src/artifact.cpp`.

### Field decoding

Matched artifacts also get their fields decoded into readable values, while the
raw values are kept. Android stores timestamps as milliseconds since the Unix
epoch and encodes message and call kinds as integers; the decoder turns these
into ISO-8601 UTC times, enum labels, and formatted durations:

```json
{
  "artifact": "android_calllog",
  "columns": { "number": "+15550000001", "date": 1700000005000,
               "duration": 7, "type": 2, "name": "Caller 1" },
  "decoded": { "date": "2023-11-14T22:13:25Z", "duration": "7s",
               "type": "outgoing" }
}
```

Decoders handle epoch-millis and epoch-seconds timestamps, call-type and
SMS-type enums, boolean flags, and durations. The timestamp decoder
range-checks its input (roughly 1990 to 2100) so an ordinary integer column
doesn't get dressed up as a date, and unknown enum codes are left alone rather
than guessed at.

## Cross-artifact timeline

With `--timeline`, the tool merges every dated event (SMS and calls, live and
recovered) into a single chronological view. Instead of a per-table dump, you
get the device's activity in order, with deleted and modified events slotted
into place and flagged.

```
2023-11-14T22:14:03Z  [R] android_sms      +15550000043  sent  text message number 43…
2023-11-14T22:13:25Z      android_calllog  outgoing  +15550000001  7s  Caller 1
2023-11-14T22:13:30Z      android_calllog  missed    +15550000002  14s
```

`[R]` marks events recovered from slack space, the freelist, or prior WAL
versions, i.e. data that isn't visible in the live database. Output is written
to both `timeline.txt` (human-readable) and `timeline.json` (each event with its
provenance). Events that appear both live and as a WAL prior version are
collapsed, keeping the live copy. Timestamps are UTC; see the note below.

> **Timezone note:** Android stores timestamps in UTC, and that's what the
> timeline emits, which is the right canonical form for evidence. A device
> *displays* times in its local timezone, so when correlating against
> screenshots you'll need to apply the device's offset.

## Building

Requires a C++17 compiler and CMake 3.16+.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Raw-image support (`--image`) uses signature carving and needs no external
dependencies.

## Usage

```
sqlrecover <input.db> [options]

  --image              treat input as a raw partition image; carve .db files out
  --wal <path>         explicit -wal file (default: <input>-wal if present)
  --output <dir>       output directory (default: ./out)
  --format json|csv    output format (default: json)
  --live               include live records too (default: deleted only)
  --table <name>       restrict output to records labelled <name>
  --report             also write a human-readable report
  --timeline           build a unified chronological timeline (timeline.txt/json)
  -v, --verbose        per-stage progress on stderr
  -h, --help           show help
```

Exit codes: `0` success, `1` parse error, `2` bad arguments. stdout is reserved
for clean machine-readable data; progress and reports go to stderr.

### Examples

Recover deleted rows from an extracted database and its WAL sidecar:

```sh
sqlrecover messages.db --report
```

Carve and analyse every database in a raw image, as CSV:

```sh
sqlrecover userdata.img --image --format csv --output ./case01
```

Only the recovered contacts, including live rows for comparison:

```sh
sqlrecover contacts2.db --live --table contacts
```

## How it works

A SQLite database is a sequence of fixed-size pages. Table rows live in the
cells of B-tree leaf pages; each cell is a length-prefixed *record* whose header
lists one serial type per column. `sqlrecover`:

1. Parses the 100-byte database header (page size, encoding, freelist root).
2. Walks each table's B-tree from the roots named in `sqlite_master`, decoding
   live records and following overflow pages so large values are reassembled.
3. Recovers residual data three ways: scanning freelist pages, walking the
   freeblock chain and unallocated gap inside each leaf page, and reconstructing
   prior row versions from WAL frames.
4. Decodes each record's serial-type body into typed values and emits them with
   provenance as JSON or CSV, plus an optional summary report.

Residual recovery is heuristic by nature: overwritten or coalesced freeblocks
yield partial rows. The tool keeps weak matches but flags them `suspect` instead
of dropping them, leaving triage to the examiner. Recovered text that isn't
valid UTF-8 is preserved as hex (`$text_raw` / `0x…`) so nothing is silently
lost and the output never corrupts.

## Project layout

```
include/sqlrecover/   public headers, one per module
src/                  implementations + main.cpp (CLI orchestration)
third_party/json.hpp  vendored nlohmann/json
tests/                ground-truth corpus generator + smoke test
```

## Testing

`tests/make_corpus.py` builds a database with a *known* set of deleted rows
(using `secure_delete=OFF`, Android's default, so freed bytes survive) and an
uncheckpointed WAL. `tests/smoke_test.sh` runs the tool against it and checks
that genuinely-deleted rows are recovered with zero false positives:

```sh
./tests/smoke_test.sh ./build/sqlrecover
```

`tests/make_android_corpus.py` builds a second corpus using authentic Android
schemas (`sms`, `calls`), and `tests/artifact_test.sh` checks that recovered and
live records are correctly recognised as `android_sms` / `android_calllog` with
named columns, and that recovered deleted messages are genuine deletions:

```sh
./tests/artifact_test.sh ./build/sqlrecover
```

`tests/timeline_test.sh` runs `--timeline` against the same corpus and checks
that the merged timeline is chronologically sorted, genuinely cross-artifact,
and flags recovered events that match known deletions:

```sh
./tests/timeline_test.sh ./build/sqlrecover
```

## Scope & limitations

- Table B-trees only; index B-trees are skipped (no row data there).
- WITHOUT ROWID tables decode as ordinary records (no rowid surfaced).
- Signature carving recovers contiguous databases; fragmented files won't
  carve cleanly.
- This is a learning/portfolio tool, not a validated evidentiary instrument.
  Treat recovered data, especially `suspect` rows, as leads to verify rather
  than proof.
