import logging
import os
import sqlite3
import time

import requests

DB_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data.db")


def clean(s):
    return "".join(ch for ch in s if 32 < ord(ch) < 127)


URL = clean(os.environ.get("ONLINE_URL", "https://YOUR-PROJECT.supabase.co")).rstrip("/")
KEY = clean(os.environ.get("ONLINE_KEY", ""))
EVERY = int(os.environ.get("SYNC_EVERY", "30"))
BATCH = 100
log = logging.getLogger("sync")

TABLES = {
    "readings": (["uid", "ts", "log_date", "log_time", "motion", "temperature", "humidity", "gas",
                  "alarm_state", "fan", "led", "mode"], "", False),
    "sleep_log": (["uid", "sleep_start", "wake_time", "duration_sec"], "AND wake_time IS NOT NULL", True),
}


def push(table, rows, merge):
    prefer = "resolution=merge-duplicates" if merge else "resolution=ignore-duplicates"
    headers = {"apikey": KEY, "Content-Type": "application/json", "Prefer": f"{prefer},return=minimal"}
    if not KEY.startswith("sb_"):
        headers["Authorization"] = f"Bearer {KEY}"
    r = requests.post(f"{URL}/rest/v1/{table}?on_conflict=uid", json=rows, timeout=15, headers=headers)
    if not r.ok:
        raise requests.HTTPError(f"{r.status_code} {r.text[:300]}", response=r)


def sync_once():
    total = 0
    for table, (cols, extra, merge) in TABLES.items():
        while True:
            conn = sqlite3.connect(DB_FILE, timeout=10)
            conn.row_factory = sqlite3.Row
            rows = conn.execute(f"SELECT id, {','.join(cols)} FROM {table} WHERE synced=0 {extra} "
                                f"ORDER BY id LIMIT ?", (BATCH,)).fetchall()
            conn.close()
            if not rows:
                break
            push(table, [{k: r[k] for k in cols} for r in rows], merge)
            ids = [r["id"] for r in rows]
            conn = sqlite3.connect(DB_FILE, timeout=10)
            conn.execute(f"UPDATE {table} SET synced=1 WHERE id IN ({','.join('?' * len(ids))})", ids)
            conn.commit()
            conn.close()
            total += len(ids)
    return total


def main():
    log.warning("online sync started (every %ss)", EVERY)
    while True:
        try:
            n = sync_once()
            if n:
                log.warning("synced %d row(s) to the online database", n)
        except requests.RequestException as e:
            log.warning("upload failed, will retry: %s", e)
        except Exception:
            log.exception("sync error")
        time.sleep(EVERY)
