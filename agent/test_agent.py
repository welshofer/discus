#!/usr/bin/env -S uv run --quiet
# /// script
# requires-python = ">=3.11"
# dependencies = ["psutil>=5.9", "certifi>=2024.0", "cryptography>=41"]
# ///
"""Tests for the discus agent's parsing, accounting and security logic.

These cover the parts that are subtle, load-bearing, and fail *silently*
when wrong -- Claude usage dedup especially, where missing the dedup
inflates every number ~3x in a way that still looks entirely plausible.

Run:  python3 -m unittest discover -s agent -v
"""

import calendar
import http.client
import importlib.util
import json
import os
import pathlib
import tempfile
import threading
import time
import types
import unittest

_SPEC = importlib.util.spec_from_file_location(
    "discus_agent", pathlib.Path(__file__).parent / "discus_agent.py")
agent = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(agent)


def iso(ts):
    """Epoch -> the UTC ISO form transcripts actually use."""
    return time.strftime("%Y-%m-%dT%H:%M:%S", time.gmtime(ts)) + ".000Z"


class TestHeadlines(unittest.TestCase):
    def test_strips_google_news_publisher_suffix(self):
        self.assertEqual(
            agent.clean_headline("Sunnyvale mother released by ICE - ktvu.com"),
            "Sunnyvale mother released by ICE")

    def test_collapses_whitespace(self):
        self.assertEqual(agent.clean_headline("  a\n\n b \t c  "), "a b c")

    def test_keeps_titles_with_no_suffix(self):
        t = "Anthropic boss Dario Amodei calls for AI development to slow down"
        self.assertEqual(agent.clean_headline(t), t)

    def test_handles_none_and_empty(self):
        self.assertEqual(agent.clean_headline(None), "")
        self.assertEqual(agent.clean_headline(""), "")

    def test_does_not_eat_a_long_trailing_clause(self):
        # The suffix pattern is bounded to 40 chars precisely so that a real
        # em-dash clause in a headline survives.
        t = "Markets rally - on hopes of a long and sustained economic recovery"
        self.assertEqual(agent.clean_headline(t), t)


class TestFeedParsing(unittest.TestCase):
    XML = """<?xml version="1.0"?><rss><channel>
      <item><title>First - Publisher</title></item>
      <item><title>Second</title></item>
      <item><title>Third</title></item>
      <item><title>Fourth</title></item>
    </channel></rss>"""

    def test_parses_and_limits(self):
        self.assertEqual(agent.parse_feed(self.XML, limit=2), ["First", "Second"])

    def test_raises_when_no_items(self):
        with self.assertRaises(RuntimeError):
            agent.parse_feed("<rss><channel></channel></rss>")


class TestQuoteParsing(unittest.TestCase):
    @staticmethod
    def doc(items):
        return {"FormattedQuoteResult": {"FormattedQuote": items}}

    def test_preserves_requested_order(self):
        d = self.doc([
            {"symbol": "AAPL", "last": "332.27", "change_pct": "+1.75%",
             "changetype": "UP", "curmktstatus": "REG_MKT"},
            {"symbol": "GOOG", "last": "335.45", "change_pct": "+1.53%",
             "changetype": "UP", "curmktstatus": "REG_MKT"},
        ])
        got = agent.parse_quotes(d, tickers=["GOOG", "AAPL"])
        self.assertEqual([q["sym"] for q in got], ["GOOG", "AAPL"])

    def test_single_quote_is_not_a_list(self):
        d = self.doc({"symbol": "AAPL", "last": "1,332.27", "change_pct": "+1.75%",
                      "changetype": "UP", "curmktstatus": "REG_MKT"})
        got = agent.parse_quotes(d, tickers=["AAPL"])
        self.assertEqual(len(got), 1)
        self.assertAlmostEqual(got[0]["last"], 1332.27)   # comma stripped

    def test_negative_change_is_down(self):
        d = self.doc([{"symbol": "MSFT", "last": "495.63", "change_pct": "-0.65%",
                       "changetype": "DOWN", "curmktstatus": "REG_MKT"}])
        q = agent.parse_quotes(d, tickers=["MSFT"])[0]
        self.assertAlmostEqual(q["pct"], -0.65)
        self.assertFalse(q["up"])

    def test_market_status_labels(self):
        cases = {"REG_MKT": "Open", "PRE_MKT": "Pre-market",
                 "POST_MKT": "Closed", "CLOSED": "Closed"}
        for raw, want in cases.items():
            d = self.doc([{"symbol": "AAPL", "last": "1", "change_pct": "0",
                           "changetype": "UP", "curmktstatus": raw}])
            self.assertEqual(agent.parse_quotes(d, tickers=["AAPL"])[0]["status"], want)

    def test_unknown_status_degrades_readably(self):
        d = self.doc([{"symbol": "AAPL", "last": "1", "change_pct": "0",
                       "changetype": "UP", "curmktstatus": "HALTED"}])
        self.assertEqual(agent.parse_quotes(d, tickers=["AAPL"])[0]["status"], "Halted")

    def test_raises_when_nothing_matches(self):
        with self.assertRaises(RuntimeError):
            agent.parse_quotes(self.doc([]), tickers=["AAPL"])


