import importlib.util
from pathlib import Path
import tempfile
import unittest


SPEC = importlib.util.spec_from_file_location("probe", Path(__file__).parents[1] / "tools/probe_routine.py")
probe = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(probe)


class ProbeTest(unittest.TestCase):
    def test_emits_supported_synthetic_pattern(self):
        # Synthetic offsets and arithmetic; this is not a copied game routine.
        emitted = probe.emit_probe(bytes.fromhex("8b41108b0083c007c3"), 0x401000)
        self.assertIn("ecx + 16u", emitted)
        self.assertIn("rhs = 7u", emitted)
        self.assertIn("(u64)(address - base) + 4 > size", emitted)
        self.assertNotIn("windows.h", emitted)

    def test_refuses_unimplemented_instruction_and_operand_forms(self):
        for code in ("", "90c3", "8b59108b0083c007c3", "648b41108b0083c007c3",
                     "8b41108b0083c007c20400", "8b41108b0083c007c3cc"):
            with self.subTest(code=code), self.assertRaises(ValueError):
                probe.emit_probe(bytes.fromhex(code), 0x401000)

    def test_source_identity_is_checked_before_parsing_or_emitting(self):
        with tempfile.TemporaryDirectory() as temp:
            executable = Path(temp) / "game.exe"
            executable.write_bytes(b"not a PE and not the selected source")
            profile = {"binaries": [{"file": "chusanApp.exe", "sha256": "0" * 64}]}
            with self.assertRaisesRegex(ValueError, "Executable hash differs"):
                probe.source_routine(executable, profile)

    def test_profile_must_name_game(self):
        with self.assertRaisesRegex(ValueError, "does not name"):
            probe.source_routine(Path("absent.exe"), {"binaries": []})


if __name__ == "__main__":
    unittest.main()
