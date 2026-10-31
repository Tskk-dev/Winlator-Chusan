import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest


SPEC = importlib.util.spec_from_file_location("inventory", Path(__file__).parents[1] / "tools/inventory.py")
inventory = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(inventory)


def fixture(bits=32):
    """Build a PE image with a named import and an ordinal import, not a mocked parser."""
    data = bytearray(0x600)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, 0x80)
    data[0x80:0x84] = b"PE\0\0"
    optional_size = 0xE0 if bits == 32 else 0xF0
    struct.pack_into("<HHIIIHH", data, 0x84, 0x14C if bits == 32 else 0x8664,
                     1, 0, 0, 0, optional_size, 0x0102 if bits == 32 else 0x0022)
    opt = 0x98
    struct.pack_into("<H", data, opt, 0x10B if bits == 32 else 0x20B)
    struct.pack_into("<I", data, opt + 16, 0x1000)
    struct.pack_into("<I" if bits == 32 else "<Q", data,
                     opt + (28 if bits == 32 else 24), 0x400000 if bits == 32 else 0x140000000)
    struct.pack_into("<II", data, opt + 32, 0x1000, 0x200)
    struct.pack_into("<II", data, opt + 56, 0x2000, 0x200)
    struct.pack_into("<I", data, opt + (92 if bits == 32 else 108), 16)
    directory = opt + (96 if bits == 32 else 112)
    struct.pack_into("<II", data, directory + 8, 0x1100, 40)
    section = opt + optional_size
    data[section:section + 8] = b".text\0\0\0"
    struct.pack_into("<IIII", data, section + 8, 0x400, 0x1000, 0x400, 0x200)
    struct.pack_into("<I", data, section + 36, 0x60000020)
    data[0x200] = 0xC3
    struct.pack_into("<IIIII", data, 0x300, 0x1140, 0, 0, 0x1170, 0x1158)
    thunk = "<III" if bits == 32 else "<QQQ"
    struct.pack_into(thunk, data, 0x340, 0x1190, (1 << (bits - 1)) | 7, 0)
    struct.pack_into(thunk, data, 0x358, 0x1190, (1 << (bits - 1)) | 7, 0)
    data[0x370:0x37D] = b"KERNEL32.dll\0"
    data[0x390:0x3A0] = b"\0\0GetProcAddress\0"
    return bytes(data)


class InventoryTest(unittest.TestCase):
    def test_real_pe_headers_and_imports(self):
        for bits, architecture in ((32, "x86"), (64, "x86_64")):
            with self.subTest(bits=bits), tempfile.TemporaryDirectory() as temp:
                path = Path(temp) / "game.exe"
                path.write_bytes(fixture(bits))
                result = inventory.inspect_binary(path)
                self.assertEqual(result["architecture"], architecture)
                self.assertEqual(result["pe_bits"], bits)
                self.assertEqual(result["entry_rva"], "0x1000")
                self.assertEqual(result["import_count"], 2)
                self.assertEqual(result["imports"][0]["symbols"], ["GetProcAddress", "#7"])
                self.assertTrue(result["dynamic_loading_declared"])
                self.assertTrue(result["sections"][0]["executable"])
                self.assertNotIn(temp, str(result))

    def test_profile_refuses_changed_and_missing_inputs(self):
        observed = {"file": "game.exe", "sha256": "a" * 64, "architecture": "x86"}
        good = {"binaries": [observed.copy()]}
        self.assertEqual(inventory.verify_profile([observed], good), [])
        bad_hash = {"binaries": [dict(observed, sha256="b" * 64)]}
        self.assertEqual(len(inventory.verify_profile([observed], bad_hash)), 1)
        bad_arch = {"binaries": [dict(observed, architecture="x86_64")]}
        self.assertEqual(len(inventory.verify_profile([observed], bad_arch)), 1)
        self.assertEqual(len(inventory.verify_profile([], good)), 1)

    def test_profile_cannot_name_a_path(self):
        with self.assertRaises(ValueError):
            inventory.verify_profile([], {"binaries": [{"file": "../game.exe"}]})

    def test_rejects_malformed_images(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "game.exe"
            path.write_bytes(b"not a PE")
            with self.assertRaises(ValueError):
                inventory.inspect_binary(path)

    def test_scan_is_top_level_and_source_is_unchanged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            original = fixture()
            (root / "game.exe").write_bytes(original)
            (root / "notes.txt").write_text("not a binary")
            (root / "data").mkdir()
            (root / "data/nested.exe").write_bytes(b"not a PE")
            report = inventory.inventory(root)
            self.assertEqual(report["binary_count"], 1)
            self.assertEqual((root / "game.exe").read_bytes(), original)

    def test_empty_source_is_an_error(self):
        with tempfile.TemporaryDirectory() as temp:
            with self.assertRaises(ValueError):
                inventory.inventory(Path(temp))


if __name__ == "__main__":
    unittest.main()
