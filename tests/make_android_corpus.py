#!/usr/bin/env python3
"""
Build a corpus using REAL Android schemas (sms, calls) with known deleted rows,
so the known-artifact recognition path can be exercised. Mirrors make_corpus.py
but with authentic column layouts.

Prints ground truth: which sms/call rows were deleted.
"""
import sqlite3, os, sys, json

def build(path):
    for suffix in ("", "-wal", "-shm"):
        try: os.remove(path + suffix)
        except FileNotFoundError: pass

    con = sqlite3.connect(path)
    con.execute("PRAGMA journal_mode=WAL;")
    con.execute("PRAGMA wal_autocheckpoint=0;")
    con.execute("PRAGMA auto_vacuum=NONE;")
    con.execute("PRAGMA secure_delete=OFF;")

    # Canonical Android SMS table (mmssms.db / 'sms').
    con.execute("""CREATE TABLE sms(
        _id INTEGER PRIMARY KEY, thread_id INTEGER, address TEXT, person INTEGER,
        date INTEGER, date_sent INTEGER, protocol INTEGER, read INTEGER,
        status INTEGER, type INTEGER, body TEXT, service_center TEXT,
        subject TEXT)""")
    # Canonical call log.
    con.execute("""CREATE TABLE calls(
        _id INTEGER PRIMARY KEY, number TEXT, date INTEGER, duration INTEGER,
        type INTEGER, name TEXT)""")

    for i in range(1, 201):
        con.execute("""INSERT INTO sms(_id,thread_id,address,person,date,date_sent,
            protocol,read,status,type,body,service_center,subject)
            VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)""",
            (i, i % 10, f"+1555{i:07d}", None, 1700000000000 + i*1000,
             1700000000000 + i*1000, 0, i % 2, -1, (i % 2) + 1,
             f"text message number {i}: meet me at the usual place", None, None))
    for i in range(1, 81):
        con.execute("INSERT INTO calls(_id,number,date,duration,type,name) VALUES (?,?,?,?,?,?)",
            (i, f"+1555{i:07d}", 1700000000000 + i*5000, i*7, (i % 3) + 1,
             f"Caller {i}" if i % 2 else None))
    con.commit()

    # Checkpoint so data is in the main file, then delete and keep residue.
    con.execute("PRAGMA wal_checkpoint(TRUNCATE);")
    con.commit()

    deleted_sms = list(range(30, 70))
    # Delete every other call so freed cells don't all coalesce into one block
    # that destroys every record header — leaving recoverable residue.
    deleted_calls = list(range(10, 60, 2))
    con.executemany("DELETE FROM sms WHERE _id=?",   [(d,) for d in deleted_sms])
    con.executemany("DELETE FROM calls WHERE _id=?", [(d,) for d in deleted_calls])
    con.commit()

    # A WAL round so prior versions exist too.
    con.execute("PRAGMA wal_checkpoint(TRUNCATE);")
    con.commit()
    con.execute("UPDATE sms SET read=1, body='[edited]' WHERE _id BETWEEN 80 AND 95")
    con.commit()

    db_snap  = open(path, "rb").read()
    wal_snap = open(path + "-wal", "rb").read() if os.path.exists(path + "-wal") else None
    con.close()
    with open(path, "wb") as f: f.write(db_snap)
    if wal_snap is not None:
        with open(path + "-wal", "wb") as f: f.write(wal_snap)

    return {"deleted_sms_ids": deleted_sms, "deleted_call_ids": deleted_calls}

if __name__ == "__main__":
    path = sys.argv[1] if len(sys.argv) > 1 else "android.db"
    print(json.dumps(build(path)))
