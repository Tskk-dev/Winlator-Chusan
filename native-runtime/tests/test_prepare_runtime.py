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
    def priority_image(self):
        code=bytearray(b"\xcc"*64)
        code[0:6]=bytes.fromhex("e80b000000c3")
        code[16:21]=bytes.fromhex("e90b000000")
        code[32:38]=bytes.fromhex("e803000000c3")
        code[40]=0xc3
        code[48:54]=bytes.fromhex("e803000000c3")
        code[56]=0xc3
        return SyntheticImage(code.hex())

    def test_observed_root_closure_precedes_unrelated_cold_expansion(self):
        image=self.priority_image();base=image.base
        retained=[base,base+5,base+16]
        blocks,frontier=prepare.decode_graph(image,[base],6,retained=retained,priority=[base+48])
        self.assertEqual(set(blocks),set(retained+[base+48,base+53,base+56]))
        self.assertEqual(frontier,[base+32])

    def test_priority_walk_follows_direct_edges_through_retained_blocks(self):
        image=self.priority_image();base=image.base
        retained=[base,base+5,base+16]
        blocks,frontier=prepare.decode_graph(image,[base,base+48],6,retained=retained,priority=[base+16])
        self.assertEqual(set(blocks),set(retained+[base+32,base+37,base+40]))
        self.assertEqual(frontier,[base+48])

    def test_retained_priority_loop_is_finite(self):
        image=SyntheticImage("ebfe")
        blocks,frontier=prepare.decode_graph(image,[image.base],1,retained=[image.base],priority=[image.base])
        self.assertEqual(set(blocks),{image.base})
        self.assertEqual(frontier,[])

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

    def test_readonly_callback_table_referenced_by_code_seeds_verified_thunks(self):
        image=SyntheticImage((bytes.fromhex("6810104000c3e900000000c3")+bytes(4)+struct.pack("<I",0x401006)).hex())
        image.is_exec=lambda address:image.base<=address<image.base+12
        thunks,_=prepare.ilt_entries(image,image.base+6,image.base+11)
        blocks,_=prepare.decode_graph(image,[image.base],10,thunks,[(image.base+16,image.end)])
        self.assertEqual(set(blocks),{image.base,image.base+6,image.base+11})
        blocks,_=prepare.decode_graph(image,[image.base],10,thunks)
        self.assertEqual(set(blocks),{image.base})
        blocks,frontier=prepare.decode_graph(image,[image.base,image.base+11],2,thunks,[(image.base+16,image.end)])
        self.assertEqual(set(blocks),{image.base,image.base+11})
        self.assertEqual(frontier,[image.base+6])

    def test_retained_blocks_require_matching_identities_and_valid_addresses(self):
        report=dict(source_sha256="source",kit_commit="translator",block_addresses=["0x401000","0x401000"])
        self.assertEqual(prepare.retained_roots(report,"source","translator"),[0x401000])
        for change in (dict(source_sha256="other"),dict(kit_commit="other"),
                       dict(block_addresses=[-1]),dict(block_addresses=["0x100000000"]),
                       dict(block_addresses="0x401000")):
            with self.subTest(change=change),self.assertRaises(ValueError):
                prepare.retained_roots(report|change,"source","translator")
        for invalid in (None,[],{}):
            with self.subTest(invalid=invalid),self.assertRaises(ValueError):
                prepare.retained_roots(invalid,"source","translator")


if __name__ == "__main__":
    unittest.main()
