"""Infer bounded root-table accesses for an existing graphics FSSM translation.

Prints a proof for inspection. Choosing the logical source root dword requires the draw's
actual bindings. Neither the shader index nor its SPIR-V module is changed.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from shader_ir import parse_file, parse_line
from shader_meta import _sgpr_regs, _split_operands


class Unsupported(ValueError):
    pass


def require(ok, why):
    if not ok:
        raise Unsupported(why)


def imm(arg):
    if arg.get("k") == "special" and arg.get("t") == "null":
        return 0
    require(arg.get("k") == "imm" and type(arg.get("v")) is int and arg["v"] >= 0, "dynamic offset")
    return arg["v"]


def clean_regs(operand):
    require(not operand.get("abs") and not operand.get("neg"), "modified scalar operand")
    return _sgpr_regs(operand)


def verify_encoding(parsed, code, llvm_mc):
    pos = parsed["code_start"]
    for ins in parsed["insns"]:
        raw = bytes.fromhex(ins["enc"])
        require(raw and ins["pc"] == pos and pos + len(raw) <= len(code), "incomplete instruction range")
        actual = code[pos:pos + len(raw)]
        if actual != raw:
            # LLVM can canonicalize unused source bits when printing a VOP3 encoding. Compare the actual
            # instruction's decoded opcode, operands, modifiers and canonical bytes. Never infer a control
            # or memory instruction from an encoding mismatch.
            require(llvm_mc is not None and ins["op"] in {"v_sub_f32_e64", "v_fmac_f32_e64"} and len(raw) == 8,
                    f"machine bytes differ at {pos}")
            try:
                result = subprocess.run([str(llvm_mc), "-triple=amdgcn-amd-amdhsa", "-mcpu=gfx1013",
                                         "-disassemble", "-show-encoding"], capture_output=True, text=True, timeout=10,
                                        input=" ".join(f"0x{byte:02x}" for byte in actual) + "\n")
            except (OSError, subprocess.TimeoutExpired) as error:
                raise Unsupported(f"could not verify machine bytes at {pos}") from error
            lines = [line for line in result.stdout.splitlines() if line.strip() and line.strip() != ".text"]
            require(result.returncode == 0 and not result.stderr.strip() and len(lines) == 1 and
                    parse_line(lines[0]) == {k: ins[k] for k in ("op", "args", "mods", "enc")},
                    f"alternate arithmetic encoding does not decode identically at {pos}")
        pos += len(raw)
    require(pos == parsed["code_end"], "incomplete machine bytes")


def infer(parsed, code, meta, llvm_mc=None, unused_dword=None):
    stage = meta["stage"]
    require(stage in {"vs", "ps"} and not parsed["invalid"], "unsupported stage or invalid instructions")
    verify_encoding(parsed, code, llvm_mc)
    if unused_dword is not None:
        require(0 <= unused_dword <= 30 and meta.get("bindings") == [] and
                not any(item["sgpr"] == unused_dword for item in meta.get("user_sgpr_reads", [])) and
                meta.get("descriptor_access") == dict(resource_heap_directly_indexed=False,
                    sampler_heap_directly_indexed=False, table_loads=False, truncates_table_ptr_high=False),
                "stage cannot ignore the paired table")
        return infer_roles(parsed, meta, unused_dword, None, unused=True)
    roots = [item["sgpr"] for item in meta.get("user_sgpr_reads", []) if item["kind"] == "ptr32_lo"]
    direct_sampler = meta.get("descriptor_access", {}).get("sampler_heap_directly_indexed")
    require(type(direct_sampler) is bool and (not direct_sampler or stage == "ps") and
            len(roots) == (2 if direct_sampler else 1) and len(set(roots)) == len(roots) and
            all(0 <= r <= 30 for r in roots), "unsupported root or sampler pointers")
    require(meta.get("descriptor_access") == dict(resource_heap_directly_indexed=False,
            sampler_heap_directly_indexed=direct_sampler, table_loads=True, truncates_table_ptr_high=True),
            "unsupported descriptor access")
    if not direct_sampler:
        return infer_roles(parsed, meta, roots[0], None)
    # Assign roles from their instruction uses. Metadata does not identify which pointer is the sampler heap.
    plans = []
    for root, sampler in (roots, roots[::-1]):
        try:
            plans.append(infer_roles(parsed, meta, root, sampler))
        except Unsupported:
            pass
    require(len(plans) == 1, "could not establish distinct root table and sampler heap")
    return plans[0]


def infer_roles(parsed, meta, root, sampler, unused=False):
    stage = meta["stage"]
    insns = parsed["insns"]
    require(insns and insns[0]["pc"] == parsed["code_start"], "missing first instruction")
    pcs = {ins["pc"] for ins in insns}
    initial = {root: "root"}
    if sampler is not None:
        initial[sampler] = "sampler_heap"
        for item in meta["user_sgpr_reads"]:
            if item["kind"] == "const32":
                require(item["sgpr"] not in initial, "overlapping initial scalar roles")
                initial[item["sgpr"]] = "input"
    flow = {insns[0]["pc"]: initial}
    images, constants, descriptors = set(), {}, {}
    indexed = set()
    size = 0
    for index, ins in enumerate(insns):
        pc, op, args = ins["pc"], ins["op"], ins["args"]
        raw = bytes.fromhex(ins["enc"])
        end = pc + len(raw)
        state = flow.pop(pc, None)
        require(state is not None, f"unreachable instruction at {pc}")
        reads, writes = _split_operands(ins)
        after = state.copy()
        if op.startswith("v_cmp") and len(writes) == 1:
            writes = [writes[0], writes[0] + 1]
        for reg in writes:
            after.pop(reg, None)
        # Values whose meaning changes when a table or its constant buffers move cannot escape.
        sensitive = {reg for reg in reads if state.get(reg) not in {None, "zero", "input"}}
        if op == "s_swappc_b64":
            require(stage == "vs" and index == 0 and meta.get("vertex_inputs") and
                    [clean_regs(arg) for arg in args] == [[4, 5], [4, 5]] and not sensitive,
                    "unsupported indirect call")
            # rdna2spirv replaces this leading fetch call with Vulkan vertex inputs.
        elif op == "s_mov_b32" and len(args) == 2:
            src = clean_regs(args[1])
            if src and len(src) == 1 and src[0] in state and len(writes) == 1:
                after[writes[0]] = state[src[0]]
            elif len(writes) == 1 and args[1].get("k") == "imm" and args[1].get("v") == 0:
                after[writes[0]] = "zero"
            else:
                require(not sensitive, "root or descriptor copied to an untracked destination")
        elif op == "s_lshl_b32":
            require(sampler is not None and len(args) == 3 and len(writes) == 1 and not ins["mods"] and
                    len(clean_regs(args[1])) == 1 and state.get(clean_regs(args[1])[0]) == "input" and
                    imm(args[2]) == 4 and not sensitive, "unproved sampler index")
            after[writes[0]] = "sampler_offset"
        elif op in {"s_load_dwordx4", "s_load_dwordx8"}:
            require(len(args) == 3 and not ins["mods"], "unsupported table load")
            base = clean_regs(args[1])
            base_kind = [state.get(reg) for reg in base]
            lanes = 8 if op.endswith("x8") else 4
            require(len(writes) == lanes, "invalid descriptor destination")
            if base_kind == ["sampler_heap", "zero"]:
                index_regs = clean_regs(args[2])
                require(lanes == 4 and len(index_regs) == 1 and state.get(index_regs[0]) == "sampler_offset",
                        "unproved external sampler descriptor")
                for lane, reg in enumerate(writes):
                    after[reg] = ("sampler", pc, lane)
            else:
                require(base_kind == ["root", "zero"], "unknown table base")
                off = imm(args[2])
                require(off % 32 == 0 and off <= 65536 - 32, "invalid descriptor load")
                require(descriptors.setdefault(off, lanes) == lanes, "incompatible descriptor types")
                for lane, reg in enumerate(writes):
                    after[reg] = (off, lanes, lane)
                size = max(size, off + 32)
        elif re.fullmatch(r"s_buffer_load_dword(?:x(?:2|4|8|16))?", op):
            require(len(args) == 3 and not ins["mods"], "unsupported constant read")
            desc = [state.get(reg) for reg in clean_regs(args[1])]
            require(len(desc) == 4 and isinstance(desc[0], tuple), "unknown constant descriptor")
            off = desc[0][0]
            require(type(off) is int and desc == [(off, 4, lane) for lane in range(4)], "modified constant descriptor")
            require(off not in indexed, "mixed scalar and indexed reads")
            length = 4 * int(op.split("x")[-1] if "x" in op else 1)
            offset = imm(args[2])
            require(offset % 4 == 0 and offset <= 65536 - length, "constant range out of bounds")
            constants[off] = max(constants.get(off, 0), offset + length)
        elif op == "buffer_load_dwordx4":
            require(len(args) == 4 and ins["mods"] == ["idxen"] and args[0].get("k") == "vgpr" and
                    args[0].get("n") == 4 and args[1].get("k") == "vgpr" and args[1].get("n") == 1 and
                    imm(args[3]) == 0, "unproved indexed buffer access")
            regs = clean_regs(args[2])
            desc = [state.get(r) for r in regs]
            require(len(desc) == 4 and isinstance(desc[0], tuple), "unknown indexed buffer")
            off = desc[0][0]
            require(type(off) is int and desc == [(off, 4, lane) for lane in range(4)] and
                    sensitive == set(regs), "overwritten or escaping indexed descriptor")
            require(off in indexed or off not in constants, "mixed scalar and indexed reads")
            indexed.add(off)
            # The per-vertex index is arbitrary. The runtime must accept the addressing mode and
            # copy the entire declared view within this cap before using a bounded image list.
            constants[off] = 65536
        elif op in {"image_sample", "image_sample_lz", "image_load"}:
            require(len(args) == (3 if op == "image_load" else 4), "unsupported image operands")
            regs = clean_regs(args[2])
            desc = [state.get(reg) for reg in regs]
            require(len(desc) == 8 and isinstance(desc[0], tuple), "unknown image descriptor")
            off = desc[0][0]
            require(type(off) is int and desc == [(off, 8, lane) for lane in range(8)], "overwritten image")
            expected = set(regs)
            if sampler is not None and op != "image_load":
                sam = clean_regs(args[3])
                slots = [state.get(reg) for reg in sam]
                require(len(slots) == 4 and isinstance(slots[0], tuple) and slots[0][0] == "sampler" and
                        slots == [("sampler", slots[0][1], lane) for lane in range(4)], "unknown sampling descriptor")
                expected.update(sam)
            require(sensitive == expected, "unmodelled image operands")
            images.add(off)
        else:
            # Only instructions outside global/texture/descriptor memory and indirect control.
            require(op in {"s_inst_prefetch", "s_mov_b64", "s_movk_i32", "s_wqm_b64", "s_and_b64", "s_andn2_b64", "s_nop",
                           "s_waitcnt", "s_endpgm", "s_cmp_eq_u32", "s_branch", "s_cbranch_scc0",
                           "s_cbranch_scc1", "exp", "v_nop", "v_mov_b32_e32", "v_log_f32_e32",
                           "v_exp_f32_e32", "v_cmpx_lt_f32_e32", "s_and_b32", "v_and_b32_e32", "v_bfi_b32",
                           "v_cmp_lg_f32_e32", "v_cndmask_b32_e32", "v_med3_f32", "v_readfirstlane_b32",
                           "v_sqrt_f32_e32", "v_sub_f32_e32", "v_sub_f32_e64", "v_rcp_f32_e32"} or
                    op.startswith(("v_interp_", "v_fma", "v_mul_", "v_add_", "v_cvt_", "v_max_")),
                    f"unmodelled opcode {op}")
            require(not sensitive, f"root or descriptor escapes into {op} at {pc}")
        targets = []
        if op == "s_branch" or op.startswith("s_cbranch_"):
            require(len(args) == 1 and args[0]["k"] == "imm", "unmodelled branch")
            targets.append(end + 4 * int.from_bytes(raw[:2], "little", signed=True))
        if op not in {"s_branch", "s_endpgm"}:
            targets.append(end)
        for target in targets:
            require(target > pc and target in pcs, "backward or invalid edge")
            if target in flow:
                old = flow[target]
                # Keep different incoming values tainted even when only one path carried a pointer.
                flow[target] = {r: old.get(r) if old.get(r) == after.get(r) else "ambiguous"
                                for r in old.keys() | after.keys()}
            else:
                flow[target] = after.copy()
    require(not flow, "incomplete control flow")
    require(set(descriptors) == images | constants.keys(), "unused or unaccounted descriptor")
    if unused:
        require(size == 0, "paired stage accesses the remapped table")
        # Guarded graphics plans must agree on a dword and capture at least one 32-byte descriptor slot.
        # This stage requires no reads; its partner supplies the actual table and descriptor bounds.
        size = 32
    else:
        require(size, "stage does not access a table")
    return {"dword": root, "bytes": size, "image_offsets": sorted(images), "single_graphics_binding": True,
            "constant_views": [dict(offset=off, bytes=n, **({"record_stride": 16} if off in indexed else {}))
                               for off, n in sorted(constants.items())]}


def prove(listing, fssm, module, entry, llvm_mc=None, unused_dword=None):
    digest = lambda data: hashlib.sha256(data).hexdigest()
    code = fssm.read_bytes()
    require(digest(code) == entry.get("fssm_sha256") and digest(listing.read_bytes()) == entry.get("src_sha256"),
            "FSSM or source identity mismatch")
    return {"plan": infer(parse_file(listing), code, entry, llvm_mc, unused_dword), "module_sha256": digest(module.read_bytes()),
            "fssm_sha256": digest(code), "listing_sha256": entry["src_sha256"]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for arg in ("index", "listing", "fssm", "module"):
        parser.add_argument(arg, type=Path)
    parser.add_argument("entry")
    parser.add_argument("--source-dword", type=int, choices=range(32), metavar="0..31",
                        help="logical dword of the unique table confirmed by the draw")
    parser.add_argument("--llvm-mc", type=Path, help="verify canonicalized register arithmetic against its original bytes")
    parser.add_argument("--unused-dword", type=int, choices=range(31), metavar="0..30",
                        help="prove that this binding-free stage ignores its partner's remapped register")
    options = parser.parse_args()
    entry = json.loads(options.index.read_bytes())[options.entry]
    report = prove(options.listing, options.fssm, options.module, entry, options.llvm_mc, options.unused_dword)
    if options.source_dword is not None:
        report["plan"]["source_dword"] = options.source_dword
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
