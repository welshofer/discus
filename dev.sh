#!/usr/bin/env bash
# Wrapper around idf.py that sets up the environment this machine needs.
#
# Two non-obvious things are handled here:
#   1. The python.org Python 3.12 ships without a usable CA bundle, so the
#      component manager cannot fetch dependencies over TLS. We point it at
#      the certifi bundle installed under ~/Library/Python/3.12.
#   2. ESP-IDF's export.sh must be sourced for idf.py to exist at all.
set -euo pipefail

export SSL_CERT_FILE="$(python3 -c 'import certifi; print(certifi.where())')"
export REQUESTS_CA_BUNDLE="$SSL_CERT_FILE"
source "$HOME/esp/esp-idf/export.sh" >/dev/null 2>&1

PORT="${DISCUS_PORT:-/dev/cu.usbmodem101}"
exec idf.py -p "$PORT" "$@"
