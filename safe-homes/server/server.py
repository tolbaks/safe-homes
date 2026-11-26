"""Safe Homes server: website + login + API for the ESP-01 + local database (SQLite).
Run:  python server.py   ->  http://localhost:5000
Optional online database (Supabase): set ONLINE_URL and ONLINE_KEY (see sync.py)."""
import math
import os
import secrets
import sqlite3
import threading
from datetime import datetime, timedelta, timezone
from functools import wraps
from flask import Flask, request, jsonify, send_from_directory, session
from werkzeug.security import generate_password_hash, check_password_hash

from database import get_db, init_db, DB_FILE

BASE = os.path.dirname(os.path.abspath(__file__))
DEVICE_KEY = os.environ.get("DEVICE_KEY", "CHANGE_ME_DEVICE_KEY")   # must match the key in the ESP-01
PH = timezone(timedelta(hours=8))            # Philippine time
ONLINE_SEC = 15                              # device counts as online if it called this recently
CMD_EXPIRE_SEC = 60                          # old website commands are dropped, never run late

app = Flask(__name__)
key_file = os.path.join(BASE, "secret.key")
if not os.path.exists(key_file):
    with open(key_file, "w") as f:
        f.write(secrets.token_hex(32))
with open(key_file) as f:
    app.secret_key = f.read().strip()
app.config.update(SESSION_COOKIE_HTTPONLY=True, SESSION_COOKIE_SAMESITE="Lax",
                  PERMANENT_SESSION_LIFETIME=timedelta(hours=8))

SENSOR_NAMES = ("temperature", "humidity", "gas", "motion")
ALERT_TEXT = {
    "temperature": {"high": "Temperature is high: ventilate the room and run the fan.", "low": "Temperature is low."},
    "humidity": {"high": "Humidity is high: risk of mold, ventilate.", "low": "Humidity is low: the air is dry."},
    "gas": {"high": "Smoke/gas level is high: check for a leak or fire and ventilate.", "low": "Smoke/gas reading is low."},
    "motion": {"high": "Nothing detected nearby.", "low": "Something is close to the sensor: check the area."},
}


init_db()


# ---------------------------------------------------------------- helpers
def now_utc():
    return datetime.now(timezone.utc)


def iso(dt):
    return dt.isoformat(timespec="seconds")


def device_time(ts):
    """Use the RTC time the device sent (unix seconds, UTC). If it sent none, or its RTC lost
    power (date before 2023), fall back to the server clock."""
    try:
        ts = int(ts)
        if ts >= 1_700_000_000:
            return datetime.fromtimestamp(ts, timezone.utc), ts
    except (TypeError, ValueError, OverflowError, OSError):
        pass
    n = now_utc()
    return n, int(n.timestamp())


def load_thresholds():
    with get_db() as c:
        return {r["sensor"]: {"low": r["low"], "high": r["high"], "enabled": bool(r["enabled"])}
                for r in c.execute("SELECT * FROM thresholds")}


def check_thresholds(live, th):
    """Which sensors are at or past their Low/High limit, and the matching advice."""
    breached, alerts = {}, []
    if not live["awake"]:                        # asleep: the last values are old, raise no alerts
        return breached, alerts
    for name in SENSOR_NAMES:
        t, v = th.get(name), live.get(name)
        if not t or not t["enabled"] or v is None:
            continue
        if t["high"] is not None and v >= t["high"]:
            breached[name] = "high"
        elif t["low"] is not None and v <= t["low"]:
            breached[name] = "low"
        if name in breached:
            alerts.append(ALERT_TEXT[name][breached[name]])
    return breached, alerts


def touch(c):
    c.execute("UPDATE live SET last_seen=? WHERE id=1", (iso(now_utc()),))


def bad(msg, code=400):
    return jsonify(error=msg), code


def login_required(f):
    @wraps(f)
    def w(*a, **k):
        return f(*a, **k) if session.get("user") else bad("login required", 401)
    return w


def device_required(f):
    @wraps(f)
    def w(*a, **k):
        if not secrets.compare_digest(request.headers.get("X-API-Key", ""), DEVICE_KEY):
            return bad("bad api key", 403)
        return f(*a, **k)
    return w


