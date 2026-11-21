"""Single-vehicle telemetry service. Run: python -m uvicorn server.app:app."""
from __future__ import annotations

import csv
import hashlib
import hmac
import io
import json
import os
import re
import secrets
import smtplib
import sqlite3
import ssl
import threading
import time
from collections import defaultdict
from contextlib import asynccontextmanager, contextmanager
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from email.message import EmailMessage
from pathlib import Path
from typing import Literal

from dotenv import load_dotenv
from fastapi import Depends, FastAPI, HTTPException, Request, Response
from fastapi.responses import FileResponse, StreamingResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, ConfigDict, Field, model_validator

ROOT = Path(__file__).resolve().parent
EARLIEST_VALID_TIME = 1735689600  # 2025-01-01T00:00:00Z, matches firmware clock check
CSP = ("default-src 'self'; img-src 'self' data: https://tile.openstreetmap.org; "
       "style-src 'self'; script-src 'self'; connect-src 'self'; frame-ancestors 'none'; "
       "base-uri 'none'; form-action 'self'")


@dataclass
class Config:
    database: str
    device_id: str
    device_token: str
    admin_user: str
    admin_password: str
    smtp_host: str = ""
    smtp_port: int = 587
    smtp_user: str = ""
    smtp_password: str = ""
    smtp_from: str = ""
    alert_email: str = ""
    low_battery: float | None = None
    allow_weak_password: bool = False  # local testing only; the tunnel script still demands >= 16 chars
    supabase_url: str = ""
    supabase_publishable_key: str = ""

    @classmethod
    def environment(cls):
        # MT15_ENV selects an isolated file (e.g. server/.env.demo for simulated data).
        env_file = Path(os.getenv("MT15_ENV") or ROOT / ".env")
        if not env_file.is_absolute():
            env_file = ROOT.parent / env_file
        load_dotenv(env_file)
        db = Path(os.getenv("DATABASE_PATH", "data/mt15.sqlite3"))
        if not db.is_absolute():
            db = ROOT.parent / db
        low = os.getenv("LOW_BATTERY_VOLTAGE", "").strip()
        cloud = os.getenv("MT15_DASHBOARD_MODE", "local") == "supabase"
        supabase_url = os.getenv("SUPABASE_URL", "").strip().rstrip("/") if cloud else ""
        # Earlier setup instructions stored the publishable key under SUPABASE_ANON_KEY.
        supabase_key = (os.getenv("SUPABASE_PUBLISHABLE_KEY", "").strip()
                        or os.getenv("SUPABASE_ANON_KEY", "").strip()) if cloud else ""
        if cloud and (not re.fullmatch(r"https://[a-z0-9-]+\.supabase\.co", supabase_url)
                      or not re.fullmatch(r"sb_publishable_[A-Za-z0-9_-]+", supabase_key)):
            raise RuntimeError("Supabase dashboard requires SUPABASE_URL and SUPABASE_PUBLISHABLE_KEY")
        return cls(str(db), os.getenv("DEVICE_ID", "mt15-001"),
                   os.getenv("DEVICE_TOKEN", ""), os.getenv("ADMIN_USER", "admin"),
                   os.getenv("ADMIN_PASSWORD", ""), os.getenv("SMTP_HOST", ""),
                   int(os.getenv("SMTP_PORT", "587")), os.getenv("SMTP_USER", ""),
                   os.getenv("SMTP_PASSWORD", ""), os.getenv("SMTP_FROM", ""),
                   os.getenv("ALERT_EMAIL", ""), float(low) if low else None,
                   os.getenv("ALLOW_WEAK_ADMIN_PASSWORD", "").strip() == "1",
                   supabase_url=supabase_url, supabase_publishable_key=supabase_key)


VN_TIME = timezone(timedelta(hours=7))


def stamp(epoch: float) -> str:
    return datetime.fromtimestamp(epoch, VN_TIME).strftime("%H:%M:%S %d/%m/%Y (UTC+7)")


