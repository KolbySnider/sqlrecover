#!/usr/bin/env python3
"""
Build a SQLite test corpus with KNOWN deleted data so recovery can be checked
against ground truth.

Strategy:
  - create a 'messages' table (mimics an Android SMS store)
  - insert N rows, remember them all
  - DELETE a known subset (do NOT VACUUM -> bytes remain as residual)
  - leave WAL mode on with uncheckpointed frames so a -wal sidecar exists

Prints the rowids it deleted so the smoke test knows what to look for.
"""
import sqlite3, os, sys, json

def build(path):
    for suffix in ("", "-wal", "-shm"):
        try: os.remove(path + suffix)
        except FileNotFoundError: pass

    con = sqlite3.connect(path)
    con.execute("PRAGMA journal_mode=WAL;")
    con.execute("PRAGMA wal_autocheckpoint=0;")  # never auto-fold WAL back
    con.execute("PRAGMA auto_vacuum=NONE;")
    con.execute("PRAGMA secure_delete=OFF;")     # KEY: leave freed bytes intact
                                                 # (Android's default behaviour)
    con.execute("""
        CREATE TABLE messages(
            id INTEGER PRIMARY KEY,
            address TEXT,
            body TEXT,
            ts INTEGER,
            read INTEGER
        )""")
    con.execute("CREATE TABLE contacts(id INTEGER PRIMARY KEY, name TEXT, phone TEXT)")

    msgs = []
    for i in range(1, 201):
        addr = f"+1555{i:07d}"
        body = f"message body number {i} - the quick brown fox jumps over {i} dogs"
        con.execute("INSERT INTO messages(id,address,body,ts,read) VALUES (?,?,?,?,?)",
                    (i, addr, body, 1700000000 + i, i % 2))
        msgs.append(i)
    for i in range(1, 51):
        con.execute("INSERT INTO contacts(id,name,phone) VALUES (?,?,?)",
                    (i, f"Contact Person {i}", f"+1555{i:07d}"))
    con.commit()

    # Delete a known subset of messages. These should be recoverable as residual
    # data once the deletions are written into the main database file.
    deleted = list(range(20, 60)) + list(range(120, 140))
    con.executemany("DELETE FROM messages WHERE id=?", [(d,) for d in deleted])
    con.commit()

    # Checkpoint so the deletions (and their freeblocks) are written into the
    # MAIN db file. This is what exercises the freelist/slack recovery paths.
    con.execute("PRAGMA wal_checkpoint(TRUNCATE);")
    con.commit()

    # Now make a fresh round of updates that stay in the WAL (uncheckpointed),
    # so the -wal file holds PRIOR versions of these rows for WAL recovery.
    con.execute("UPDATE messages SET body='REDACTED', read=1 WHERE id BETWEEN 60 AND 70")
    con.commit()

    # Python's sqlite3 checkpoints on close, which would delete the -wal file.
    # Snapshot both files now (connection open, WAL populated) and restore after
    # close so the on-disk state matches a live, mid-transaction device image.
    db_snap  = open(path, "rb").read()
    wal_snap = open(path + "-wal", "rb").read() if os.path.exists(path + "-wal") else None
    con.close()
    with open(path, "wb") as f: f.write(db_snap)
    if wal_snap is not None:
        with open(path + "-wal", "wb") as f: f.write(wal_snap)

    return {"deleted_message_ids": deleted,
            "remaining_messages": [m for m in msgs if m not in deleted],
            "wal_prior_version_ids": list(range(60, 71)),
            "contacts": 50}

if __name__ == "__main__":
    path = sys.argv[1] if len(sys.argv) > 1 else "test.db"
    truth = build(path)
    print(json.dumps(truth))
