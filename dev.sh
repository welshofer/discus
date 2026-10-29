#!/usr/bin/env bash
# Wrapper around idf.py that sets up the environment this machine needs.
#
# Two non-obvious things are handled here:
#   1. The python.org Python 3.12 ships without a usable CA bundle, so the
#      component manager cannot fetch dependencies over TLS. We point it at
#      the certifi bundle installed under ~/Library/Python/3.12.
#   2. ESP-IDF's export.sh must be sourced for idf.py to exist at all.
#
# After any command that leaves a new build/discus.bin, the image is signed
# with keys/ota_signing_key.pem (or $DISCUS_OTA_KEY) so the agent can serve
# it as an OTA update. Devices refuse unsigned images.
set -euo pipefail

export SSL_CERT_FILE="$(python3 -c 'import certifi; print(certifi.where())')"
export REQUESTS_CA_BUNDLE="$SSL_CERT_FILE"
source "$HOME/esp/esp-idf/export.sh" >/dev/null 2>&1

PORT="${DISCUS_PORT:-/dev/cu.usbmodem101}"
idf.py -p "$PORT" "$@"

ROOT="$(cd "$(dirname "$0")" && pwd)"
BIN="$ROOT/build/discus.bin"
KEY="${DISCUS_OTA_KEY:-$ROOT/keys/ota_signing_key.pem}"
if [ -f "$BIN" ] && [ "$BIN" -nt "$BIN.sig" ]; then
    if [ -f "$KEY" ]; then
        # ESP-IDF's Python (on PATH after export.sh) already has `cryptography`.
        python "$ROOT/agent/ota_sign.py" sign --key "$KEY" --bin "$BIN"
    else
        echo "warning: no OTA signing key at $KEY; build/discus.bin is unsigned" >&2
        echo "         and devices will not install it. See README 'Signed updates'." >&2
    fi
fi
