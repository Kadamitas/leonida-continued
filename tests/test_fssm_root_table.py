"""CPU-only checks for root-table provenance, using fabricated decoder records."""
import copy
import shutil
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from fssm_root_table import infer, Unsupported, verify_encoding
from shader_ir import parse_line


def sg(r, n=1):
    return dict(k="sgpr", r=r, n=n)


def vg(r, n=1):
    return dict(k="vgpr", r=r, n=n)


def literal(value):
    return dict(k="imm", v=value)


def infer_ops(ops, *, corrupt=False, separate_sampler=False, unused_dword=None):
    # infer consumes the output of the ISA decoder. These records are never assembled or executed.
    entries = []
    code = bytearray()
    for op, *args in ops:
        raw = (args[0]["v"] if op.startswith("s_cbranch") else 0).to_bytes(4, "little")
        modifiers = args.pop()["mods"] if args and "mods" in args[-1] else []
        entries.append(dict(pc=len(code), enc=raw.hex(), op=op, args=args, mods=modifiers))
        code.extend(raw)
    if corrupt:
        code[0] ^= 1
    meta = dict(stage="ps", user_sgpr_reads=[dict(kind="ptr32_lo", sgpr=6)],
                descriptor_access=dict(resource_heap_directly_indexed=False, sampler_heap_directly_indexed=False,
                                       table_loads=True, truncates_table_ptr_high=True))
    if separate_sampler:
        meta["descriptor_access"]["sampler_heap_directly_indexed"] = True
        meta["user_sgpr_reads"] = [dict(kind="ptr32_lo", sgpr=11), dict(kind="const32", sgpr=9),
                                   dict(kind="ptr32_lo", sgpr=8)]
    if unused_dword is not None:
        meta.update(bindings=[], user_sgpr_reads=[])
        meta["descriptor_access"].update(table_loads=False, truncates_table_ptr_high=False)
    return infer(dict(insns=entries, code_start=0, code_end=len(code), invalid=0), code, meta,
                 unused_dword=unused_dword)


