#!/usr/bin/env -S uv run --quiet
# /// script
# requires-python = ">=3.11"
# dependencies = ["psutil>=5.9", "certifi>=2024.0"]
# ///
"""
discus agent — collects everything the round display shows and serves it as
one small JSON document.

Design notes
------------
All network calls happen HERE, not on the ESP32-P4. The device gets a single
plaintext HTTP GET and renders the result. That keeps TLS, API keys, rate
limiting and flaky-upstream handling on the machine that is good at them.

Every source is cached with its own TTL and refreshed by a background thread,
so an HTTP request from the device never blocks on an upstream API. If a
source fails we keep serving the last good value and mark it stale rather
than dropping the panel.

Run:  ./agent/discus_agent.py          (uv installs psutil on first run)
Test: curl -s localhost:8787/metrics | python3 -m json.tool
"""

import calendar
import json
import os
import re
import socket
import sys
import threading
import time
import urllib.parse
import urllib.request
import ssl
import xml.etree.ElementTree as ET
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import certifi
import psutil

# Explicit trust store. Do not rely on the interpreter's default -- several
# macOS Python builds have no usable CA bundle at all.
SSL_CTX = ssl.create_default_context(cafile=certifi.where())

PORT = 8787

UA = ("Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
      "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120 Safari/537.36")

TICKERS = ["GOOG", "MSFT", "AAPL"]

# socket.gethostname() gives "MacBook-Pro-16---M5-Max", which is noise on a
# glanceable panel. Set None to fall back to the raw hostname.
HOST_LABEL = "MacBook Pro"

# CNBC's curmktstatus values, mapped to something readable from a distance.
# Note POST_MKT covers both weekday after-hours AND the whole weekend (CNBC
# reports the last session's state rather than a separate weekend value), so
# "Closed" is the honest label for the common case.
MARKET_STATUS = {
    "REG_MKT":  "Open",
    "PRE_MKT":  "Pre-market",
    "POST_MKT": "Closed",
    "CLOSED":   "Closed",
}
MAX_HEADLINES = 3

# Local news is a Google News geo-query -- no API key, and it follows
# whatever town you name here.
LOCAL_QUERY = "Sunnyvale OR 94087 California"

FEEDS = [
    {"key": "world", "label": "WORLD", "source": "BBC",
     "url": "https://feeds.bbci.co.uk/news/world/rss.xml"},
    {"key": "us", "label": "US", "source": "NPR",
     "url": "https://feeds.npr.org/1001/rss.xml"},
    {"key": "tech", "label": "TECH", "source": "ARS TECHNICA",
     "url": "https://feeds.arstechnica.com/arstechnica/index"},
    {"key": "local", "label": "SUNNYVALE", "source": "GOOGLE NEWS",
     "url": "https://news.google.com/rss/search?q="
            + urllib.parse.quote(LOCAL_QUERY)
            + "&hl=en-US&gl=US&ceid=US:en"},
]

# Open-Meteo WMO weather codes, condensed to something that fits a round screen.
WMO = {
    0: "Clear", 1: "Mostly clear", 2: "Partly cloudy", 3: "Overcast",
    45: "Fog", 48: "Rime fog",
    51: "Light drizzle", 53: "Drizzle", 55: "Heavy drizzle",
    56: "Freezing drizzle", 57: "Freezing drizzle",
    61: "Light rain", 63: "Rain", 65: "Heavy rain",
    66: "Freezing rain", 67: "Freezing rain",
    71: "Light snow", 73: "Snow", 75: "Heavy snow", 77: "Snow grains",
    80: "Light showers", 81: "Showers", 82: "Violent showers",
    85: "Snow showers", 86: "Snow showers",
    95: "Thunderstorm", 96: "Thunderstorm, hail", 99: "Thunderstorm, hail",
}


def http_json(url, timeout=15):
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    with urllib.request.urlopen(req, timeout=timeout, context=SSL_CTX) as r:
        return json.loads(r.read().decode("utf-8", "replace"))


def http_text(url, timeout=15):
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    with urllib.request.urlopen(req, timeout=timeout, context=SSL_CTX) as r:
        return r.read().decode("utf-8", "replace")


