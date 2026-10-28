#!/usr/bin/env -S uv run --quiet
# /// script
# requires-python = ">=3.11"
# dependencies = ["cryptography>=41"]
# ///
"""
Sign discus firmware images for OTA.

The device only marks a downloaded image bootable after checking an
ECDSA-P256 / SHA-256 signature over the exact bytes it received, against a
public key compiled into the running firmware. This script holds the other
half: it makes the key pair and signs build/discus.bin.

    agent/ota_sign.py keygen     # once: keys/ota_signing_key.pem + main/ota_pubkey.pem
    agent/ota_sign.py sign       # after every build (dev.sh does this for you)
    agent/ota_sign.py verify     # optional sanity check

The private key lives in keys/ (gitignored) or wherever DISCUS_OTA_KEY
points. The public key, main/ota_pubkey.pem, is not secret and is committed
so CI builds embed it.

The signature is DER-encoded, which is what mbedTLS's mbedtls_pk_verify()
expects on the device. It is written next to the image as discus.bin.sig;
the agent hex-encodes it into /firmware.json.

`cryptography` is already in ESP-IDF's Python environment (esptool needs
it), so dev.sh runs this with that interpreter; `uv run` installs it too.
"""

import argparse
import os
import sys

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_KEY = os.environ.get(
    "DISCUS_OTA_KEY", os.path.join(ROOT, "keys", "ota_signing_key.pem"))
DEFAULT_PUB = os.path.join(ROOT, "main", "ota_pubkey.pem")
DEFAULT_BIN = os.path.join(ROOT, "build", "discus.bin")


def sig_path(bin_path):
    return bin_path + ".sig"


def keygen(key_path, pub_path):
    if os.path.exists(key_path):
        raise SystemExit(f"refusing to overwrite existing key {key_path}")
    key = ec.generate_private_key(ec.SECP256R1())
    os.makedirs(os.path.dirname(key_path) or ".", exist_ok=True)
    pem = key.private_bytes(serialization.Encoding.PEM,
                            serialization.PrivateFormat.PKCS8,
                            serialization.NoEncryption())
    # 0600 from the moment it exists, not chmod'd afterwards.
    fd = os.open(key_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "wb") as f:
        f.write(pem)
    with open(pub_path, "wb") as f:
        f.write(key.public_key().public_bytes(
            serialization.Encoding.PEM,
            serialization.PublicFormat.SubjectPublicKeyInfo))


def load_private(key_path):
    with open(key_path, "rb") as f:
        key = serialization.load_pem_private_key(f.read(), password=None)
    if not isinstance(key, ec.EllipticCurvePrivateKey) or \
            not isinstance(key.curve, ec.SECP256R1):
        raise SystemExit(f"{key_path} is not an ECDSA P-256 private key")
    return key


def load_public(pub_path):
    with open(pub_path, "rb") as f:
        pub = serialization.load_pem_public_key(f.read())
    if not isinstance(pub, ec.EllipticCurvePublicKey) or \
            not isinstance(pub.curve, ec.SECP256R1):
        raise SystemExit(f"{pub_path} is not an ECDSA P-256 public key")
    return pub


def verify(bin_path, pub_path, sig_file=None):
    """True if sig_file (default bin_path.sig) is a valid signature."""
    with open(bin_path, "rb") as f:
        data = f.read()
    with open(sig_file or sig_path(bin_path), "rb") as f:
        sig = f.read()
    try:
        load_public(pub_path).verify(sig, data, ec.ECDSA(hashes.SHA256()))
        return True
    except InvalidSignature:
        return False


def sign(bin_path, key_path, pub_path=None):
    """Write bin_path.sig. If pub_path exists, also prove the signature
    verifies against it: a private key that does not match the public key
    compiled into the firmware would make every future update fail on the
    device, and that is far better caught here."""
    key = load_private(key_path)
    with open(bin_path, "rb") as f:
        data = f.read()
    sig = key.sign(data, ec.ECDSA(hashes.SHA256()))

    if pub_path and os.path.exists(pub_path):
        try:
            load_public(pub_path).verify(sig, data, ec.ECDSA(hashes.SHA256()))
        except InvalidSignature:
            raise SystemExit(f"{key_path} does not match {pub_path}; "
                             "the device would reject this image") from None

    out = sig_path(bin_path)
    tmp = out + ".tmp"
    with open(tmp, "wb") as f:
        f.write(sig)
    os.replace(tmp, out)            # never serve a half-written signature
    return out


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[1])
    sub = p.add_subparsers(dest="cmd", required=True)

    k = sub.add_parser("keygen", help="create a new signing key pair")
    k.add_argument("--key", default=DEFAULT_KEY)
    k.add_argument("--pub", default=DEFAULT_PUB)

    s = sub.add_parser("sign", help="sign a firmware image")
    s.add_argument("--key", default=DEFAULT_KEY)
    s.add_argument("--pub", default=DEFAULT_PUB)
    s.add_argument("--bin", default=DEFAULT_BIN)

    v = sub.add_parser("verify", help="check an image's signature")
    v.add_argument("--pub", default=DEFAULT_PUB)
    v.add_argument("--bin", default=DEFAULT_BIN)

    a = p.parse_args(argv)
    if a.cmd == "keygen":
        keygen(a.key, a.pub)
        print(f"private key: {a.key}  (keep it out of git; back it up)")
        print(f"public key:  {a.pub}  (commit this)")
        print("Then: ./dev.sh fullclean build, and flash once over USB.")
    elif a.cmd == "sign":
        print(f"signed {a.bin} -> {sign(a.bin, a.key, a.pub)}")
    else:
        ok = verify(a.bin, a.pub)
        print("signature OK" if ok else "signature INVALID")
        return 0 if ok else 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