# ---------------------------------------------------------------- page + accounts
@app.get("/")
def home():
    return send_from_directory(os.path.join(BASE, "templates"), "index.html")


@app.post("/api/auth/register")
def register():
    d = request.get_json(silent=True) or {}
    name = str(d.get("fullName", "")).strip()
    email = str(d.get("email", "")).strip().lower()
    pw = str(d.get("password", ""))
    if not name:
        return bad("Please enter your name.")
    if "@" not in email:
        return bad("Please enter a valid email address.")
    if len(pw) < 8:
        return bad("Password must be at least 8 characters.")
    conn = get_db()
    try:
        conn.execute("INSERT INTO users (full_name, email, password_hash, created_at) "
                     "VALUES (?, ?, ?, datetime('now'))", (name, email, generate_password_hash(pw)))
        conn.commit()
    except sqlite3.IntegrityError:
        return bad("An account with this email already exists.", 409)
    finally:
        conn.close()
    return jsonify(ok=True), 201


@app.post("/api/auth/login")
def login():
    d = request.get_json(silent=True) or {}
    email = str(d.get("email", "")).strip().lower()
    pw = str(d.get("password", ""))
    conn = get_db()
    u = conn.execute("SELECT * FROM users WHERE email = ?", (email,)).fetchone()
    conn.close()
    if u and check_password_hash(u["password_hash"], pw):
        session.clear()
        session["user"] = email
        session.permanent = True
        return jsonify(ok=True, token="ok", user={"email": u["email"], "fullName": u["full_name"]})
    return bad("Wrong email or password.", 401)


@app.post("/api/auth/logout")
def logout():
    session.clear()
    return jsonify(ok=True)


# ---------------------------------------------------------------- device API (ESP-01, local network)
@app.post("/api/device/reading")
@device_required
def dev_reading():
    d = request.get_json(silent=True)
    if not isinstance(d, dict) or not d:
        return bad("bad json")
    when, ts = device_time(d.get("ts"))
    local = when.astimezone(PH)
    uid = f"node1-{ts}"
    with get_db() as c:                         # INSERT OR IGNORE: a resent row never duplicates
        c.execute("INSERT OR IGNORE INTO readings (uid, ts, log_date, log_time, motion, temperature, "
                  "humidity, gas, alarm_state, fan, led, mode) VALUES (?,?,?,?,?,?,?,?,?,?,?,?)",
                  (uid, iso(when), local.strftime("%Y-%m-%d"), local.strftime("%H:%M:%S"),
                   d.get("motion"), d.get("temperature"), d.get("humidity"), d.get("gas"),
                   d.get("alarm_state"), d.get("fan"), d.get("led"), d.get("mode", "AUTO")))
        touch(c)
    return jsonify(ok=True, uid=uid)


@app.post("/api/device/live")
@device_required
def dev_live():
    d = request.get_json(silent=True)
    if not isinstance(d, dict) or not d:
        return bad("bad json")
    with get_db() as c:
        old = c.execute("SELECT mode, awake, last_change FROM live WHERE id=1").fetchone()
        mode = d.get("mode") or old["mode"]
        changed = (mode != old["mode"]) or not old["awake"] or not old["last_change"]
        c.execute("UPDATE live SET mode=?, motion=?, temperature=?, humidity=?, gas=?, alarm_state=?, "
                  "fan=?, led=?, awake=1, last_change=?, updated=?, last_seen=? WHERE id=1",
                  (mode, d.get("motion"), d.get("temperature"), d.get("humidity"), d.get("gas"),
                   d.get("alarm_state"), d.get("fan"), d.get("led"),
                   iso(now_utc()) if changed else old["last_change"], iso(now_utc()), iso(now_utc())))
    return jsonify(ok=True)


