import os
import sqlite3

DB_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data.db")

DEFAULT_THRESHOLDS = {            # sensor: (low, high)
    "temperature": (18, 32),
    "humidity": (30, 90),
    "gas": (None, 400),
    "motion": (50, None),               # distance in cm: alert when something is at or closer than 50 cm
}


def get_db():
    conn = sqlite3.connect(DB_FILE, timeout=10)
    conn.row_factory = sqlite3.Row
    return conn


def init_db():
    with get_db() as c:
        c.execute("""CREATE TABLE IF NOT EXISTS users (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            full_name TEXT NOT NULL,
            email TEXT NOT NULL UNIQUE,
            password_hash TEXT NOT NULL,
            created_at TEXT NOT NULL)""")
        c.execute("""CREATE TABLE IF NOT EXISTS readings (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            uid TEXT NOT NULL UNIQUE,
            ts TEXT NOT NULL,
            log_date TEXT NOT NULL,
            log_time TEXT NOT NULL,
            motion REAL,                -- ultrasonic distance in cm
            temperature REAL,
            humidity REAL,
            gas REAL,
            alarm_state INTEGER,
            fan INTEGER,
            led INTEGER,
            mode TEXT,
            synced INTEGER NOT NULL DEFAULT 0)""")
        c.execute("""CREATE TABLE IF NOT EXISTS sleep_log (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            uid TEXT NOT NULL UNIQUE,
            sleep_start TEXT NOT NULL,
            wake_time TEXT,
            duration_sec INTEGER,
            synced INTEGER NOT NULL DEFAULT 0)""")
        c.execute("""CREATE TABLE IF NOT EXISTS commands (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            cmd TEXT NOT NULL,
            target TEXT,
            value INTEGER,
            status TEXT NOT NULL DEFAULT 'pending',
            created_at TEXT NOT NULL)""")
        c.execute("""CREATE TABLE IF NOT EXISTS live (
            id INTEGER PRIMARY KEY CHECK (id = 1),
            mode TEXT, motion INTEGER, temperature REAL, humidity REAL, gas REAL,
            alarm_state INTEGER, fan INTEGER, led INTEGER,
            awake INTEGER NOT NULL DEFAULT 1,
            last_change TEXT, updated TEXT, last_seen TEXT)""")
        c.execute("INSERT OR IGNORE INTO live (id, mode, awake) VALUES (1, 'AUTO', 1)")
        c.execute("""CREATE TABLE IF NOT EXISTS thresholds (
            sensor TEXT PRIMARY KEY,
            low REAL, high REAL,
            enabled INTEGER NOT NULL DEFAULT 1,
            updated_at TEXT)""")
        for name, (lo, hi) in DEFAULT_THRESHOLDS.items():
            c.execute("INSERT OR IGNORE INTO thresholds (sensor, low, high, updated_at) "
                      "VALUES (?, ?, ?, datetime('now'))", (name, lo, hi))
        # the ultrasonic value changed from 0/1 to centimetres: move the old default limit (High = 1)
        c.execute("UPDATE thresholds SET low=50, high=NULL WHERE sensor='motion' AND low IS NULL AND high=1")


if __name__ == "__main__":
    init_db()
    with get_db() as c:
        for r in c.execute("SELECT name FROM sqlite_master WHERE type='table' ORDER BY name"):
            print("table:", r["name"])