class Cached:
    """One upstream source, refreshed on a TTL, never raising to the caller."""

    def __init__(self, name, fn, ttl):
        self.name = name
        self.fn = fn
        # int, or a callable returning one -- lets a source slow itself down
        # when its upstream data cannot be changing.
        self._ttl = ttl
        self.value = None
        self.fetched_at = 0.0
        self.error = None
        self.lock = threading.Lock()

    @property
    def ttl(self):
        return self._ttl() if callable(self._ttl) else self._ttl

    def stale(self):
        return (time.time() - self.fetched_at) > self.ttl

    def refresh(self, force=False):
        if not force and not self.stale():
            return
        try:
            v = self.fn()
            with self.lock:
                self.value = v
                self.fetched_at = time.time()
                self.error = None
        except Exception as e:                       # noqa: BLE001
            with self.lock:
                self.error = f"{type(e).__name__}: {e}"
            print(f"[{self.name}] {self.error}", file=sys.stderr)

    def snapshot(self):
        with self.lock:
            age = int(time.time() - self.fetched_at) if self.fetched_at else None
            return self.value, age, self.error


# ── sources ───────────────────────────────────────────────────────────────

def fetch_location():
    d = http_json("http://ip-api.com/json/?fields=status,city,regionName,"
                  "countryCode,lat,lon")
    if d.get("status") != "success":
        raise RuntimeError("ip-api lookup failed")
    return {"city": d.get("city") or "?", "region": d.get("regionName") or "",
            "country": d.get("countryCode") or "", "lat": d["lat"], "lon": d["lon"]}


def fetch_weather():
    loc, _, _ = LOCATION.snapshot()
    if not loc:
        LOCATION.refresh(force=True)
        loc, _, _ = LOCATION.snapshot()
    if not loc:
        raise RuntimeError("no location")

    url = ("https://api.open-meteo.com/v1/forecast"
           f"?latitude={loc['lat']}&longitude={loc['lon']}"
           "&current=temperature_2m,apparent_temperature,relative_humidity_2m,"
           "weather_code,wind_speed_10m"
           "&daily=temperature_2m_max,temperature_2m_min"
           "&timezone=auto&forecast_days=1")
    d = http_json(url)
    cur, daily = d["current"], d["daily"]
    code = int(cur["weather_code"])
    c = float(cur["temperature_2m"])
    return {
        "city": loc["city"],
        "country": loc["country"],
        "code": code,
        "text": WMO.get(code, f"code {code}"),
        "temp_c": round(c, 1),
        "temp_f": round(c * 9 / 5 + 32, 1),
        "feels_c": round(float(cur["apparent_temperature"]), 1),
        "humidity": int(cur["relative_humidity_2m"]),
        "wind_kph": round(float(cur["wind_speed_10m"]), 1),
        "hi_c": round(float(daily["temperature_2m_max"][0]), 1),
        "lo_c": round(float(daily["temperature_2m_min"][0]), 1),
        # US gets Fahrenheit; everyone else Celsius.
        "unit": "F" if loc["country"] == "US" else "C",
    }


# Last raw curmktstatus seen, used to pick the polling cadence. Deriving this
# from the feed itself rather than a hardcoded 9:30-16:00 ET schedule means
# exchange holidays and early closes are handled for free -- no calendar to
# maintain and nothing to get wrong once a year.
_last_mkt_raw = ""


def stocks_ttl():
    if _last_mkt_raw == "REG_MKT":
        return 15                      # regular session: prices move constantly
    # Pre/after-hours on a weekday still trade, just thinly.
    if _last_mkt_raw in ("PRE_MKT", "POST_MKT") and time.localtime().tm_wday < 5:
        return 60
    return 300                         # closed, or the weekend


def _to_float(v, dflt=0.0):
    try:
        return float(str(v or "").replace(",", "").replace("%", "").replace("+", ""))
    except ValueError:
        return dflt


def parse_quotes(doc, tickers=None):
    """CNBC quote payload -> list of quote dicts, in the caller's order.

    Also records the raw market status so stocks_ttl() can pick a cadence."""
    global _last_mkt_raw
    tickers = TICKERS if tickers is None else tickers

    q = doc["FormattedQuoteResult"]["FormattedQuote"]
    if isinstance(q, dict):                  # single-symbol responses are not a list
        q = [q]
    by_sym = {it.get("symbol"): it for it in q}

    out = []
    for sym in tickers:                      # preserve the requested order
        it = by_sym.get(sym)
        if not it:
            continue
        pct = _to_float(it.get("change_pct"))
        raw_status = (it.get("curmktstatus") or "").upper()
        _last_mkt_raw = raw_status
        out.append({
            "sym": sym,
            "last": _to_float(it.get("last")),
            "pct": pct,
            "up": (it.get("changetype", "").upper() != "DOWN") and pct >= 0,
            "status": MARKET_STATUS.get(raw_status, raw_status.title()),
        })
    if not out:
        raise RuntimeError("no quotes parsed")
    return out