class TestStocksTTL(unittest.TestCase):
    """The cadence must track the market's own reported state, and a weekday
    after-hours session (thin but live trading) must not be confused with the
    weekend (nothing will move until Monday) -- same enum value, both times."""

    def setUp(self):
        self._real_time = agent.time

    def tearDown(self):
        agent.time = self._real_time
        agent._last_mkt_raw = ""

    def _ttl(self, raw, weekday):
        agent.time = types.SimpleNamespace(
            localtime=lambda *a: time.struct_time(
                (2026, 9, 14 + weekday, 17, 0, 0, weekday, 257, 1)),
            time=self._real_time.time,
            altzone=self._real_time.altzone,
            timezone=self._real_time.timezone,
            tzname=self._real_time.tzname)
        agent._last_mkt_raw = raw
        return agent.stocks_ttl()

    def test_open_is_fast_any_day(self):
        self.assertEqual(self._ttl("REG_MKT", 2), 15)

    def test_weekday_after_hours_is_medium(self):
        self.assertEqual(self._ttl("POST_MKT", 2), 60)
        self.assertEqual(self._ttl("PRE_MKT", 2), 60)

    def test_weekend_is_slow(self):
        self.assertEqual(self._ttl("POST_MKT", 5), 300)
        self.assertEqual(self._ttl("PRE_MKT", 6), 300)

    def test_closed_is_slow(self):
        self.assertEqual(self._ttl("CLOSED", 2), 300)


