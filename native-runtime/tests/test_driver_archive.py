import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest
import zipfile

TOOLS = Path(__file__).parents[1] / "tools"
sys.path.insert(0, str(TOOLS))
SPEC = importlib.util.spec_from_file_location("driver_tools", TOOLS / "test_vulkan_driver.py")
driver = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(driver)


class DriverArchiveTest(unittest.TestCase):
    def package(self, root, extra=None, machine=183):
        archive = root / "driver.zip"
        header = bytearray(20)
        header[:6] = b"\x7fELF\x02\x01"
        header[18:20] = machine.to_bytes(2, "little")
        with zipfile.ZipFile(archive, "w") as package:
            package.writestr("meta.json", json.dumps({"libraryName": "driver.so"}))
            package.writestr("driver.so", header)
            if extra:
                package.writestr(extra, b"untrusted")
        return archive

    def test_extracts_a_flat_arm64_driver_package(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            out = root / "out"
            out.mkdir()
            driver.extract_driver(self.package(root), out)
            self.assertTrue((out / "driver.so").is_file())

    def test_rejects_traversal_before_writing_outside_destination(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            out = root / "out"
            out.mkdir()
            with self.assertRaises(ValueError):
                driver.extract_driver(self.package(root, "../escaped"), out)
            self.assertFalse((root / "escaped").exists())

    def test_rejects_wrong_architecture(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            out = root / "out"
            out.mkdir()
            with self.assertRaisesRegex(ValueError, "not an ARM64"):
                driver.extract_driver(self.package(root, machine=62), out)

    def test_rejects_symbolic_links(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive = self.package(root)
            with zipfile.ZipFile(archive, "a") as package:
                entry = zipfile.ZipInfo("link")
                entry.create_system = 3
                entry.external_attr = 0o120777 << 16
                package.writestr(entry, "../../outside")
            out = root / "out"
            out.mkdir()
            with self.assertRaisesRegex(ValueError, "symbolic link"):
                driver.extract_driver(archive, out)


if __name__ == "__main__":
    unittest.main()
