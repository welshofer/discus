# discus

[![build](https://github.com/welshofer/discus/actions/workflows/build.yml/badge.svg)](https://github.com/welshofer/discus/actions/workflows/build.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.5-red)
![LVGL](https://img.shields.io/badge/LVGL-9.5-green)

A nine-panel dashboard on a 4-inch round touchscreen, driven by an
**ESP32-P4**. Swipe between panels or let it rotate on its own every 90
seconds.

> Clock · Mac system load · Weather · Markets · World / US / Tech / Local
> news · Claude Code usage

The device is a **renderer**, nothing more. A small Python agent on your Mac
collects everything — host stats, weather, stock quotes, RSS headlines, local
Claude Code token usage — and serves it as one JSON document. The board does
a single plaintext HTTP GET every three seconds and draws the result. No API
keys and no TLS stack live on the microcontroller; its only secrets are the
WiFi password and a shared token for the agent. Firmware updates are
signature-checked on the device, so the plain link is never trusted with
what code runs.

Built with ESP-IDF 5.5 and LVGL 9.5. No image assets: the weather icons are
drawn from primitives and the clock face uses a purpose-built 128px font
containing eleven glyphs.

**If you have this board, start with [Gotchas discovered the hard
way](#gotchas-discovered-the-hard-way).** Several of them (silicon revision,
inverted LVGL lock semantics, PSRAM off by default) will stop you cold
otherwise.

## Hardware (verified against BSP source + live boot log)

| Subsystem | Detail |
|---|---|
| MCU | ESP32-P4, **silicon revision v1.3** (pre-3.0 line) |
| PSRAM | 32 MB, 16-line HEX @ 200 MHz (confirmed at boot) |
| Flash | 32 MB NOR (GigaDevice) |
| Display | 720x720 round IPS, **JD9365** driver, MIPI-DSI 2-lane @ 1500 Mbps |
| Touch | **GT911** @ I2C `0x5d`, 10-point |
| I2C | SDA=`GPIO7`, SCL=`GPIO8` (shared: touch + codec) |
| Audio | ES8311 codec + ES7210 echo-cancel; I2S MCLK=`13` SCLK=`12` WS=`10` DOUT=`9` DIN=`11` |
| Amp enable | `GPIO53` — must be driven high or there is no sound |
| Backlight | `GPIO26` (LEDC PWM); LCD reset `GPIO27` |
| microSD | 4-bit SDIO: D0-D3=`39,40,41,42`, CMD=`44`, CLK=`43` |
| Wi-Fi/BLE | ESP32-C6-MINI-1 over SDIO: CLK=`18` CMD=`19` D0-D3=`14-17`, reset=`54` |

No IMU and no buttons — the BSP declares `BSP_CAPS_IMU 0` / `BSP_CAPS_BUTTONS 0`.

## Architecture

```
┌─────────────────────────┐                  ┌──────────────────────┐
│  agent/discus_agent.py  │   HTTP + JSON    │   ESP32-P4           │
│  (your Mac)             │ ◄─────────────── │   polls every 3s     │
│                         │  :8787/metrics   │   renders 5 panels   │
│  psutil    host stats   │                  │                      │
│  open-meteo  weather    │                  │   no keys, no TLS    │
│  CNBC        quotes     │                  │   on the device      │
│  BBC RSS     headlines  │                  │                      │
│  ~/.claude   token use  │                  │                      │
└─────────────────────────┘                  └──────────────────────┘
```

### Refresh cadence

Two independent stages. The device polls the agent every **3 s**; the agent
refreshes each upstream on its own TTL, swept by a background thread every
5 s. Worst-case age on screen is `TTL + ~8 s`.

| Data | TTL |
|---|---|
| Mac CPU / mem / disk | live, never cached (CPU is a rolling 1 s average) |
| Stocks | **adaptive** -- 15 s open, 60 s weekday pre/after-hours, 300 s otherwise |
| Claude prompt usage | 300 s |
| Weather | 600 s |
| News x4 | 1800 s each |
| IP geolocation | 6 h |

The stock cadence is derived from the market's *own* reported
`curmktstatus`, not a hardcoded 9:30-16:00 ET schedule -- so exchange
holidays and early closes are handled for free, with no calendar to maintain.

The device **polls**; the Mac does not push. That means no held socket and no
reconnect state machine on the MCU, and the whole contract is reproducible
with `curl`. Every upstream API call, TLS handshake and API key stays on the
Mac. Each source is cached on its own TTL and refreshed by a background
thread, so a device request never blocks on a slow upstream.

### Tests

```sh
uv run agent/test_agent.py         # 50 tests, no network required
```

Focused on logic that fails *silently*: Claude usage dedup, the Google News
suffix regex, market-status mapping and the adaptive stock cadence. Verified
by mutation testing -- deliberately reintroducing each of six known bugs
(dropping `message.id` dedup, counting tool results as prompts, including
sidechain prompts, `timegm` -> `mktime`, dropping the headline regex, losing
the weekday/weekend split) makes the suite fail.

The security side is covered the same way: the token check on every
endpoint (over real loopback HTTP), refusing a LAN bind without a token,
refusing to offer an unsigned or stale-signed image, and sign/verify/tamper
round trips for `agent/ota_sign.py`. Removing any of those checks fails the
suite.

### Panels

| # | Panel | Source |
|---|---|---|
| 1 | Clock + seconds ring | agent epoch, device keeps ticking |
| 2 | Mac CPU / memory / disk | `psutil` |
| 3 | Weather + drawn icon | Open-Meteo, located by IP |
| 4 | GOOG / MSFT / AAPL | CNBC quote endpoint |
| 5 | World news | BBC World RSS |
| 6 | US news | NPR |
| 7 | Tech news | Ars Technica |
| 8 | Local news | Google News geo-query (`LOCAL_QUERY`) |
| 9 | Claude prompt usage | `~/.claude/projects/**/*.jsonl` |

Swipe horizontally to move between them; they wrap around at both ends. With
no touch input the carousel advances every 90 seconds, and any touch resets
that dwell timer.

### Running the agent

```sh
uv run agent/discus_agent.py          # installs psutil + certifi on first run
curl -s localhost:8787/metrics | python3 -m json.tool
```

By default the agent listens on **127.0.0.1 only**. That is safe but the
display cannot reach it, so for real use you opt in to the LAN explicitly,
and a LAN bind **requires a shared token** -- the agent refuses to start
without one:

```sh
python3 -c 'import secrets; print(secrets.token_urlsafe(24))'   # make a token
export DISCUS_TOKEN=<that token>
uv run agent/discus_agent.py --bind 0.0.0.0        # or DISCUS_BIND=0.0.0.0
curl -s -H "X-Discus-Token: $DISCUS_TOKEN" <mac-ip>:8787/metrics
```

`--token-file PATH` reads the token from a file instead of the environment.
Every endpoint (`/metrics`, `/firmware.json`, `/firmware.bin`) answers 401
without the right `X-Discus-Token` header. That matters because between them
they expose your host stats, location, Claude usage and a firmware image
with your WiFi password compiled into it. Put the same token in
`main/wifi_creds.h` as `DISCUS_AGENT_TOKEN_OVERRIDE` so the device sends it.

The token travels over plain HTTP, so it keeps out casual LAN clients, not
someone actively intercepting your WiFi traffic. Firmware integrity does
not depend on it: that is the signature's job (below).

The device must be pointed at your Mac's LAN address via
`CONFIG_DISCUS_AGENT_URL` (`idf.py menuconfig` -> Discus Dashboard).

### When the Mac goes away

After 12 s without a successful fetch (two missed 3 s polls), the panels dim
behind a scrim and the status line turns amber: `stale • 2h ago`. Numbers
that have stopped moving must not keep looking authoritative -- otherwise an
overnight sleep leaves a confident "cpu 34%" on screen that is eight hours
old. The device keeps retrying every 3 s and recovers on its own; verified by
stopping the agent for 45 s (10 failed polls, no panic) and restarting it.

Note `sleep 0` in your AC `pmset` profile means the Mac never system-sleeps
while plugged in, so this path is mostly a battery/unplugged concern. Power
Nap does **not** keep a custom LaunchAgent socket serviced.

Also worth decoupling: if the board is powered from the Mac's USB port, a
Mac sleep can cut power entirely and the display goes dark rather than
stale. A separate 5V/3A brick avoids that.

### The clock

The agent sends a UTC `epoch` plus the **current** `utc_offset_s` (DST
already applied) rather than a POSIX TZ string, because newlib on the ESP32
ships no tzdata -- `America/Los_Angeles` means nothing to it, and
hand-building a POSIX rule with DST transitions is easy to get subtly wrong.
Recomputing the offset on the Mac every poll means DST handles itself.

The device calls `settimeofday()` only when drift exceeds 2s; re-setting on
every 3s poll would make the seconds ring stutter backwards. Between
corrections the ESP32 keeps its own time, so **the clock carries on ticking
while the Mac is asleep** -- it is the one panel that stays useful when the
agent is unreachable. Local fields come from `gmtime_r(now + offset)`, which
sidesteps tzdata entirely.

### Over-the-air updates

The agent serves `/firmware.json` and `/firmware.bin` straight out of
`build/discus.bin`. The device compares the manifest version against its own
`esp_app_get_description()->version` -- both read the same string ESP-IDF
bakes in from `git describe`, so there is no separate version file to bump.
Rebuild and the update publishes itself; the device picks it up within five
minutes.

```sh
./dev.sh build          # builds, then signs build/discus.bin
```

### Signed updates

The download is plain HTTP, so the device trusts none of it. While streaming
the image into the spare slot it hashes it, and only if an **ECDSA-P256 /
SHA-256 signature** over those exact bytes verifies against the public key
compiled into the *running* firmware does it call
`esp_ota_set_boot_partition()`. A bad or missing signature leaves the current
slot booting and wipes the rejected image's header so not even a rollback can
select it. A build with no public key refuses every update.

One-time setup:

```sh
python3 agent/ota_sign.py keygen    # or: uv run agent/ota_sign.py keygen
#   keys/ota_signing_key.pem  private -- gitignored, never commit, back it up
#   main/ota_pubkey.pem       public  -- commit this
git add main/ota_pubkey.pem
./dev.sh fullclean build            # CMake must re-run to embed the new key
./dev.sh flash                      # once, over USB (see below)
```

After that, `./dev.sh build` signs every image it produces with
`keys/ota_signing_key.pem` (override with `DISCUS_OTA_KEY`), writing
`build/discus.bin.sig`; the agent puts the signature in `/firmware.json` and
will not offer an image whose signature is missing or older than the image.
`agent/ota_sign.py sign` also checks the signature against
`main/ota_pubkey.pem`, so a key mismatch is caught on the Mac rather than as
a silently refused update. `agent/ota_sign.py verify` checks an image by hand.

> **One more cable.** Firmware from before signed updates installs anything
> it is served, and cannot check a signature. The first signed build has to
> go on over USB with `./dev.sh flash`; every update after that is wireless
> and verified. Lose the private key and the same applies: generate a new
> pair and reflash over USB.

Why not ESP-IDF's built-in `CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT`? It
trusts whichever key signed the *running* image's signature block, and it
either signs at build time (so CI would need the private key) or needs a
separate `espsecure.py` step. A small explicit check in `discus_ota.c`, a
public key file in the source tree, and a CI build that compiles but refuses
updates was easier to reason about for one device on a desk.

**Anti-bricking.** `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` means a freshly
flashed image boots in `ESP_OTA_IMG_PENDING_VERIFY` and is confirmed only
once it has actually reached the agent. Push firmware that cannot join WiFi
or find the Mac and the bootloader reverts to the other slot on next boot --
which matters when the device is in another room.

> **One last cable.** Enabling this changed the partition table from a single
> `factory` slot to `ota_0`/`ota_1` + `otadata`, and a partition layout can
> only be written over USB. Connect the board once and run `./dev.sh flash`;
> every update after that is wireless. (If the bootloader refuses the new
> layout, `./dev.sh erase-flash` then flash again.)

### Night dimming

The agent decides brightness and the device just applies it, so the schedule
is tunable without a reflash:

| Phase | Level |
|---|---|
| sunrise to sunset | 100% |
| sunset to 23:00 | 45% |
| 23:00 to sunrise | 12% |

Sunrise and sunset come from Open-Meteo (already fetched for the weather
panel) for the actual location, so the schedule tracks the seasons. If
weather is unavailable it falls back to a fixed 07:00-21:00 day, rather than
leaving the panel at full output all night because an API failed. Tune
`DISPLAY_DAY` / `DISPLAY_EVENING` / `DISPLAY_NIGHT` at the top of the agent.

### Running it permanently (launchd)

Installed as a user LaunchAgent so it starts at login and restarts if it dies:

```sh
launchctl print    gui/$UID/com.welshofer.discus-agent    # status + pid
launchctl kickstart -k gui/$UID/com.welshofer.discus-agent # restart now
launchctl bootout  gui/$UID/com.welshofer.discus-agent    # stop + disable
tail -f ~/Library/Logs/discus-agent.log
```

Plist: `~/Library/LaunchAgents/com.welshofer.discus-agent.plist`.
`KeepAlive` is on (verified: SIGKILL -> back in ~2s with a new pid) and
`ThrottleInterval` is 10s so a startup crash-loop cannot spin.

Since the agent now binds to loopback unless told otherwise, the plist must
opt in to the LAN. Either add `--bind 0.0.0.0 --token-file
/Users/<you>/.config/discus/token` to `ProgramArguments`, or set both in
`EnvironmentVariables`:

```xml
<key>EnvironmentVariables</key>
<dict>
    <key>DISCUS_BIND</key>  <string>0.0.0.0</string>
    <key>DISCUS_TOKEN</key> <string>your-agent-token</string>
</dict>
```

(A token file with `chmod 600` keeps the secret out of the plist.)

### Claude usage: what is and is not knowable

**The limit is measured in prompts, not tokens.** Anthropic publishes no
fixed token bucket and throttles dynamically by system load, context length
and model tier. On Max 20x the documented ceiling is roughly **200-900
prompts per rolling 5-hour window** -- the low end for heavy agentic work,
the high end for light conversational use. The panel therefore counts
prompts, fills its ring against the optimistic ceiling, and prints the whole
range rather than implying a precision that does not exist. Past the
heavy-use floor the ring turns amber.

Counting prompts correctly is the fiddly part:

- **Most `type: "user"` rows are not prompts.** They are tool results, and
  they carry the *same* `promptId` as the prompt that spawned them. In one
  session: 150 user rows, but only 7 `promptSource: "typed"`. Filter to
  `promptSource` in `typed`/`queued` and dedup by `promptId`.
- **Dedup assistant usage by `message.id` too.** One id spans several rows
  (streaming plus tool blocks); summing naively inflates tokens ~3x.
- **Timestamps are UTC.** Use `calendar.timegm`, not `time.mktime`; and do
  not hand-correct with `time.timezone`, which ignores DST and silently
  shifts every window boundary by an hour for half the year.

Two honest caveats the panel cannot overcome:

1. **This counts only Claude Code.** claude.ai, the desktop app and Claude
   Code all draw from the same pool, so local transcripts are a *floor* on
   real usage, never the whole picture.
2. **Prompt cost is wildly non-uniform.** A representative sample from this
   project: 8 prompts produced 135 assistant turns and 442K billable tokens.
   That ~17x fan-out is precisely why the published ceiling is a range.

Tune `CLAUDE_PROMPT_LIMIT_5H` / `CLAUDE_PROMPT_FLOOR_5H` at the top of the
agent. Optional token budgets (`CLAUDE_BUDGET_*`) remain available but are
off by default.

## Quick start

```sh
# 1. Agent on the Mac (installs psutil + certifi on first run)
export DISCUS_TOKEN=$(python3 -c 'import secrets; print(secrets.token_urlsafe(24))')
uv run agent/discus_agent.py --bind 0.0.0.0
curl -s -H "X-Discus-Token: $DISCUS_TOKEN" localhost:8787/metrics | python3 -m json.tool

# 2. Point the firmware at your Mac; give it WiFi and the same token
cp main/wifi_creds.h.example main/wifi_creds.h   # gitignored; edit it
./dev.sh menuconfig        # Discus Dashboard -> Agent metrics URL

# 3. OTA signing key (once; commit main/ota_pubkey.pem, never keys/)
python3 agent/ota_sign.py keygen

# 4. Build and flash
./dev.sh build
./dev.sh flash
./dev.sh monitor
```

`dev.sh` wraps `idf.py` with the environment this setup needs — it sources
ESP-IDF and points Python at a working CA bundle, since several macOS Python
builds ship without one and the component manager cannot fetch dependencies
over TLS without it.

Requires ESP-IDF **v5.5** (the BSP declares `idf: ">=5.5"`), plus `cmake` and
`ninja`, which ESP-IDF expects from the system on macOS rather than
installing itself.

## Gotchas discovered the hard way

1. **Silicon revision.** This board is P4 **v1.3**, but ESP-IDF 5.5 defaults to
   the v3.x (ECO5) line, and Espressif's own Kconfig says the two are
   *mutually exclusive*. Without `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y` and
   `CONFIG_ESP32P4_REV_MIN_100=y`, flashing fails with
   `requires chip revision in range [v3.1 - v3.99]`.

2. **The LVGL lock timeout is signed and inverted.** `esp_lv_adapter_lock()`
   treats `-1` as "wait forever" and `0` as "fail immediately" — the opposite
   of the older `esp_lvgl_port` convention. `bsp_display_lock(0)` therefore
   fails, and using LVGL anyway panics with a null-screen dereference. Use
   `DISCUS_LVGL_FOREVER` and always check the return.

3. **PSRAM is off by default** and must be enabled explicitly. Three 720x720
   RGB565 framebuffers is ~3.1 MB and will never fit in internal SRAM.

4. **The default 1 MB app partition is too small** once LVGL and esp-hosted are
   linked in. See `partitions.csv`.

5. **`W ledc: GPIO 26 is not usable` is benign.** The BSP calls
   `bsp_display_brightness_init()` twice — once in `bsp_display_new()`, again
   in `bsp_display_start()` — so the second LEDC claim warns. Not a real
   conflict.

6. **C6 co-processor firmware reports a stale version — but it works.** Boot
   logs `esp-hosted fw versions: host=3.0.7 coprocessor=0.0.0` and
   `major version mismatch — OTA coprocessor from host`. Despite the ERROR
   level, association succeeds and the link is stable (verified: DHCP lease
   plus 5/5 ping from the host). Treat it as a warning, not a blocker. If
   Wi-Fi ever becomes flaky, this is the first suspect; there is an ESP32-C6
   UART header (SH1.0 4-pin) on the board as a fallback flashing path.

7. **LVGL's `lv_snprintf` has no `%f`.** `LV_SPRINTF_USE_FLOAT` is off by
   default to save flash, and the failure is silent and baffling: `"%.0f%%"`
   renders as the literal `f%`. Format integers and round at the call site.

8. **`__has_include` + ninja is a staleness trap.** `discus_creds.h` guards on
   `__has_include("wifi_creds.h")`. If the object files were already compiled
   when that header did not exist, the depfile has no reference to it, so
   creating the header later triggers no rebuild and the override silently
   does nothing. `touch main/discus_creds.h` after creating it. The same
   applies to `main/ota_pubkey.pem`: CMake only looks for it at configure
   time, so run `./dev.sh reconfigure` (or `fullclean`) after `keygen`.

   Tell-tale sign: the binary jumps ~136 KB once credentials are real,
   because an empty SSID literal lets the compiler dead-code-eliminate the
   entire Wi-Fi path.

9. **Do not build a wrapping carousel on `lv_tileview`.** Tileview is
   scrollable, and a scrollable object consumes the drag: LVGL then delivers
   neither `LV_EVENT_GESTURE` (only sent when *nothing* scrolled) nor a
   usable `LV_EVENT_RELEASED`. Both a gesture-based and a press/release-based
   wrap were tried against tileview and neither fires. The working design is
   a plain non-scrollable strip `PANEL_COUNT * 720` wide whose `x` we animate
   ourselves, with the gesture handler on the screen. Everything on the strip
   must have `LV_OBJ_FLAG_CLICKABLE` removed, or it becomes the gesture
   target instead of the screen and the swipe is swallowed again.

10. **The Montserrat glyph range is tiny.** Built-in LVGL Montserrat covers
    `0x20-0x7F` plus exactly two extras: `0xB0` (degree) and `0x2022`
    (bullet). A U+00B7 middle dot, en dash or arrow renders as an empty box
    on the panel -- and nothing warns you. Use `•` (U+2022) as the only
    non-ASCII separator.

11. **48px is the largest built-in font.** `lv_font_montserrat_48` is the
    ceiling. The clock needed more, so `main/fonts/lv_font_clock_128.c` is
    generated with `lv_font_conv` from the same `Montserrat-Medium.ttf` LVGL
    builds its own fonts from (it ships inside the lvgl component). Restrict
    the glyph range to what the label actually renders -- digits and a colon
    here -- or the file balloons:

    ```sh
    TTF=$(find ~/Library/Caches/Espressif -name Montserrat-Medium.ttf | head -1)
    npx lv_font_conv --font "$TTF" --size 128 --bpp 4 --format lvgl \
        --no-compress --range 0x30-0x3A --lv-include lvgl.h \
        -o main/fonts/lv_font_clock_128.c
    ```

    Eleven glyphs at 128px/4bpp is ~190KB. The full ASCII range would be
    roughly eight times that. Declare it with `LV_FONT_DECLARE()` and add
    the `.c` to `SRCS`.

12. **`lv_line` draws ONE continuous polyline, not separate segments.**
    Feeding it 16 points expecting 8 discrete sun rays renders a single
    zigzag star outline instead, connecting each ray's tip to the next ray's
    base -- and because the path never closes, it looks like one segment is
    "missing". Discrete strokes need one `lv_line` object each. The point
    arrays must also be `static`: `lv_line` stores the pointer, it does not
    copy.

13. **The panel is round but the framebuffer is square.** ~21% of every frame is
   hidden behind the bezel. Keep the layout radial.