class RootTableProof(unittest.TestCase):
    program = [
        ("s_mov_b32", sg(7), literal(0)),
        ("s_load_dwordx8", sg(16, 8), sg(6, 2), literal(32)),
        ("image_sample", vg(1), vg(3, 2), sg(16, 8), sg(24, 4)),
        ("s_load_dwordx4", sg(8, 4), sg(6, 2), literal(128)),
        ("s_buffer_load_dwordx4", sg(10, 4), sg(8, 4), literal(48)),
        ("s_endpgm",),
    ]

    def test_direct_descriptors(self):
        self.assertEqual(infer_ops(self.program), {"dword": 6, "bytes": 160, "image_offsets": [32], "single_graphics_binding": True,
                         "constant_views": [{"offset": 128, "bytes": 64}]})

    def test_invalid(self):
        for name, operation in [
                ("dynamic table displacement", ("s_load_dwordx8", sg(16, 8), sg(6, 2), sg(1))),
                ("unknown high pointer", ("s_mov_b32", sg(7), literal(3))),
                ("overwritten image descriptor", ("s_mov_b32", sg(19), literal(13))),
                ("root address escapes into arithmetic", ("v_add_f32_e32", vg(1), sg(6), vg(0))),
                ("unmodelled memory op", ("flat_load_dword", vg(1), vg(2, 2))),
                ("unproved conditional merge", ("s_cbranch_scc1", literal(1))),
        ]:
            with self.subTest(name=name):
                program = copy.copy(self.program)
                if name.startswith("dynamic"):
                    program[1] = operation
                elif name.startswith("unknown"):
                    program[0] = operation
                elif name.startswith("unproved"):
                    program.insert(1, operation)  # skips the only image descriptor load
                else:
                    program.insert(2, operation)
                with self.assertRaises(Unsupported):
                    infer_ops(program)
        with self.assertRaises(Unsupported):
            infer_ops(self.program, corrupt=True)

    def test_constant_descriptors_cannot_escape(self):
        for operation in [
            ("v_add_f32_e32", vg(1), sg(8), vg(2)),
            ("s_buffer_load_dwordx4", sg(10, 4), sg(8, 4), sg(3)),
        ]:
            with self.subTest(op=operation):
                with self.assertRaises(Unsupported):
                    infer_ops([*self.program[:4], operation, *self.program[4:]])

    def test_separate_sampler_heap(self):
        # s9 initially holds the independent sampler index. It is later zeroed for the table base.
        ops = [("s_mov_b32", sg(2), sg(9)), ("s_mov_b32", sg(9), literal(0)),
               ("s_mov_b32", sg(14), sg(11)), ("s_mov_b32", sg(15), literal(0)),
               ("s_load_dwordx8", sg(16, 8), sg(8, 2), literal(32)),
               ("s_lshl_b32", sg(3), sg(2), literal(4)),
               ("s_load_dwordx4", sg(24, 4), sg(14, 2), sg(3)),
               ("image_load", vg(0), vg(1, 3), sg(16, 8)),
               ("image_sample", vg(0), vg(1, 2), sg(16, 8), sg(24, 4)),
               ("s_endpgm",)]
        plan = infer_ops(ops, separate_sampler=True)
        self.assertEqual((plan["dword"], plan["bytes"], plan["image_offsets"]), (8, 64, [32]))
        for index, wrong in [
            (0, ("s_mov_b32", sg(2), sg(8))),  # root pointer as external index
            (2, ("s_mov_b32", sg(14), sg(8))),  # dynamic sampler load from the compact root
            (5, ("s_lshl_b32", sg(3), sg(2), literal(3))),  # wrong sampler stride
            (6, ("s_load_dwordx8", sg(24, 8), sg(14, 2), sg(3))),  # resource read from sampler heap
            (7, ("s_mov_b32", sg(25), literal(0))),  # partially overwritten sampler
            (7, ("s_buffer_load_dword", sg(0), sg(24, 4), literal(0))),  # sampler used as a buffer
            (7, ("image_load", vg(0), vg(1, 3), sg(24, 8))),  # sampler used as an image
            (8, ("s_setpc_b64", sg(14, 2))),  # indirect control leaves the proven instruction range
        ]:
            with self.subTest(wrong=wrong), self.assertRaises(Unsupported):
                infer_ops([*ops[:index], wrong, *ops[index + 1:]], separate_sampler=True)

    @unittest.skipUnless(shutil.which("llvm-mc"), "LLVM AMDGPU disassembler unavailable")
    def test_canonicalized_arithmetic_requires_the_same_decoded_operands(self):
        for line in ["v_sub_f32_e64 v2, v3, v4 ; encoding: [0x02,0x00,0x04,0xd5,0x03,0x09,0x02,0x02]",
                     "v_fmac_f32_e64 v3, -v1, v4 ; encoding: [0x03,0x00,0x2b,0xd5,0x01,0x09,0x02,0x22]"]:
            record = dict(parse_line(line), pc=0)
            parsed = dict(insns=[record], code_start=0, code_end=8)
            alternative = bytearray.fromhex(record["enc"])
            alternative[7] ^= 2  # unused source field is canonicalized by the printer
            verify_encoding(parsed, alternative, shutil.which("llvm-mc"))
            with self.assertRaises(Unsupported):
                verify_encoding(parsed, alternative, None)
            alternative[0] ^= 1  # changed destination register
            with self.assertRaises(Unsupported):
                verify_encoding(parsed, alternative, shutil.which("llvm-mc"))

    def test_stage_can_ignore_partners_pointer_only_if_it_never_uses_it(self):
        ops = [("v_mov_b32_e32", vg(1), sg(7)), ("s_endpgm",)]
        plan = infer_ops(ops, unused_dword=8)
        self.assertEqual((plan["dword"], plan["bytes"], plan["image_offsets"], plan["constant_views"]),
                         (8, 32, [], []))
        for prefix in [[("s_mov_b32", sg(7), sg(8))], [("s_setpc_b64", sg(16, 2))]]:
            with self.assertRaises(Unsupported):
                infer_ops([*prefix, *ops], unused_dword=8)

    def test_dynamic_index_requires_the_complete_bounded_view(self):
        root = [("s_mov_b32", sg(7), literal(0)), ("s_load_dwordx4", sg(16, 4), sg(6, 2), literal(32))]
        row = ("buffer_load_dwordx4", vg(4, 4), vg(1), sg(16, 4), literal(0), {"mods": ["idxen"]})
        end = ("s_endpgm",)
        self.assertEqual(infer_ops([*root, row, row, end])["constant_views"],
                         [dict(offset=32, bytes=65536, record_stride=16)])
        for bad in [(*row[:-1], {"mods": ["idxen", "offen"]}), (*row[:-2], literal(4), row[-1]),
                    ("s_buffer_load_dword", sg(0), sg(16, 4), literal(0)),
                    ("buffer_store_dwordx4", *row[1:])]:
            with self.subTest(bad=bad), self.assertRaises(Unsupported):
                infer_ops([*root, row, bad, end])


if __name__ == "__main__":
    unittest.main()