def fetch_stocks():
    syms = "|".join(TICKERS)
    url = ("https://quote.cnbc.com/quote-html-webservice/restQuote/symbolType/"
           f"symbol?symbols={urllib.parse.quote(syms)}&requestMethod=itv&noform=1"
           "&partnerId=2&fund=1&exthrs=1&output=json")
    return parse_quotes(http_json(url))


def clean_headline(title):
    """Normalise whitespace and drop Google News' trailing " - Publisher".

    That suffix appears on every Google News item, costs a whole line on a
    470px-wide label, and the source is already shown in the panel header."""
    t = re.sub(r"\s+", " ", (title or "").strip())
    return re.sub(r"\s+-\s+[^-]{2,40}$", "", t)


def parse_feed(xml_text, limit=None):
    """RSS/Atom-ish XML -> list of cleaned headline strings."""
    limit = MAX_HEADLINES if limit is None else limit
    root = ET.fromstring(xml_text)
    items = []
    for item in root.iterfind(".//item"):
        t = clean_headline(item.findtext("title"))
        if t:
            items.append(t)
        if len(items) >= limit:
            break
    if not items:
        raise RuntimeError("no headlines parsed")
    return items


def make_news_fetcher(feed):
    def fetch():
        return parse_feed(http_text(feed["url"]))
    return fetch



# ── Claude Code usage ─────────────────────────────────────────────────────
# Computed from the local transcripts in ~/.claude/projects/**/*.jsonl.
#
# IMPORTANT: nothing on disk records your subscription quota, so "remaining"
# is only meaningful against a budget you set yourself. Leave these None to
# show raw consumption with no percentage ring.
CLAUDE_DIR = os.path.expanduser("~/.claude/projects")

# Anthropic does NOT publish a fixed token bucket, and throttles dynamically
# by system load, context length and model tier. The documented unit is
# prompts per rolling 5-hour window: roughly 200-900 on Max 20x, the low end
# for heavy agentic/multi-agent work and the high end for light chat.
#
# So the ring is drawn against the optimistic ceiling and the panel also
# shows the lower bound, because a single number here would be false
# precision. Note also that claude.ai, the desktop app and Claude Code all
# draw from the SAME pool -- this counts only what Claude Code wrote locally,
# so it is a floor on your real usage, never the whole picture.
CLAUDE_PROMPT_LIMIT_5H = 900
CLAUDE_PROMPT_FLOOR_5H = 200

CLAUDE_BUDGET_5H = None        # optional token budgets; usually leave None
CLAUDE_BUDGET_WEEK = None
CLAUDE_BUDGET_MONTH = None

# path -> (size, mtime, usage_recs, prompt_recs)
_claude_files = {}


def _ts(d):
    """Transcript timestamps are UTC. calendar.timegm converts a UTC
    struct_time correctly; time.mktime would treat it as local time, and
    hand-correcting with time.timezone ignores DST (a silent one-hour error
    for half the year, which would shift every window boundary)."""
    ts = d.get("timestamp")
    if not ts:
        return None
    try:
        return calendar.timegm(time.strptime(ts[:19], "%Y-%m-%dT%H:%M:%S"))
    except Exception:                                    # noqa: BLE001
        return None


def _parse_transcript(path):
    """Return (usage_recs, prompt_recs).

    usage_recs:  (ts, message_id, billable_tokens, cache_read_tokens)
    prompt_recs: (ts, promptId) for human-initiated prompts only

    Only metadata is read -- never message content."""
    usage, prompts = [], []
    try:
        with open(path, "r", errors="replace") as f:
            for line in f:
                if '"usage"' not in line and '"promptId"' not in line:
                    continue
                try:
                    d = json.loads(line)
                except Exception:                        # noqa: BLE001
                    continue
                t = _ts(d)
                if t is None:
                    continue

                # Human prompts. User rows also carry tool results, which
                # share the originating promptId but have promptSource unset
                # -- counting those would inflate the number many times over.
                if d.get("type") == "user" and not d.get("isSidechain"):
                    if d.get("promptSource") in ("typed", "queued"):
                        pid = d.get("promptId")
                        if pid:
                            prompts.append((t, pid))

                m = d.get("message")
                if not isinstance(m, dict):
                    continue
                u = m.get("usage")
                if not isinstance(u, dict):
                    continue
                billable = ((u.get("input_tokens") or 0)
                            + (u.get("output_tokens") or 0)
                            + (u.get("cache_creation_input_tokens") or 0))
                usage.append((t, m.get("id") or d.get("requestId") or "",
                              billable, u.get("cache_read_input_tokens") or 0))
    except OSError:
        pass
    return usage, prompts