class TestClaudeUsage(unittest.TestCase):
    """The important one. One message.id spans several transcript rows
    (streaming deltas plus tool blocks) and most type:"user" rows are tool
    results, not prompts. Getting either wrong inflates the numbers several
    fold while still looking completely believable."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self._real_dir = agent.CLAUDE_DIR
        agent.CLAUDE_DIR = self.tmp.name
        agent._claude_files.clear()

    def tearDown(self):
        agent.CLAUDE_DIR = self._real_dir
        agent._claude_files.clear()
        self.tmp.cleanup()

    def write(self, name, rows):
        p = os.path.join(self.tmp.name, name)
        with open(p, "w") as f:
            for r in rows:
                f.write(json.dumps(r) + "\n")
        return p

    @staticmethod
    def assistant(ts, mid, inp=10, out=20, cc=30, cr=1000):
        return {"type": "assistant", "timestamp": iso(ts),
                "message": {"id": mid, "usage": {
                    "input_tokens": inp, "output_tokens": out,
                    "cache_creation_input_tokens": cc,
                    "cache_read_input_tokens": cr}}}

    @staticmethod
    def user(ts, pid, source=None, sidechain=False):
        return {"type": "user", "timestamp": iso(ts), "promptId": pid,
                "promptSource": source, "isSidechain": sidechain}

    def test_timestamps_parse_as_utc(self):
        now = int(time.time())
        p = self.write("a.jsonl", [self.assistant(now, "m1")])
        usage, _ = agent._parse_transcript(p)
        self.assertEqual(len(usage), 1)
        # within a second of the epoch we encoded -- proves timegm, not mktime
        self.assertAlmostEqual(usage[0][0], now, delta=1)

    def test_duplicate_message_ids_counted_once(self):
        now = int(time.time())
        self.write("a.jsonl", [
            self.assistant(now, "msg_1"),
            self.assistant(now, "msg_1"),     # streaming continuation
            self.assistant(now, "msg_1"),     # tool block
        ])
        t = agent.fetch_claude_usage()
        self.assertEqual(t["block_5h"]["msgs"], 1)
        self.assertEqual(t["block_5h"]["billable"], 60)   # not 180

    def test_dedup_spans_separate_transcripts(self):
        now = int(time.time())
        self.write("a.jsonl", [self.assistant(now, "msg_1")])
        self.write("b.jsonl", [self.assistant(now, "msg_1")])   # resumed session
        t = agent.fetch_claude_usage()
        self.assertEqual(t["block_5h"]["msgs"], 1)

    def test_only_typed_and_queued_rows_count_as_prompts(self):
        now = int(time.time())
        self.write("a.jsonl", [
            self.user(now, "p1", "typed"),
            self.user(now, "p1", None),       # tool result, same promptId
            self.user(now, "p1", None),
            self.user(now, "p2", "queued"),
            self.user(now, "p3", "system"),   # not user-initiated
        ])
        t = agent.fetch_claude_usage()
        self.assertEqual(t["block_5h"]["prompts"], 2)

    def test_sidechain_prompts_excluded(self):
        now = int(time.time())
        self.write("a.jsonl", [
            self.user(now, "p1", "typed"),
            self.user(now, "p2", "typed", sidechain=True),   # subagent
        ])
        self.assertEqual(agent.fetch_claude_usage()["block_5h"]["prompts"], 1)

    def test_old_activity_excluded_from_5h_window(self):
        now = int(time.time())
        self.write("a.jsonl", [
            self.assistant(now, "recent"),
            self.assistant(now - 6 * 3600, "old"),           # outside 5h
        ])
        t = agent.fetch_claude_usage()
        self.assertEqual(t["block_5h"]["msgs"], 1)
        self.assertEqual(t["month"]["msgs"], 2)

    def test_prompt_headroom_reported(self):
        now = int(time.time())
        self.write("a.jsonl", [self.user(now, "p1", "typed")])
        t = agent.fetch_claude_usage()
        self.assertEqual(t["prompt_used"], 1)
        self.assertEqual(t["prompt_limit"], agent.CLAUDE_PROMPT_LIMIT_5H)
        self.assertEqual(t["prompt_floor"], agent.CLAUDE_PROMPT_FLOOR_5H)

    def test_malformed_lines_are_skipped(self):
        now = int(time.time())
        p = os.path.join(self.tmp.name, "a.jsonl")
        with open(p, "w") as f:
            f.write('{"usage": not json at all\n')
            f.write(json.dumps(self.assistant(now, "ok")) + "\n")
        self.assertEqual(agent.fetch_claude_usage()["block_5h"]["msgs"], 1)


class TestPeriods(unittest.TestCase):
    def test_week_starts_monday(self):
        starts = agent._period_starts(time.time())
        self.assertEqual(time.localtime(starts["week"]).tm_wday, 0)

    def test_buckets_are_ordered_oldest_first(self):
        s = agent._period_starts(time.time())
        self.assertLessEqual(s["month"], s["week"])
        self.assertLessEqual(s["week"], s["today"])


class TestClock(unittest.TestCase):
    def test_offset_matches_local_time(self):
        c = agent.clock_info()
        local = time.localtime(c["epoch"])
        shifted = time.gmtime(c["epoch"] + c["utc_offset_s"])
        self.assertEqual((local.tm_hour, local.tm_min),
                         (shifted.tm_hour, shifted.tm_min))


def _fake_image(path, version=b"v1.2.3-4-gabcdef"):
    """Just enough of an ESP-IDF app image for firmware_info() to read."""
    head = bytearray(0x100)
    head[0x20:0x24] = (0xABCD5432).to_bytes(4, "little")
    head[0x30:0x30 + len(version)] = version
    head[0x50:0x56] = b"discus"
    with open(path, "wb") as f:
        f.write(bytes(head) + os.urandom(4096))


class TestConfig(unittest.TestCase):
    TOKEN = "t" * 24

    def test_defaults_to_loopback_without_token(self):
        self.assertEqual(agent.parse_config([], env={}),
                         ("127.0.0.1", agent.PORT, None))

    def test_lan_bind_without_token_refuses_to_start(self):
        for bind in ("0.0.0.0", "192.168.0.52", "", "mac.local"):
            with self.subTest(bind=bind):
                with self.assertRaises(SystemExit):
                    agent.parse_config(["--bind", bind], env={})

    def test_lan_bind_via_env_needs_token_too(self):
        with self.assertRaises(SystemExit):
            agent.parse_config([], env={"DISCUS_BIND": "0.0.0.0"})
        self.assertEqual(
            agent.parse_config([], env={"DISCUS_BIND": "0.0.0.0",
                                        "DISCUS_TOKEN": self.TOKEN}),
            ("0.0.0.0", agent.PORT, self.TOKEN))

    def test_token_file(self):
        with tempfile.NamedTemporaryFile("w", delete=False) as f:
            f.write(self.TOKEN + "\n")
        self.addCleanup(os.unlink, f.name)
        _, _, token = agent.parse_config(
            ["--bind", "0.0.0.0", "--token-file", f.name], env={})
        self.assertEqual(token, self.TOKEN)

    def test_short_token_rejected(self):
        with self.assertRaises(SystemExit):
            agent.parse_config(["--bind", "0.0.0.0"], env={"DISCUS_TOKEN": "abc"})

    def test_loopback_variants(self):
        for host in ("127.0.0.1", "127.0.0.53", "localhost", "::1"):
            self.assertTrue(agent.is_loopback(host), host)
        for host in ("0.0.0.0", "", "192.168.0.52", "mac.local"):
            self.assertFalse(agent.is_loopback(host), host)


class TestAuthorized(unittest.TestCase):
    def test_no_token_configured_allows_all(self):
        self.assertTrue(agent.authorized({}, None))

    def test_token_required_when_configured(self):
        tok = "s3cret-s3cret-s3cret"
        self.assertFalse(agent.authorized({}, tok))
        self.assertFalse(agent.authorized({agent.TOKEN_HEADER: "nope"}, tok))
        self.assertFalse(agent.authorized({agent.TOKEN_HEADER: tok[:-1]}, tok))
        self.assertTrue(agent.authorized({agent.TOKEN_HEADER: tok}, tok))


class _FirmwareDir(unittest.TestCase):
    """Points the agent's FIRMWARE_BIN/SIG at a temp dir for one test."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.bin = os.path.join(self.tmp.name, "discus.bin")
        self.sig = self.bin + ".sig"
        self._old = agent.FIRMWARE_BIN, agent.FIRMWARE_SIG
        agent.FIRMWARE_BIN, agent.FIRMWARE_SIG = self.bin, self.sig
        _fake_image(self.bin)

    def tearDown(self):
        agent.FIRMWARE_BIN, agent.FIRMWARE_SIG = self._old
        self.tmp.cleanup()

    def write_sig(self, data=b"\x30" + b"\x01" * 69, newer=True):
        with open(self.sig, "wb") as f:
            f.write(data)
        st = os.stat(self.bin)
        t = st.st_mtime_ns + (10**9 if newer else -10**9)
        os.utime(self.sig, ns=(t, t))