class Telemetry(BaseModel):
    model_config = ConfigDict(extra="forbid", allow_inf_nan=False)
    device_id: str = Field(min_length=1, max_length=64)
    boot_id: str = Field(pattern=r"^[a-zA-Z0-9_-]{1,64}$")
    seq: int = Field(ge=0, le=4294967295)
    uptime_s: int = Field(ge=0)
    measured_at: datetime | None = None
    is_replay: bool = False
    mode: Literal["driving", "parked", "tracking", "low_battery", "bench"]
    ignition: bool | None = None
    armed: bool = False
    config_revision: int = Field(default=0, ge=0)
    battery_v: float | None = Field(default=None, ge=0, le=30)
    temperature_c: float | None = Field(default=None, ge=-55, le=125)
    gps_valid: bool = False
    latitude: float | None = Field(default=None, ge=-90, le=90)
    longitude: float | None = Field(default=None, ge=-180, le=180)
    speed_kmh: float | None = Field(default=None, ge=0, le=350)
    satellites: int | None = Field(default=None, ge=0, le=100)
    acceleration_g: float | None = Field(default=None, ge=0, le=32)
    gyro_dps: float | None = Field(default=None, ge=0, le=4000)
    motion: bool = False
    tilt_deg: float | None = Field(default=None, ge=-180, le=180)  # signed lean: + left, - right
    fallen: bool = False
    signal_dbm: int | None = Field(default=None, ge=-150, le=0)
    next_report_s: int = Field(default=5, ge=5, le=86400)
    errors: list[str] = Field(default_factory=list, max_length=12)

    @model_validator(mode="after")
    def consistency(self):
        if self.gps_valid and (self.latitude is None or self.longitude is None):
            raise ValueError("gps_valid requires coordinates")
        if not self.gps_valid and any(x is not None for x in (self.latitude, self.longitude, self.speed_kmh)):
            raise ValueError("Invalid GPS must use null coordinates and speed")
        if any(len(e) > 80 for e in self.errors):
            raise ValueError("Error code too long")
        if self.measured_at:
            if self.measured_at.tzinfo is None:
                raise ValueError("measured_at requires timezone")
            if self.measured_at.timestamp() > time.time() + 300:
                raise ValueError("Timestamp is in the future")
            if self.measured_at.timestamp() < EARLIEST_VALID_TIME:
                raise ValueError("Timestamp predates clock synchronisation")
        return self


class Login(BaseModel):
    username: str = Field(max_length=128)
    password: str = Field(max_length=256)


class Settings(BaseModel):
    armed: bool


class Store:
    def __init__(self, path):
        self.path = path
        Path(path).parent.mkdir(parents=True, exist_ok=True)
        with self.db() as db:
            db.executescript("""
            PRAGMA journal_mode=WAL;
            CREATE TABLE IF NOT EXISTS telemetry (
              id INTEGER PRIMARY KEY, boot_id TEXT NOT NULL, seq INTEGER NOT NULL,
              received_at REAL NOT NULL, sample_at REAL NOT NULL, payload TEXT NOT NULL,
              UNIQUE(boot_id,seq));
            CREATE INDEX IF NOT EXISTS sample_time ON telemetry(sample_at);
            CREATE TABLE IF NOT EXISTS latest (id INTEGER PRIMARY KEY CHECK(id=1), telemetry_id INTEGER);
            CREATE TABLE IF NOT EXISTS settings (id INTEGER PRIMARY KEY CHECK(id=1), armed INTEGER, revision INTEGER);
            INSERT OR IGNORE INTO settings VALUES(1,0,0);
            CREATE TABLE IF NOT EXISTS alerts (
              id INTEGER PRIMARY KEY, created_at REAL, kind TEXT, telemetry_id INTEGER,
              message TEXT, mail_status TEXT, attempts INTEGER DEFAULT 0,
              next_attempt REAL DEFAULT 0, last_error TEXT DEFAULT '');
            CREATE TABLE IF NOT EXISTS sessions (digest TEXT PRIMARY KEY, csrf TEXT, expires REAL);
            """)

    @contextmanager
    def db(self):
        db = sqlite3.connect(self.path, timeout=15)
        db.row_factory = sqlite3.Row
        try:
            with db:
                yield db
        finally:
            db.close()