def _period_starts(now):
    lt = time.localtime(now)
    midnight = time.mktime((lt.tm_year, lt.tm_mon, lt.tm_mday, 0, 0, 0, 0, 0, -1))
    week = midnight - lt.tm_wday * 86400                 # Monday
    month = time.mktime((lt.tm_year, lt.tm_mon, 1, 0, 0, 0, 0, 0, -1))
    return {"block_5h": now - 5 * 3600, "today": midnight,
            "week": week, "month": month}


def fetch_claude_usage():
    now = time.time()
    starts = _period_starts(now)
    oldest = min(starts.values())

    seen_paths = set()
    for root, _dirs, files in os.walk(CLAUDE_DIR):
        for fn in files:
            if not fn.endswith(".jsonl"):
                continue
            fp = os.path.join(root, fn)
            try:
                st = os.stat(fp)
            except OSError:
                continue
            # A file last touched before the oldest bucket cannot contribute.
            if st.st_mtime < oldest:
                _claude_files.pop(fp, None)
                continue
            seen_paths.add(fp)
            cached = _claude_files.get(fp)
            if cached and cached[0] == st.st_size and cached[1] == st.st_mtime:
                continue
            usage, prompts = _parse_transcript(fp)
            _claude_files[fp] = (st.st_size, st.st_mtime, usage, prompts)
    for gone in set(_claude_files) - seen_paths:
        _claude_files.pop(gone, None)

    # One message id spans several transcript rows (streaming + tool blocks),
    # so dedup globally or every number is inflated severalfold.
    seen_ids = set()
    totals = {k: {"billable": 0, "cache_read": 0, "msgs": 0, "prompts": 0}
              for k in starts}
    for _sz, _mt, recs, _pr in _claude_files.values():
        for t, mid, billable, cread in recs:
            if mid and mid in seen_ids:
                continue
            if mid:
                seen_ids.add(mid)
            for name, start in starts.items():
                if t >= start:
                    b = totals[name]
                    b["billable"] += billable
                    b["cache_read"] += cread
                    b["msgs"] += 1

    # Prompts, deduped by promptId across every transcript.
    seen_prompts = {k: set() for k in starts}
    for _sz, _mt, _recs, prompts in _claude_files.values():
        for t, pid in prompts:
            for name, start in starts.items():
                if t >= start and pid not in seen_prompts[name]:
                    seen_prompts[name].add(pid)
                    totals[name]["prompts"] += 1

    budgets = {"block_5h": CLAUDE_BUDGET_5H, "week": CLAUDE_BUDGET_WEEK,
               "month": CLAUDE_BUDGET_MONTH}
    for name, b in totals.items():
        lim = budgets.get(name)
        b["budget"] = lim
        b["pct"] = round(100.0 * b["billable"] / lim, 1) if lim else None

    # Seconds until the rolling 5h window has fully turned over.
    totals["block_5h"]["window_s"] = 5 * 3600

    # Prompt headroom for the rolling 5h window. The ceiling is a documented
    # range, not a constant, so expose both ends and let the UI say so.
    used = totals["block_5h"]["prompts"]
    totals["prompt_limit"] = CLAUDE_PROMPT_LIMIT_5H
    totals["prompt_floor"] = CLAUDE_PROMPT_FLOOR_5H
    totals["prompt_used"] = used
    totals["prompt_pct"] = round(100.0 * used / CLAUDE_PROMPT_LIMIT_5H, 1)
    totals["sessions"] = len(_claude_files)
    return totals


LOCATION = Cached("location", fetch_location, ttl=6 * 3600)
WEATHER  = Cached("weather",  fetch_weather,  ttl=600)
STOCKS   = Cached("stocks",   fetch_stocks,   ttl=stocks_ttl)
CLAUDE   = Cached("claude",   fetch_claude_usage, ttl=300)

NEWS_SOURCES = [
    (f, Cached("news:" + f["key"], make_news_fetcher(f), ttl=1800)) for f in FEEDS
]
SOURCES = [LOCATION, WEATHER, STOCKS, CLAUDE] + [c for _f, c in NEWS_SOURCES]


