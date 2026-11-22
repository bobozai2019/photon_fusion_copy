"""Exercise the output policy using disposable repository-owned files."""

from contextlib import redirect_stdout
import io
import os
from pathlib import Path
import sys
import time
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import artifacts
import sdk_packages as package


class ArtifactTests(unittest.TestCase):
    def test_storage_policy(self):
        fixture = artifacts.create_job(artifacts.ROOT, "tmp/storage-test", "temp")
        root = fixture / "out"
        root.mkdir()
        try:
            expired = artifacts.create_job(root, "tmp/expired", "temp")
            active = artifacts.create_job(root, "tmp/active", "temp")
            unknown = root / "tmp" / "unknown"
            unknown.mkdir()
            with_unknown_file = artifacts.create_job(root, "tmp/mixed", "temp")
            for job in (unknown, with_unknown_file):
                (job / "user.txt").write_text("keep", encoding="ascii")
            old = time.time() - artifacts.DAY - 1
            for job in (active, expired):
                os.utime(job / artifacts.RECORD, (old, old))
            with artifacts.job_lock(active), redirect_stdout(io.StringIO()) as log:
                artifacts.clean(root)
                self.assertTrue(expired.exists())
                self.assertIn("candidate", log.getvalue())
                artifacts.clean(root, apply=True)
                self.assertIn("Active job", log.getvalue())
                self.assertIn("Unknown directory", log.getvalue())
                self.assertTrue(active.exists())
                self.assertTrue(unknown.exists())
                self.assertTrue((with_unknown_file / "user.txt").exists())
                self.assertFalse(expired.exists())

            for n in (1, 2):
                current = artifacts.create_job(root, "runs/current", "run")
                with artifacts.job_lock(current, "run"):
                    (current / "error.txt").write_text(str(n), encoding="ascii")
                last_fail = artifacts.rotate_run(root, passed=False)
                self.assertEqual((last_fail / "error.txt").read_text(encoding="ascii"), str(n))
                self.assertFalse(current.exists())
            self.assertFalse(artifacts.budget(root, ceiling=1, reserve=0)["allowed"])
            self.assertFalse(artifacts.budget(root, reserve=1 << 60)["allowed"])
            with self.assertRaises(ValueError):
                artifacts.item_at(root, "../outside")
            with self.assertRaises(ValueError):
                artifacts.remove_owned(root, unknown, "temp")
        finally:
            artifacts.seal_job(fixture, "temp")
            artifacts.remove_owned(artifacts.ROOT, fixture, "temp")

    def test_package_rotation(self):
        fixture = artifacts.create_job(artifacts.ROOT, "tmp/package-test", "temp")
        root = fixture / "out"
        work = root / "tmp" / "package-win-x64"
        work.mkdir(parents=True)
        staged = work / "staged"
        current = root / package.SLOT / "current"
        previous = root / package.SLOT / "previous"

        def stage(payload):
            staged.mkdir(exist_ok=True)
            archive = staged / package.PACKAGE
            archive.write_bytes(payload)
            digest = package.sha256(archive)
            (staged / package.MANIFEST).write_text(f"{digest}  {package.PACKAGE}\n", encoding="ascii")
            return digest

        try:
            first = stage(b"first")
            self.assertTrue(package.publish(root, work, staged))
            second = stage(b"second")
            self.assertTrue(package.publish(root, work, staged))
            self.assertEqual(package.slot_digest(root, previous), first)
            self.assertEqual(package.slot_digest(root, current), second)
            stage(b"second")
            self.assertFalse(package.publish(root, work, staged))
            stage(b"third")

            rename = Path.rename

            def fail_last_move(source, destination):
                if source == staged:
                    raise OSError("simulated publish error")
                return rename(source, destination)

            with patch.object(Path, "rename", fail_last_move), self.assertRaises(OSError):
                package.publish(root, work, staged)
            self.assertEqual(package.slot_digest(root, previous), first)
            self.assertEqual(package.slot_digest(root, current), second)
        finally:
            artifacts.seal_job(fixture, "temp")
            artifacts.remove_owned(artifacts.ROOT, fixture, "temp")


if __name__ == "__main__":
    unittest.main()