class TestFirmwareManifest(_FirmwareDir):
    def test_unsigned_image_is_not_offered(self):
        with self.assertRaisesRegex(RuntimeError, "not signed"):
            agent.firmware_info()

    def test_stale_signature_is_not_offered(self):
        self.write_sig(newer=False)
        with self.assertRaisesRegex(RuntimeError, "older than the image"):
            agent.firmware_info()

    def test_implausible_signature_rejected(self):
        self.write_sig(b"\x30" * 200)
        with self.assertRaisesRegex(RuntimeError, "implausible"):
            agent.firmware_info()

    def test_signed_manifest_carries_hex_signature(self):
        sig = b"\x30\x44" + bytes(range(68))
        self.write_sig(sig)
        info = agent.firmware_info()
        self.assertEqual(info["version"], "v1.2.3-4-gabcdef")
        self.assertEqual(bytes.fromhex(info["sig"]), sig)


class TestServerAuth(_FirmwareDir):
    """Real HTTP round trips against the handler, on loopback."""
    TOKEN = "integration-token-0123456789"

    def setUp(self):
        super().setUp()
        self.write_sig(b"\x30\x44" + bytes(68))
        self._old_token = agent.Handler.token
        agent.Handler.token = self.TOKEN
        self.srv = agent.ThreadingHTTPServer(("127.0.0.1", 0), agent.Handler)
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()

    def tearDown(self):
        self.srv.shutdown()
        self.srv.server_close()
        agent.Handler.token = self._old_token
        super().tearDown()

    def get(self, path, token=None):
        c = http.client.HTTPConnection("127.0.0.1", self.srv.server_port, timeout=5)
        headers = {agent.TOKEN_HEADER: token} if token else {}
        c.request("GET", path, headers=headers)
        r = c.getresponse()
        body = r.read()
        c.close()
        return r.status, body

    def test_every_endpoint_requires_the_token(self):
        for path in ("/", "/metrics", "/firmware.json", "/firmware.bin", "/nope"):
            with self.subTest(path=path):
                self.assertEqual(self.get(path)[0], 401)
                self.assertEqual(self.get(path, "wrong-token")[0], 401)

    def test_manifest_and_image_served_with_token(self):
        status, body = self.get("/firmware.json", self.TOKEN)
        self.assertEqual(status, 200)
        doc = json.loads(body)
        self.assertTrue(doc["ok"])
        self.assertTrue(doc["url"].endswith("/firmware.bin"))
        self.assertIn("sig", doc)

        status, body = self.get("/firmware.bin", self.TOKEN)
        self.assertEqual(status, 200)
        with open(self.bin, "rb") as f:
            self.assertEqual(body, f.read())


