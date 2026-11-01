import importlib.util
from pathlib import Path
import struct
import unittest

import capstone

SPEC = importlib.util.spec_from_file_location("prepare_runtime", Path(__file__).parents[1] / "tools/prepare_runtime.py")
prepare = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(prepare)


class SyntheticImage:
    base = 0x401000

    def __init__(self, code):
        self.data = bytes.fromhex(code)
        self.end = self.base + len(self.data)
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self.md.detail = True

    def is_exec(self, address):
        return self.base <= address < self.base + len(self.data)


class RuntimePreparationTest(unittest.TestCase):
    def test_follows_call_callee_and_continuation(self):
        image = SyntheticImage("e801000000c3c3")
        blocks, frontier = prepare.decode_graph(image, [image.base], 10)
        self.assertEqual(set(blocks), {image.base, image.base + 5, image.base + 6})
        self.assertEqual(frontier, [])

    def test_loop_is_finite_and_branch_fallthrough_is_admitted(self):
        image = SyntheticImage("4975fdc3")
        blocks, _ = prepare.decode_graph(image, [image.base], 10)
        self.assertEqual(set(blocks), {image.base, image.base + 3})

    def test_rejects_branch_into_instruction_middle(self):
        image = SyntheticImage("b801000000ebfa")
        with self.assertRaisesRegex(ValueError, "Overlapping"):
            prepare.decode_graph(image, [image.base], 10)

    def test_limit_preserves_frontier_and_indirect_target_is_not_guessed(self):
        image = SyntheticImage("e801000000c3ffe0")
        blocks, frontier = prepare.decode_graph(image, [image.base], 1)
        self.assertEqual(len(blocks), 1)
        self.assertEqual(frontier, [image.base + 5, image.base + 6])
        blocks, frontier = prepare.decode_graph(image, [image.base + 6], 5)
        self.assertEqual(len(blocks), 1)
        self.assertEqual(frontier, [])

    def test_header_patch_refuses_unknown_header_layout(self):
        with self.assertRaisesRegex(ValueError, "memory helper changed"):
            prepare.checked_header("/* ---------------------------------------------------------------- memory */")

    def test_initializer_table_skips_nulls_and_preserves_order(self):
        image=SyntheticImage((bytes(16)+struct.pack("<III",0x401000,0,0x401004)).hex())
        roots,metadata=prepare.table_roots(image,image.base+16,image.end)
        self.assertEqual(roots,[0x401000,0x401004])
        self.assertEqual(metadata["callbacks"],2)

    def test_initializer_table_rejects_bad_bounds_and_noncode(self):
        image=SyntheticImage(struct.pack("<I",0xffffffff).hex())
        for first,last in ((image.base+1,image.end),(image.base-4,image.end),(image.base,image.end)):
            with self.subTest(first=first,last=last),self.assertRaises(ValueError):
                prepare.table_roots(image,first,last)

    def test_verified_ilt_immediate_recovers_callback_and_callee(self):
        image=SyntheticImage("6806104000c3e900000000c3")
        thunks,metadata=prepare.ilt_entries(image,image.base+6,image.base+11)
        blocks,_=prepare.decode_graph(image,[image.base],10,thunks)
        self.assertEqual(set(blocks),{image.base,image.base+6,image.base+11})
        self.assertEqual(metadata["entries"],1)
        blocks,_=prepare.decode_graph(image,[image.base],10)
        self.assertEqual(set(blocks),{image.base})

    def test_ilt_rejects_partial_entries_nonjumps_and_unmapped_targets(self):
        for code,end in (("e900000000c3",4),("9000000000c3",5),("e9ffffff7fc3",5)):
            image=SyntheticImage(code)
            with self.subTest(code=code),self.assertRaises(ValueError):
                prepare.ilt_entries(image,image.base,image.base+end)


if __name__ == "__main__":
    unittest.main()