# On macOS "/" is the sealed, read-only system volume and always reads ~0%
# used. Everything the user actually fills up lives on the Data volume.
DISK_PATH = "/System/Volumes/Data" if os.path.isdir("/System/Volumes/Data") else "/"

_cpu_pct = 0.0


def cpu_sampler():
    """psutil.cpu_percent() measures since the previous call, so calling it
    ad hoc from a request handler returns whatever window happened to elapse
    (often ~0). Sample on a fixed 1s interval in the background instead."""
    global _cpu_pct
    while True:
        _cpu_pct = psutil.cpu_percent(interval=1.0)


def system_metrics():
    """Live, uncached - the one thing that must always be instantaneous."""
    vm = psutil.virtual_memory()
    du = psutil.disk_usage(DISK_PATH)
    return {
        "cpu": round(_cpu_pct, 1),
        "mem": round(vm.percent, 1),
        "disk": round(du.percent, 1),
        "mem_used_gb": round(vm.used / 1e9, 1),
        "mem_total_gb": round(vm.total / 1e9, 1),
        "disk_free_gb": round(du.free / 1e9, 1),
        "host": HOST_LABEL or socket.gethostname().split(".")[0],
        "uptime_s": int(time.time() - psutil.boot_time()),
        "load1": round(psutil.getloadavg()[0], 2),
    }


def clock_info():
    """Wall-clock time for the device.

    We send a UTC epoch plus the *current* offset rather than a POSIX TZ
    string: newlib on the ESP32 has no tzdata, so "America/Los_Angeles"
    means nothing to it, and hand-building a POSIX rule string with DST
    transitions is easy to get subtly wrong. Recomputing the offset here on
    every poll means DST changes itself and the device never has to know."""
    lt = time.localtime()
    is_dst = lt.tm_isdst > 0
    return {
        "epoch": int(time.time()),
        "utc_offset_s": -(time.altzone if is_dst else time.timezone),
        "tz": time.tzname[1 if is_dst else 0],
    }


def build_payload():
    weather, w_age, w_err = WEATHER.snapshot()
    stocks, s_age, s_err = STOCKS.snapshot()
    claude, c_age, c_err = CLAUDE.snapshot()

    news = {}
    for feed, cache in NEWS_SOURCES:
        items, age, err = cache.snapshot()
        news[feed["key"]] = {
            "ok": items is not None, "age_s": age, "err": err,
            "label": feed["label"], "source": feed["source"],
            "items": items or [],
        }
    return {
        "ts": int(time.time()),
        "clock": clock_info(),
        "system": system_metrics(),
        "weather": {"ok": weather is not None, "age_s": w_age,
                    "err": w_err, **(weather or {})},
        "stocks": {"ok": stocks is not None, "age_s": s_age,
                   "err": s_err, "ttl_s": STOCKS.ttl, "mkt_raw": _last_mkt_raw,
                   "items": stocks or []},
        "news": news,
        "claude": {"ok": claude is not None, "age_s": c_age,
                   "err": c_err, **(claude or {})},
    }


_hits = {"count": 0, "last_client": "", "last_ts": 0}


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self):                                        # noqa: N802
        if self.path.split("?")[0] not in ("/metrics", "/"):
            self.send_error(404, "try /metrics")
            return
        _hits["count"] += 1
        _hits["last_client"] = self.client_address[0]
        _hits["last_ts"] = int(time.time())
        payload = build_payload()
        payload["served"] = dict(_hits)
        body = json.dumps(payload, separators=(",", ":")).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        pass            # the device polls constantly; don't spam the terminal


def refresher():
    while True:
        for s in SOURCES:
            s.refresh()
        time.sleep(5)


def lan_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    except Exception:                                        # noqa: BLE001
        return "127.0.0.1"
    finally:
        s.close()


def main():
    threading.Thread(target=cpu_sampler, daemon=True).start()
    for s in SOURCES:
        s.refresh(force=True)
    threading.Thread(target=refresher, daemon=True).start()

    srv = ThreadingHTTPServer(("0.0.0.0", PORT), Handler)
    print(f"discus agent on http://{lan_ip()}:{PORT}/metrics")
    for s in SOURCES:
        _, age, err = s.snapshot()
        print(f"  {s.name:<9} {'ok' if err is None else 'FAILED: ' + err}")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\nbye")


if __name__ == "__main__":
    main()