def _load_signer():
    try:
        import cryptography  # noqa: F401
    except ImportError:
        return None
    spec = importlib.util.spec_from_file_location(
        "ota_sign", pathlib.Path(__file__).parent / "ota_sign.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


signer = _load_signer()


@unittest.skipIf(signer is None, "cryptography not installed")
class TestOtaSigning(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        d = self.tmp.name
        self.key = os.path.join(d, "keys", "ota_signing_key.pem")
        self.pub = os.path.join(d, "ota_pubkey.pem")
        self.bin = os.path.join(d, "discus.bin")
        _fake_image(self.bin)
        signer.keygen(self.key, self.pub)

    def tearDown(self):
        self.tmp.cleanup()

    def test_private_key_is_owner_only(self):
        self.assertEqual(os.stat(self.key).st_mode & 0o777, 0o600)

    def test_keygen_never_overwrites(self):
        with self.assertRaises(SystemExit):
            signer.keygen(self.key, self.pub)

    def test_sign_then_verify(self):
        out = signer.sign(self.bin, self.key, self.pub)
        self.assertEqual(out, self.bin + ".sig")
        self.assertTrue(signer.verify(self.bin, self.pub))

    def test_tampered_image_fails(self):
        signer.sign(self.bin, self.key, self.pub)
        with open(self.bin, "r+b") as f:
            f.seek(0x200)
            b = f.read(1)
            f.seek(0x200)
            f.write(bytes([b[0] ^ 1]))
        self.assertFalse(signer.verify(self.bin, self.pub))

    def test_wrong_key_fails(self):
        other_key = os.path.join(self.tmp.name, "other.pem")
        other_pub = os.path.join(self.tmp.name, "other.pub.pem")
        signer.keygen(other_key, other_pub)
        signer.sign(self.bin, other_key)
        self.assertFalse(signer.verify(self.bin, self.pub))

    def test_sign_refuses_key_that_does_not_match_firmware_pubkey(self):
        other_key = os.path.join(self.tmp.name, "other.pem")
        signer.keygen(other_key, os.path.join(self.tmp.name, "x.pem"))
        with self.assertRaises(SystemExit):
            signer.sign(self.bin, other_key, self.pub)
        self.assertFalse(os.path.exists(self.bin + ".sig"))

    def test_signature_is_der_and_fits_the_device_buffer(self):
        signer.sign(self.bin, self.key, self.pub)
        with open(self.bin + ".sig", "rb") as f:
            sig = f.read()
        self.assertEqual(sig[0], 0x30)          # ASN.1 SEQUENCE, as mbedTLS wants
        self.assertLessEqual(len(sig), 72)      # SIG_MAX on the device is 80

    def test_agent_serves_what_the_signer_wrote(self):
        old = agent.FIRMWARE_BIN, agent.FIRMWARE_SIG
        agent.FIRMWARE_BIN, agent.FIRMWARE_SIG = self.bin, self.bin + ".sig"
        try:
            signer.sign(self.bin, self.key, self.pub)
            info = agent.firmware_info()
        finally:
            agent.FIRMWARE_BIN, agent.FIRMWARE_SIG = old
        sig_file = os.path.join(self.tmp.name, "from_manifest.sig")
        with open(sig_file, "wb") as f:
            f.write(bytes.fromhex(info["sig"]))
        self.assertTrue(signer.verify(self.bin, self.pub, sig_file))


if __name__ == "__main__":
    unittest.main(verbosity=2)
