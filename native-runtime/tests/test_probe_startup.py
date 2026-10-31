import importlib.util
from pathlib import Path
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location("probe_startup", Path(__file__).parents[1] / "tools/probe_startup.py")
probe = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(probe)


class StartupProbeTest(unittest.TestCase):
    def test_rejects_jump_outside_or_into_instruction(self):
        for code in ("eb7fc3", "ebffc3"):
            with self.subTest(code=code), self.assertRaisesRegex(ValueError, "instruction boundary"):
                probe.validate_decode(bytes.fromhex(code), 0x401000, [])

    def test_rejects_indirect_jump_and_unadmitted_call(self):
        for code in ("ffe0c3", "ff1500104000c3", "e800000000c3"):
            with self.subTest(code=code), self.assertRaises(ValueError):
                probe.validate_decode(bytes.fromhex(code), 0x401000, [])

    def test_rejects_unsupported_and_incomplete_instructions(self):
        for code in ("", "0f", "90c3", "c20400"):
            with self.subTest(code=code), self.assertRaises(ValueError):
                probe.validate_decode(bytes.fromhex(code), 0x401000, [])

    def test_accepts_bounded_synthetic_iat_call(self):
        decoded = probe.validate_decode(bytes.fromhex("ff1500104000c3"), 0x401000, [0x401000])
        self.assertEqual([i.mnemonic for i in decoded], ["call", "ret"])

    def test_checks_source_identity_before_loading_kit(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "synthetic.exe"
            path.write_bytes(b"synthetic")
            with self.assertRaisesRegex(ValueError, "Executable hash differs"):
                probe.generate(path, Path("absent"), {"source_sha256": "0" * 64})


if __name__ == "__main__":
    unittest.main()
