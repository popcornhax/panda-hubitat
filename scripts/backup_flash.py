#!/usr/bin/env python3
"""Read-only, chunked Panda Touch backup. Never erases or writes flash."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
from datetime import datetime, timezone

SIZE = 16 * 1024 * 1024
CHUNK = 128 * 1024
DEFAULT_ROOT = Path.home() / 'Library/Application Support/panda-hubitat/backups'


def esptool(port, *args):
    result = subprocess.run(
        [sys.executable, '-m', 'esptool', '--chip', 'esp32s3', '--port', port,
         '--baud', '115200', *map(str, args)],
        capture_output=True, text=True, timeout=180,
    )
    if result.returncode:
        raise RuntimeError(f'esptool {args[0]} failed: {result.stdout[-1500:]} {result.stderr[-1500:]}')
    return result.stdout


def backup(port, root=DEFAULT_ROOT, run=esptool):
    # Explicit chip selection makes esptool reject non-S3 chips before any reads.
    identity = run(port, 'flash-id')
    if not re.search(r'Detected flash size:\s*16MB', identity):
        raise RuntimeError('Expected a detected 16MB flash; refusing to proceed.')
    mac_match = re.search(r'MAC:\s*([0-9a-fA-F:]{17})', identity)
    if not mac_match:
        raise RuntimeError('Cannot establish chip MAC identity; refusing backup.')
    mac = mac_match.group(1).lower()
    security = run(port, 'get-security-info')
    for feature in ('Secure Boot', 'Flash Encryption'):
        if not re.search(rf'^{feature}:\s*Disabled\s*$', security, re.MULTILINE):
            raise RuntimeError(f'{feature} is enabled or unknown; plain-image recovery is not established.')
    root = Path(root).expanduser().resolve()
    repo = Path(__file__).resolve().parents[1]
    if root == repo or repo in root.parents:
        raise RuntimeError('Backups must be outside the source tree.')
    root.mkdir(parents=True, exist_ok=True, mode=0o700)
    os.chmod(root, 0o700)
    dest = root / (datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ-') + mac.replace(':', ''))
    dest.mkdir(mode=0o700)
    (dest / 'identity.txt').write_text(identity + '\n' + security)
    image = dest / 'original-16MiB.bin.partial'
    with image.open('xb') as output:
        for offset in range(0, SIZE, CHUNK):
            piece = dest / 'chunk.bin'
            for attempt in range(1, 21):
                piece.unlink(missing_ok=True)
                try:
                    run(port, 'read-flash', offset, CHUNK, piece)
                    if not piece.exists() or piece.stat().st_size != CHUNK:
                        raise RuntimeError('Incomplete chunk')
                    break
                except (RuntimeError, subprocess.TimeoutExpired):
                    if attempt == 20:
                        raise RuntimeError(f'Backup stopped at {offset:#x}; no verified backup exists.')
            output.write(piece.read_bytes())
            piece.unlink()
            print(f'Backed up {(offset + CHUNK) // CHUNK}/{SIZE // CHUNK} chunks', flush=True)
    if image.stat().st_size != SIZE:
        raise RuntimeError('Wrong complete backup size')
    # A fresh chip-side digest of the entire image verifies concatenation and reads.
    verification = run(port, 'verify-flash', 0, image)
    if 'Verification successful' not in verification:
        raise RuntimeError('Missing explicit whole-flash verification success')
    current_identity = run(port, 'flash-id')
    current_mac = re.search(r'MAC:\s*([0-9a-fA-F:]{17})', current_identity)
    if not current_mac or current_mac.group(1).lower() != mac:
        raise RuntimeError('Device identity changed during backup')
    final = image.with_suffix('')
    image.rename(final)
    digest = hashlib.sha256(final.read_bytes()).hexdigest()
    (dest / 'SHA256SUMS').write_text(f'{digest}  {final.name}\n')
    (dest / 'VERIFIED.json').write_text(json.dumps({
        'mac': mac, 'size': SIZE, 'sha256': digest, 'image': final.name,
        'verified_at_utc': datetime.now(timezone.utc).isoformat(),
        'verification': 'esptool full-image chip digest matched',
    }, indent=2) + '\n')
    for path in dest.iterdir():
        path.chmod(0o600)
    print(f'Verified private backup: {final}\nSHA256: {digest}')
    return final


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True)
    parser.add_argument('--backup-root', type=Path, default=DEFAULT_ROOT)
    args = parser.parse_args()
    os.umask(0o077)
    try:
        backup(args.port, args.backup_root)
    except (RuntimeError, subprocess.TimeoutExpired) as exc:
        parser.exit(1, f'{exc}\nSTOP: do not erase or flash.\n')
