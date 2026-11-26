"""Exercise the same package checks used by the player and R1 packer."""
import hashlib
from pathlib import Path
import subprocess
import tempfile
import unittest

VERIFY = Path(__file__).resolve().parents[1] / "verify-upt.sh"


class UpdatePackageTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.ota = self.root / "ota_v0"
        self.ota.mkdir()
        (self.root / "ota_config.in").write_text("current_version=0\n")
        (self.ota / "ota_v0.ok").touch()
        (self.ota / "ota_compatibility.in").write_text("compatibility=hiby_r1\n")
        self.manifest = self.ota / "ota_update.in"
        sections = ["ota_version=0\n"]
        for kind, name in (("kernel", "xImage"), ("rootfs", "rootfs.squashfs")):
            chunks = [f"{kind}-0".encode(), f"{kind}-1".encode()]
            payload = b"".join(chunks)
            whole = hashlib.md5(payload).hexdigest()
            previous = whole
            sums = []
            for i, chunk in enumerate(chunks):
                (self.ota / f"{name}.{i:04d}.{previous}").write_bytes(chunk)
                previous = hashlib.md5(chunk).hexdigest()
                sums.append(previous)
            (self.ota / f"ota_md5_{name}.{whole}").write_text("\n".join(sums) + "\n")
            sections.append(f"img_type={kind}\nimg_name={name}\nimg_size={len(payload)}\nimg_md5={whole}\n")
        self.manifest.write_text("\n".join(sections))

    def accepts(self, model="r1"):
        return subprocess.run(["sh", VERIFY, model, self.root, "--tree"]).returncode == 0

    def test_valid_for_correct_model(self):
        self.assertTrue(self.accepts())
        self.assertFalse(self.accepts("r3proii"))

    def test_damaged_chunk(self):
        next(self.ota.glob("rootfs.squashfs.0001.*")).write_bytes(b"damaged")
        self.assertFalse(self.accepts())

    def test_missing_kernel_chunk(self):
        next(self.ota.glob("xImage.0000.*")).unlink()
        self.assertFalse(self.accepts())

    def test_swapped_chunks(self):
        a, b = sorted(self.ota.glob("rootfs.squashfs.0*"))
        first, second = a.read_bytes(), b.read_bytes()
        a.write_bytes(second)
        b.write_bytes(first)
        self.assertFalse(self.accepts())

    def test_oversized_rootfs(self):
        self.manifest.write_text(self.manifest.read_text().replace("img_size=16", "img_size=47185921"))
        self.assertFalse(self.accepts())

    def test_incomplete_checksum_list(self):
        next(self.ota.glob("ota_md5_rootfs.*")).write_bytes(b"")
        self.assertFalse(self.accepts())

    def test_empty_update(self):
        empty = self.root / "r1.upt"
        empty.touch()
        self.assertNotEqual(subprocess.run(["sh", VERIFY, "r1", empty]).returncode, 0)


if __name__ == "__main__":
    unittest.main()