@app.post("/api/device/sleep")
@device_required
def dev_sleep():
    d = request.get_json(silent=True) or {}
    event = d.get("event")
    when, ts = device_time(d.get("ts"))
    w = iso(when)
    with get_db() as c:
        open_row = c.execute("SELECT id, sleep_start FROM sleep_log WHERE wake_time IS NULL "
                             "ORDER BY id DESC LIMIT 1").fetchone()
        if event == "sleep":
            if not open_row:
                c.execute("INSERT OR IGNORE INTO sleep_log (uid, sleep_start) VALUES (?, ?)",
                          (f"node1-sleep-{ts}", w))
            c.execute("UPDATE live SET mode='SLEEP', awake=0, last_change=?, last_seen=? WHERE id=1",
                      (w, iso(now_utc())))
        elif event == "wake":
            if open_row:
                secs = max(0, int((when - datetime.fromisoformat(open_row["sleep_start"])).total_seconds()))
                c.execute("UPDATE sleep_log SET wake_time=?, duration_sec=?, synced=0 WHERE id=?",
                          (w, secs, open_row["id"]))
            c.execute("UPDATE live SET mode='AUTO', awake=1, last_change=?, last_seen=? WHERE id=1",
                      (w, iso(now_utc())))
        else:
            return bad("bad event")
    return jsonify(ok=True)


@app.get("/api/device/command")
@device_required
def dev_command():
    cutoff = iso(now_utc() - timedelta(seconds=CMD_EXPIRE_SEC))
    with get_db() as c:
        touch(c)
        c.execute("UPDATE commands SET status='expired' WHERE status IN ('pending','sent') AND created_at < ?", (cutoff,))
        r = c.execute("SELECT * FROM commands WHERE status='pending' ORDER BY id LIMIT 1").fetchone()
        if not r:
            return jsonify({"t": int(now_utc().timestamp())})   # no command, but always the laptop time
        c.execute("UPDATE commands SET status='sent' WHERE id=?", (r["id"],))
    out = {"cmd": r["cmd"], "t": int(now_utc().timestamp())}
    if r["target"]:
        out["target"] = r["target"]
    if r["value"] is not None:
        out["value"] = r["value"]
    return jsonify(out)


# ---------------------------------------------------------------- website API (login required)
@app.get("/api/status")
@login_required
def api_status():
    with get_db() as c:
        row = dict(c.execute("SELECT * FROM live WHERE id=1").fetchone())
        pending = c.execute("SELECT COUNT(*) n FROM commands WHERE status IN ('pending','sent')").fetchone()["n"]
        unsynced = c.execute("SELECT COUNT(*) n FROM readings WHERE synced=0").fetchone()["n"]
    online = False
    if row["last_seen"]:
        online = (now_utc() - datetime.fromisoformat(row["last_seen"])).total_seconds() < ONLINE_SEC
    breached, alerts = check_thresholds(row, load_thresholds())
    return jsonify(live=row, online=online, pending=pending, unsynced=unsynced, breached=breached, alerts=alerts)


@app.get("/api/readings")
@login_required
def api_readings():
    lim = min(max(request.args.get("limit", 20, type=int), 1), 200)
    off = max(request.args.get("offset", 0, type=int), 0)
    with get_db() as c:
        total = c.execute("SELECT COUNT(*) n FROM readings").fetchone()["n"]
        rows = [dict(r) for r in c.execute("SELECT * FROM readings ORDER BY ts DESC LIMIT ? OFFSET ?", (lim, off))]
    return jsonify(rows=rows, total=total)


@app.get("/api/sleeplog")
@login_required
def api_sleeplog():
    with get_db() as c:
        rows = [dict(r) for r in c.execute("SELECT * FROM sleep_log ORDER BY id DESC LIMIT 50")]
    return jsonify(rows=rows)


@app.post("/api/command")
@login_required
def api_command():
    d = request.get_json(silent=True) or {}
    cmd = str(d.get("cmd", "")).upper()
    target, value = d.get("target"), d.get("value")
    with get_db() as c:
        live = c.execute("SELECT mode, awake FROM live WHERE id=1").fetchone()
        asleep = not live["awake"]
        if cmd == "WAKE":
            if not asleep:
                return bad("Device is already awake.", 409)
        elif asleep:
            return bad("Device is asleep. Wake it first.", 409)
        elif cmd in ("AUTO", "MAINTENANCE", "SLEEP"):
            pass
        elif cmd == "SET":
            if live["mode"] != "MAINTENANCE":
                return bad("Switch to MAINTENANCE mode first.", 409)
            if target not in ("buzzer", "fan", "led") or value not in (0, 1):
                return bad("bad SET command")
        elif cmd == "READ":
            if target not in ("motion", "temperature", "humidity", "gas"):
                return bad("bad READ target")
        else:
            return bad("unknown command")
        c.execute("INSERT INTO commands (cmd, target, value, created_at) VALUES (?,?,?,?)",
                  (cmd, target if cmd in ("SET", "READ") else None,
                   value if cmd == "SET" else None, iso(now_utc())))
    return jsonify(ok=True)