def create_app(config: Config | None = None, *, worker=True):
    cfg = config or Config.environment()
    if len(cfg.device_token) < 24 or not cfg.admin_password or (len(cfg.admin_password) < 12 and not cfg.allow_weak_password):
        raise RuntimeError("Configure DEVICE_TOKEN (>=24 chars) and ADMIN_PASSWORD (>=12 chars) in server/.env "
                           "(or set ALLOW_WEAK_ADMIN_PASSWORD=1 for local testing only)")
    if len(cfg.admin_password) < 12:
        print("WARNING: weak ADMIN_PASSWORD allowed for local testing. Do not publish this server.")
    if cfg.low_battery is not None and not 9 <= cfg.low_battery <= 14:
        raise RuntimeError("LOW_BATTERY_VOLTAGE must be 9..14 V, or empty")
    store = Store(cfg.database)
    salt = secrets.token_bytes(16)
    password_hash = hashlib.pbkdf2_hmac("sha256", cfg.admin_password.encode(), salt, 260000)
    stop = threading.Event()
    attempts = defaultdict(list)
    attempts_lock = threading.Lock()
    mail_configured = bool(cfg.smtp_host and cfg.smtp_from and cfg.alert_email)

    def maintenance():
        with store.db() as db:
            db.execute("DELETE FROM sessions WHERE expires < ?", (time.time(),))
            # Preserve latest even if the vehicle has been offline for >30 days.
            db.execute("DELETE FROM telemetry WHERE received_at < ? AND id NOT IN (SELECT telemetry_id FROM latest)",
                       (time.time() - 30 * 86400,))
            db.execute("DELETE FROM alerts WHERE created_at < ? AND mail_status != 'pending'", (time.time() - 30 * 86400,))
        if not mail_configured:
            return
        with store.db() as db:
            row = db.execute("SELECT * FROM alerts WHERE mail_status='pending' AND next_attempt<=? ORDER BY id LIMIT 1", (time.time(),)).fetchone()
        if not row:
            return
        mail = EmailMessage()
        mail["From"], mail["To"] = cfg.smtp_from, cfg.alert_email
        mail["Subject"] = f"[MT15] {row['kind']} — cảnh báo #{row['id']}"
        mail["Message-ID"] = f"<mt15-{row['id']}-{int(row['created_at'])}@iot.local>"
        mail.set_content(row["message"])
        try:
            with smtplib.SMTP(cfg.smtp_host, cfg.smtp_port, timeout=15) as smtp:
                smtp.starttls(context=ssl.create_default_context())
                if cfg.smtp_user:
                    smtp.login(cfg.smtp_user, cfg.smtp_password)
                smtp.send_message(mail)
            with store.db() as db:
                db.execute("UPDATE alerts SET mail_status='sent',last_error='' WHERE id=?", (row["id"],))
        except Exception as error:
            count = row["attempts"] + 1
            with store.db() as db:
                db.execute("UPDATE alerts SET attempts=?,next_attempt=?,last_error=?,mail_status=? WHERE id=?",
                           (count, time.time() + min(3600, 30 * 2 ** min(count, 7)),
                            type(error).__name__, "failed" if count >= 10 else "pending", row["id"]))

    def run_worker():
        while not stop.wait(10):
            try:
                maintenance()
            except Exception:
                # Database remains authoritative; transient failures retry next tick.
                pass

    @asynccontextmanager
    async def lifespan(app):
        thread = threading.Thread(target=run_worker, daemon=True)
        if worker:
            thread.start()
        yield
        stop.set()
        if worker:
            thread.join(timeout=20)

    app = FastAPI(title="MT15 Monitor", lifespan=lifespan, docs_url=None, redoc_url=None, openapi_url=None)
    app.state.store, app.state.maintenance = store, maintenance
    app.mount("/static", StaticFiles(directory=ROOT / "static"), name="static")
    csp = CSP
    if cfg.supabase_url:
        realtime_url = "wss://" + cfg.supabase_url.removeprefix("https://")
        csp = CSP.replace("connect-src 'self';", f"connect-src 'self' {cfg.supabase_url} {realtime_url};")

    @app.middleware("http")
    async def security_headers(request, call_next):
        if request.method in {"POST", "PUT"}:
            # Chunked bodies would bypass the Content-Length cap; devices and browsers send a length.
            if "transfer-encoding" in request.headers:
                return Response(status_code=411)
            try:
                if int(request.headers.get("content-length", "0")) > 16384:
                    return Response(status_code=413)
            except ValueError:
                return Response(status_code=400)
        response = await call_next(request)
        response.headers["X-Content-Type-Options"] = "nosniff"
        response.headers["X-Frame-Options"] = "DENY"
        response.headers["Referrer-Policy"] = "no-referrer"
        response.headers["Cache-Control"] = "no-store"
        response.headers["Content-Security-Policy"] = csp
        return response

    def engine_off_low(p):
        return (p.get("battery_v") is not None and p["battery_v"] < cfg.low_battery
                and p["mode"] != "driving" and p.get("ignition") is not True)

    def low_battery_sustained(db, body):
        # Cranking and charging transients are not a weak battery: require two
        # consecutive fresh engine-off samples from the same boot below threshold.
        if cfg.low_battery is None or body.is_replay or not engine_off_low(body.model_dump()):
            return False
        previous = db.execute("SELECT payload FROM telemetry WHERE boot_id=? AND seq<? "
                              "AND json_extract(payload,'$.is_replay')=0 ORDER BY seq DESC LIMIT 1",
                              (body.boot_id, body.seq)).fetchone()
        return bool(previous) and engine_off_low(json.loads(previous["payload"]))

    def admin(request: Request):
        raw = request.cookies.get("mt15_session", "")
        digest = hashlib.sha256(raw.encode()).hexdigest()
        with store.db() as db:
            session = db.execute("SELECT * FROM sessions WHERE digest=? AND expires>?", (digest, time.time())).fetchone()
        if not session:
            raise HTTPException(401, "Login required")
        if request.method not in {"GET", "HEAD"} and not secrets.compare_digest(request.headers.get("x-csrf-token", "").encode(), session["csrf"].encode()):
            raise HTTPException(403, "Invalid CSRF token")
        return session

    def device(request: Request):
        auth = request.headers.get("authorization", "")
        if not secrets.compare_digest(auth.encode(), ("Bearer " + cfg.device_token).encode()):
            raise HTTPException(401, "Invalid device token")

    @app.get("/")
    def index():
        return FileResponse(ROOT / "static" / "index.html")

    @app.get("/dashboard-config")
    def dashboard_config():
        if cfg.supabase_url:
            return {"mode": "supabase", "url": cfg.supabase_url,
                    "key": cfg.supabase_publishable_key, "device_id": cfg.device_id}
        return {"mode": "local"}

    @app.get("/health")
    def health():
        with store.db() as db:
            db.execute("SELECT 1")
        return {"ok": True}

    @app.post("/api/v1/login")
    def login(body: Login, request: Request, response: Response):
        # Do not trust X-Forwarded-For provided by Internet clients.
        ip = request.client.host if request.client else "unknown"
        with attempts_lock:
            now = time.time()
            for key in list(attempts):
                attempts[key] = [t for t in attempts[key] if now - t < 60]
                if not attempts[key]:
                    del attempts[key]
            if len(attempts[ip]) >= 5:
                raise HTTPException(429, "Try again after one minute")
            attempts[ip].append(now)
        candidate = hashlib.pbkdf2_hmac("sha256", body.password.encode(), salt, 260000)
        if not (hmac.compare_digest(candidate, password_hash) and hmac.compare_digest(body.username.encode(), cfg.admin_user.encode())):
            raise HTTPException(401, "Incorrect username or password")
        token, csrf = secrets.token_urlsafe(32), secrets.token_urlsafe(24)
        with store.db() as db:
            db.execute("INSERT INTO sessions VALUES(?,?,?)", (hashlib.sha256(token.encode()).hexdigest(), csrf, time.time() + 12 * 3600))
        # cloudflared forwards HTTPS to localhost; proxy trust is limited to loopback by uvicorn.
        response.set_cookie("mt15_session", token, httponly=True, secure=request.url.scheme == "https", samesite="strict", max_age=43200)
        return {"csrf": csrf}

    @app.get("/api/v1/me")
    def me(session=Depends(admin)):
        return {"username": cfg.admin_user, "csrf": session["csrf"]}

    @app.post("/api/v1/logout")
    def logout(response: Response, session=Depends(admin)):
        with store.db() as db:
            db.execute("DELETE FROM sessions WHERE digest=?", (session["digest"],))
        response.delete_cookie("mt15_session")
        return {"ok": True}

    @app.post("/api/v1/telemetry", dependencies=[Depends(device)])
    def telemetry(body: Telemetry):
        if body.device_id != cfg.device_id:
            raise HTTPException(403, "Unknown device")
        now = time.time()
        payload = body.model_dump(mode="json")
        sample_at = body.measured_at.timestamp() if body.measured_at else now
        with store.db() as db:
            cursor = db.execute("INSERT OR IGNORE INTO telemetry(boot_id,seq,received_at,sample_at,payload) VALUES(?,?,?,?,?)",
                                (body.boot_id, body.seq, now, sample_at, json.dumps(payload)))
            duplicate = cursor.rowcount == 0
            settings = dict(db.execute("SELECT armed,revision FROM settings WHERE id=1").fetchone())
            settings["armed"] = bool(settings["armed"])
            if not duplicate:
                tid = cursor.lastrowid
                previous = db.execute("SELECT payload FROM telemetry WHERE id=(SELECT telemetry_id FROM latest WHERE id=1)").fetchone()
                old = json.loads(previous["payload"]) if previous else None
                if not body.is_replay:
                    if not old or old["boot_id"] != body.boot_id or old["seq"] < body.seq:
                        db.execute("INSERT OR REPLACE INTO latest VALUES(1,?)", (tid,))
                kinds = []
                if body.motion and body.armed and body.ignition is not True:
                    kinds.append("movement")
                if low_battery_sustained(db, body):
                    kinds.append("low_battery")
                # Tip-over alerts whether or not anti-theft is armed; only on the transition to fallen.
                if body.fallen and not (old and old.get("boot_id") == body.boot_id and old.get("fallen")):
                    kinds.append("fall")
                for kind in kinds:
                    recent = db.execute("SELECT 1 FROM alerts WHERE kind=? AND created_at>?", (kind, now - 300)).fetchone()
                    if recent:
                        continue
                    position = "GPS chưa có vị trí hợp lệ."
                    if body.gps_valid:
                        position = f"https://maps.google.com/?q={body.latitude:.6f},{body.longitude:.6f}"
                    else:
                        previous_fix = db.execute("SELECT sample_at,payload FROM telemetry WHERE json_extract(payload,'$.gps_valid')=1 AND sample_at<=? ORDER BY sample_at DESC LIMIT 1", (sample_at,)).fetchone()
                        if previous_fix:
                            fix = json.loads(previous_fix["payload"])
                            position = f"Vị trí cuối ({stamp(previous_fix['sample_at'])}): https://maps.google.com/?q={fix['latitude']:.6f},{fix['longitude']:.6f}"
                    label = {"movement": "Chuyển động khi đang bật chống trộm", "low_battery": "Bình yếu",
                             "fall": (f"Xe bị ngã sang {'trái' if body.tilt_deg > 0 else 'phải'} (nghiêng {abs(body.tilt_deg):.0f}°)"
                                     if body.tilt_deg is not None else "Xe bị ngã")}[kind]
                    battery = f"{body.battery_v:.2f} V" if body.battery_v is not None else "không đo được"
                    measured = stamp(body.measured_at.timestamp()) if body.measured_at else "thiết bị chưa đồng bộ giờ"
                    message = (f"Xe: {cfg.device_id}\nSự kiện: {label} ({kind})\nThời điểm đo: {measured}\n"
                               f"Nhận lúc: {stamp(now)}\nĐiện áp: {battery}\n"
                               f"Gửi bù: {'có (dữ liệu cũ)' if body.is_replay else 'không'}\n{position}")
                    db.execute("INSERT INTO alerts(created_at,kind,telemetry_id,message,mail_status) VALUES(?,?,?,?,?)",
                               (now, kind, tid, message, "pending" if mail_configured else "disabled"))
        return {"accepted": True, "duplicate": duplicate, "ack": {"boot_id": body.boot_id, "seq": body.seq},
                "server_time": datetime.now(timezone.utc).isoformat(), "settings": settings}

    @app.get("/api/v1/status", dependencies=[Depends(admin)])
    def status():
        with store.db() as db:
            row = db.execute("SELECT * FROM telemetry WHERE id=(SELECT telemetry_id FROM latest WHERE id=1)").fetchone()
            settings = dict(db.execute("SELECT armed,revision FROM settings WHERE id=1").fetchone())
            queue = db.execute("SELECT count(*) FROM alerts WHERE mail_status='pending'").fetchone()[0]
        settings["armed"] = bool(settings["armed"])
        result = {"device_id": cfg.device_id, "settings": settings, "smtp_configured": mail_configured,
                  "email_pending": queue, "low_battery_voltage": cfg.low_battery, "telemetry": None,
                  "connection": "never_seen", "config_applied": False}
        if row:
            p = json.loads(row["payload"])
            age = max(0, time.time() - row["received_at"])
            result.update(telemetry=p, received_at=row["received_at"], age_s=int(age),
                          connection=("sleeping" if p["next_report_s"] > 60 else "online") if age <= max(30, p["next_report_s"] + 60) else "offline",
                          config_applied=p["config_revision"] == settings["revision"] and p["armed"] == settings["armed"])
        return result

    def history_rows(start: float | None, end: float | None, limit: int):
        if not 1 <= limit <= 10000:
            raise HTTPException(422, "limit must be 1..10000")
        start = start if start is not None else time.time() - 86400
        end = end if end is not None else time.time() + 1
        if end < start:
            raise HTTPException(422, "Invalid time range")
        with store.db() as db:
            rows = db.execute("SELECT received_at,sample_at,payload FROM telemetry WHERE sample_at BETWEEN ? AND ? ORDER BY sample_at DESC,id DESC LIMIT ?", (start, end, limit)).fetchall()
        result = []
        for r in reversed(rows):
            p = json.loads(r["payload"])
            # Without a device clock the sample time is only the receive time (late for replays).
            result.append({"received_at": r["received_at"], "sample_at": r["sample_at"],
                           "time_source": "device" if p.get("measured_at") else "received", **p})
        return result

    @app.get("/api/v1/history", dependencies=[Depends(admin)])
    def history(start: float | None = None, end: float | None = None, limit: int = 1000):
        return history_rows(start, end, limit)

    @app.get("/api/v1/history.csv", dependencies=[Depends(admin)])
    def export(start: float | None = None, end: float | None = None):
        fields = ["sample_at", "time_source", "received_at", "battery_v", "temperature_c", "latitude", "longitude", "gps_valid", "speed_kmh", "motion", "tilt_deg", "fallen", "mode", "is_replay"]
        output = io.StringIO()
        writer = csv.DictWriter(output, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(history_rows(start, end, 10000))
        return StreamingResponse(iter([output.getvalue()]), media_type="text/csv", headers={"Content-Disposition": 'attachment; filename="mt15-history.csv"'})

    @app.get("/api/v1/alerts", dependencies=[Depends(admin)])
    def alerts():
        with store.db() as db:
            return [dict(r) for r in db.execute("SELECT * FROM alerts ORDER BY id DESC LIMIT 100")]

    @app.post("/api/v1/test-email")
    def test_email(session=Depends(admin)):
        if not mail_configured:
            raise HTTPException(409, "SMTP chưa cấu hình trong server/.env")
        now = time.time()
        with store.db() as db:
            if db.execute("SELECT 1 FROM alerts WHERE kind='test' AND created_at>?", (now - 60,)).fetchone():
                raise HTTPException(429, "Vừa gửi email thử; đợi một phút")
            db.execute("INSERT INTO alerts(created_at,kind,telemetry_id,message,mail_status) VALUES(?,?,?,?,?)",
                       (now, "test", None, f"Email thử từ MT15 Monitor ({cfg.device_id}). Không phải cảnh báo thật.", "pending"))
        return {"queued": True}

    @app.put("/api/v1/settings")
    def settings(body: Settings, session=Depends(admin)):
        with store.db() as db:
            db.execute("UPDATE settings SET revision=revision+1,armed=? WHERE id=1 AND armed!=?", (body.armed, body.armed))
            row = dict(db.execute("SELECT armed,revision FROM settings WHERE id=1").fetchone())
        return {"armed": bool(row["armed"]), "revision": row["revision"]}

    return app


# Lazy ASGI entry point keeps importing models possible without real credentials.
class LazyApp:
    instance = None

    async def __call__(self, scope, receive, send):
        if self.instance is None:
            self.instance = create_app()
        await self.instance(scope, receive, send)


app = LazyApp()
