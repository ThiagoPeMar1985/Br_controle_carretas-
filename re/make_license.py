#!/usr/bin/env python3
"""License tooling for br_controle_de_carretas.

License format:  BRCDT1.<yyyymmdd>.<base64url(sig)>
Signed payload:  "BRCDT1|<hwid-lower>|<yyyymmdd>"  (SHA-256, ECDSA P-256,
                  signature emitted as raw r||s, 64 bytes — what
                  BCryptVerifySignature expects)

Commands:
    keygen            generate the key pair -> license_keys/BR-CDT-private.pem
                      and print the 64 public bytes to paste into
                      src/license/license.cpp (k_public_key)
    pubkey            print the C array for k_public_key again
    hwid              print this PC's Hardware ID (same value the plugin
                      shows on the Licença tab)
    mint <hwid> [d]   mint a license bound to <hwid> valid for [d] days
                      (default 365). <hwid> with or without dashes.

The private key never leaves license_keys/ (git-ignored). Keep it safe —
whoever has it can mint licenses for any machine.
"""
import base64
import hashlib
import sys
import time
import uuid as uuidlib
from datetime import datetime, timezone
from pathlib import Path

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec, utils

KEYS_DIR = Path(__file__).resolve().parent.parent / "license_keys"
PRIV_PEM = KEYS_DIR / "BR-CDT-private.pem"
PRODUCT = "BRCDT1"


def b64url(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode()


def _load_or_die():
    if not PRIV_PEM.exists():
        sys.exit("no private key — run `keygen` first")
    return serialization.load_pem_private_key(PRIV_PEM.read_bytes(), None)


def cmd_keygen():
    KEYS_DIR.mkdir(exist_ok=True)
    if PRIV_PEM.exists():
        print("key already exists — keeping it (delete license_keys/ to reset)")
    else:
        priv = ec.generate_private_key(ec.SECP256R1())
        PRIV_PEM.write_bytes(priv.private_bytes(
            serialization.Encoding.PEM,
            serialization.PrivateFormat.PKCS8,
            serialization.NoEncryption()))
        print(f"private key -> {PRIV_PEM}")
    cmd_pubkey()


def cmd_pubkey():
    priv = _load_or_die()
    pub = priv.public_key().public_numbers()
    xy = pub.x.to_bytes(32, "big") + pub.y.to_bytes(32, "big")
    print("// paste into k_public_key (after the 8-byte header) "
          "in src/license/license.cpp:")
    for i in range(0, 64, 8):
        print("    " + " ".join(f"0x{b:02X}," for b in xy[i:i + 8]))


def _smbios_uuid_bytes() -> bytes:
    """WMI UUID -> raw SMBIOS bytes (first three fields little-endian)."""
    import subprocess
    out = subprocess.check_output(
        ["powershell", "-NoProfile", "-Command",
         "(Get-CimInstance Win32_ComputerSystemProduct).UUID"],
        text=True).strip()
    u = uuidlib.UUID(out)
    # uuid.bytes is RFC4122 (big-endian) order; SMBIOS stores time_low,
    # time_mid and time_hi little-endian
    return (u.bytes[0:4][::-1] + u.bytes[4:6][::-1] +
            u.bytes[6:8][::-1] + u.bytes[8:16])


def _hwid_from_uuid(raw: bytes) -> str:
    h = hashlib.sha256(b"brcdt|" + raw).digest()[:8]
    return "-".join(h[i:i + 2].hex().upper() for i in range(0, 8, 2))


def cmd_hwid():
    print(_hwid_from_uuid(_smbios_uuid_bytes()))


def cmd_mint(hwid: str, days: int = 365):
    hwid = hwid.strip().lower()
    if len(hwid.replace("-", "")) != 16:
        sys.exit("bad hwid — expected XX-XX-XX-XX-XX-XX-XX-XX")
    # expiry = now + days*24h, unix timestamp — the plugin compares it
    # straight against the UTC clock, so a short license lives its full hours
    exp = int(time.time()) + days * 86400
    expiry = datetime.fromtimestamp(exp, timezone.utc)
    payload = f"{PRODUCT}|{hwid}|{exp}".encode()
    sig_der = _load_or_die().sign(payload, ec.ECDSA(hashes.SHA256()))
    r, s = utils.decode_dss_signature(sig_der)
    sig = r.to_bytes(32, "big") + s.to_bytes(32, "big")
    print(f"{PRODUCT}.{exp}.{b64url(sig)}")
    print(f"# bound to {hwid}, expires {expiry.isoformat()} UTC", file=sys.stderr)


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd == "keygen":
        cmd_keygen()
    elif cmd == "pubkey":
        cmd_pubkey()
    elif cmd == "hwid":
        cmd_hwid()
    elif cmd == "mint":
        if len(sys.argv) < 3:
            sys.exit("usage: make_license.py mint <hwid> [days]")
        cmd_mint(sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 365)
    else:
        sys.exit("usage: make_license.py keygen|pubkey|hwid|mint <hwid> [days]")
