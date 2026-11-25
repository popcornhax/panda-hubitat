"""Failure-injection tests; these do not assert hardware verification."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('backup_flash', Path(__file__).resolve().parents[1] / 'scripts/backup_flash.py')
backup_flash = importlib.util.module_from_spec(spec)
spec.loader.exec_module(backup_flash)


class BackupTests(unittest.TestCase):
    def run_fake(self, failure=None):
        calls = []

        def run(port, command, *args):
            calls.append(command)
            if command == 'flash-id':
                return 'MAC: aa:bb:cc:dd:ee:ff\nDetected flash size: 16MB\n'
            if command == 'get-security-info':
                return 'Secure Boot: Disabled\nFlash Encryption: Disabled\n'
            if command == 'read-flash':
                offset, size, path = args
                path.write_bytes(bytes([offset % 256]) * (size - 1 if failure == 'short' else size))
                return ''
            if command == 'verify-flash':
                if failure == 'verify':
                    raise RuntimeError('digest mismatch')
                return 'Verification successful (digest matched).'
            raise AssertionError(command)

        return run, calls

    def test_complete_image_and_manifest_only_after_chip_verification(self):
        run, calls = self.run_fake()
        with tempfile.TemporaryDirectory() as root, patch.object(backup_flash, 'SIZE', 256), patch.object(backup_flash, 'CHUNK', 128):
            final = backup_flash.backup('fake', root, run)
            self.assertEqual(final.stat().st_size, 256)
            self.assertTrue((final.parent / 'VERIFIED.json').exists())
            self.assertEqual(calls.count('read-flash'), 2)
            self.assertIn('verify-flash', calls)
            self.assertFalse(any('write' in c or 'erase' in c for c in calls))

    def test_partial_reads_never_produce_verified_manifest(self):
        run, calls = self.run_fake('short')
        with tempfile.TemporaryDirectory() as root, patch.object(backup_flash, 'SIZE', 256), patch.object(backup_flash, 'CHUNK', 128):
            with self.assertRaisesRegex(RuntimeError, 'no verified backup'):
                backup_flash.backup('fake', root, run)
            self.assertEqual(calls.count('read-flash'), 20)
            self.assertFalse(list(Path(root).rglob('VERIFIED.json')))

    def test_digest_failure_never_publishes_backup(self):
        run, _ = self.run_fake('verify')
        with tempfile.TemporaryDirectory() as root, patch.object(backup_flash, 'SIZE', 256), patch.object(backup_flash, 'CHUNK', 128):
            with self.assertRaisesRegex(RuntimeError, 'digest mismatch'):
                backup_flash.backup('fake', root, run)
            self.assertFalse(list(Path(root).rglob('VERIFIED.json')))
            self.assertFalse(list(Path(root).rglob('original-16MiB.bin')))

    def test_wrong_flash_size_is_rejected_before_reading(self):
        with tempfile.TemporaryDirectory() as root:
            with self.assertRaisesRegex(RuntimeError, '16MB'):
                backup_flash.backup('fake', root, lambda *_: 'Detected flash size: 8MB')
            self.assertEqual(list(Path(root).iterdir()), [])

    def test_protected_or_unknown_security_state_is_rejected(self):
        for security in ('Secure Boot: Enabled\nFlash Encryption: Disabled', 'unknown response'):
            with self.subTest(security=security), tempfile.TemporaryDirectory() as root:
                def run(port, command):
                    return ('MAC: aa:bb:cc:dd:ee:ff\nDetected flash size: 16MB'
                            if command == 'flash-id' else security)
                with self.assertRaisesRegex(RuntimeError, 'plain-image recovery'):
                    backup_flash.backup('fake', root, run)
                self.assertEqual(list(Path(root).iterdir()), [])

    def test_changed_device_and_missing_verification_are_rejected(self):
        for failure in ('changed_mac', 'missing_confirmation'):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as root, patch.object(backup_flash, 'SIZE', 256), patch.object(backup_flash, 'CHUNK', 128):
                original, _ = self.run_fake()
                identity_reads = 0

                def run(port, command, *args):
                    nonlocal identity_reads
                    if command == 'verify-flash' and failure == 'missing_confirmation':
                        return 'Unexpected output with exit code zero'
                    if command == 'flash-id':
                        identity_reads += 1
                        if identity_reads > 1 and failure == 'changed_mac':
                            return 'MAC: 11:22:33:44:55:66\nDetected flash size: 16MB'
                    return original(port, command, *args)

                with self.assertRaises(RuntimeError):
                    backup_flash.backup('fake', root, run)
                self.assertFalse(list(Path(root).rglob('VERIFIED.json')))
                self.assertFalse(list(Path(root).rglob('original-16MiB.bin')))


if __name__ == '__main__':
    unittest.main()
