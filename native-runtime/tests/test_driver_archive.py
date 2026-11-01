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


class DriverDrawTest(unittest.TestCase):
    def result(self):
        return dict(custom_requested=False, logical_device_created=True, draw_verified=True,
                    draw_width=4, draw_height=4, draw_matching_pixels=16,
                    draw_readback_rgba=[255, 0, 0, 255])

    def test_accepts_complete_pixel_evidence(self):
        driver.validate_draw(self.result(), False)

    def test_rejects_query_only_wrong_driver_and_incomplete_draw(self):
        for change in (dict(draw_verified=False), dict(draw_matching_pixels=15),
                       dict(draw_readback_rgba=[0, 0, 255, 255]), dict(custom_requested=True),
                       dict(draw_error="timeout"), dict(logical_device_created=False)):
            with self.subTest(change=change), self.assertRaises(ValueError):
                driver.validate_draw(self.result() | change, False)

    def shader_result(self):
        return self.result() | dict(d3d9_ps_probe_available=True, d3d9_ps_translation_compiled=True,
                d3d9_ps_shader_model="ps_1_1", d3d9_ps_draw_verified=True,
                d3d9_ps_draw_width=4, d3d9_ps_draw_height=4,
                d3d9_ps_expected_rgba=[0, 255, 0, 255], d3d9_ps_draw_matching_pixels=16,
                d3d9_ps_readback_rgba=[0, 255, 0, 255])

    def test_accepts_translated_shader_pixel_evidence(self):
        driver.validate_draw(self.shader_result(), False, require_d3d9=True)

    def test_rejects_missing_optional_shader_when_required(self):
        with self.assertRaisesRegex(ValueError, "D3D9 pixel shader"):
            driver.validate_draw(self.result(), False, require_d3d9=True)

    def test_rejects_compile_only_and_incomplete_translated_shader_draw(self):
        for change in (dict(d3d9_ps_translation_compiled=False), dict(d3d9_ps_draw_verified=False),
                       dict(d3d9_ps_draw_matching_pixels=15), dict(d3d9_ps_draw_width=8),
                       dict(d3d9_ps_readback_rgba=[255, 0, 0, 255]),
                       dict(d3d9_ps_shader_model="ps_2_0"), dict(d3d9_ps_draw_error="timeout")):
            with self.subTest(change=change), self.assertRaisesRegex(ValueError, "D3D9 pixel shader"):
                driver.validate_draw(self.shader_result() | change, False)


if __name__ == "__main__":
    unittest.main()