@app.route("/api/thresholds", methods=["GET", "POST"])
@login_required
def api_thresholds():
    if request.method == "POST":
        data = request.get_json(silent=True)
        if not isinstance(data, dict) or not data:
            return bad("bad json")
        clean = {}
        for name, t in data.items():
            if name not in SENSOR_NAMES or not isinstance(t, dict):
                return bad(f"unknown sensor: {name}")
            try:
                vals = [None if t.get(k) in (None, "") else float(t.get(k)) for k in ("low", "high")]
            except (TypeError, ValueError):
                return bad(f"{name}: Low and High must be numbers")
            if any(v is not None and not math.isfinite(v) for v in vals):
                return bad(f"{name}: Low and High must be numbers")
            if vals[0] is not None and vals[1] is not None and vals[0] >= vals[1]:
                return bad(f"{name}: Low must be smaller than High")
            clean[name] = (vals[0], vals[1], 1 if t.get("enabled", True) else 0)
        with get_db() as c:
            for name, (lo, hi, en) in clean.items():
                c.execute("UPDATE thresholds SET low=?, high=?, enabled=?, updated_at=? WHERE sensor=?",
                          (lo, hi, en, iso(now_utc()), name))
    return jsonify(load_thresholds())


@app.post("/api/sync")
@login_required
def api_sync():
    try:
        import sync
    except ImportError:
        return bad("sync.py is not installed yet.", 503)
    if not sync.URL or not sync.KEY:
        return bad("Online database not configured (set ONLINE_URL and ONLINE_KEY).", 503)
    try:
        return jsonify(synced=sync.sync_once())
    except Exception:
        return bad("Online database unreachable. Data is safe locally and will sync later.", 503)


# ---------------------------------------------------------------- database status for the website
import time

_online_cache = {"t": 0, "ok": False, "detail": "checking"}


def check_online():
    """Ask Supabase if it is reachable. Cached 15 s so the dashboard polling stays light."""
    if time.time() - _online_cache["t"] < 15:
        return _online_cache["ok"], _online_cache["detail"]
    ok, detail = False, "not configured (set ONLINE_URL and ONLINE_KEY)"
    try:
        import sync
        import requests
        if sync.URL and sync.KEY:
            h = {"apikey": sync.KEY}
            if not sync.KEY.startswith("sb_"):
                h["Authorization"] = "Bearer " + sync.KEY
            r = requests.get(f"{sync.URL}/rest/v1/readings?select=uid&limit=1", headers=h, timeout=3)
            ok = r.ok
            detail = "connected" if ok else f"error {r.status_code}"
    except ImportError:
        detail = "sync.py or requests missing"
    except Exception:
        detail = "unreachable (no internet)"
    _online_cache.update(t=time.time(), ok=ok, detail=detail)
    return ok, detail


@app.get("/api/db-status")
@login_required
def api_db_status():
    local_ok = True
    try:
        with get_db() as c:
            ur = c.execute("SELECT COUNT(*) n FROM readings WHERE synced=0").fetchone()["n"]
            us = c.execute("SELECT COUNT(*) n FROM sleep_log WHERE synced=0 AND wake_time IS NOT NULL").fetchone()["n"]
    except Exception:
        local_ok, ur, us = False, 0, 0
    online_ok, detail = check_online()
    return jsonify(local_db=local_ok, local_file=os.path.basename(DB_FILE), online_db=online_ok,
                   online_detail=detail, unsynced_readings=ur, unsynced_sleep=us)


if __name__ == "__main__":
    try:
        import sync
        if sync.URL and sync.KEY:               # auto sync to the online database in the background
            threading.Thread(target=sync.main, daemon=True).start()
    except ImportError:
        print("sync.py not found: online sync is off for now.")
    print("Database file:", DB_FILE)
    app.run(host="0.0.0.0", port=5000, debug=False)
