"""Prepare bounded AOT startup from the pinned PE; keep all private output in .work."""
import argparse
from collections import defaultdict, deque
import hashlib
import importlib
import json
from pathlib import Path
import re
import subprocess
import sys
from types import SimpleNamespace

import capstone
from capstone.x86_const import X86_OP_IMM, X86_OP_REG
import pefile

ROOT = Path(__file__).resolve().parents[1]


def checked_header(source):
    """Guard kit-internal memory helpers too, without modifying the pinned checkout."""
    declaration = "void runtime_memory_check(uint32_t address, uint32_t size, int write);\n"
    source = source.replace("/* ---------------------------------------------------------------- memory */",
                            declaration + "/* ---------------------------------------------------------------- memory */", 1)
    helpers = {"rd8": (1, 0), "rd16": (2, 0), "rd32": (4, 0), "rd64": (8, 0),
               "wr8": (1, 1), "wr16": (2, 1), "wr32": (4, 1), "wr64": (8, 1),
               "rdf32": (4, 0), "rdf64": (8, 0), "wrf32": (4, 1), "wrf64": (8, 1),
               "rdf80": (10, 0), "wrf80": (10, 1), "wrbcd80": (10, 1)}
    for name, (size, write) in helpers.items():
        pattern = rf"((?:RECOMP_HOT_INLINE|static inline) [^\n]+\b{name}\(uint32_t a[^\n]*\) \{{)"
        source, count = re.subn(pattern, lambda match: match[1] + f"\n    runtime_memory_check(a,{size}u,{write});", source)
        if count != 1:
            raise ValueError("Pinned memory helper changed: " + name)
    return source


def decode_graph(image, roots, limit, thunks=frozenset(), readonly=(), retained=(), priority=()):
    """Follow actual direct flow, splitting at every transfer; indirect flow stays pending."""
    queue = deque(roots)
    keep = deque(dict.fromkeys(retained))
    urgent = deque(priority)
    urgent_seen = set()
    direct_edges = {}
    decoded = {}
    instruction_bytes = {}
    frontier = set()
    table_cache={}
    def callbacks(value):
        if value in thunks:
            return (value,)
        if value in table_cache:
            return table_cache[value]
        found=[]
        if thunks and not value%4:
            for first,last in readonly:
                if first<=value<last and not image.is_exec(value):
                    cursor=value
                    while cursor+4<=last:
                        target=int.from_bytes(image.data[cursor-image.base:cursor-image.base+4],"little")
                        if target not in thunks:break
                        found.append(target);cursor+=4
                    break
        table_cache[value]=found
        return found
    while keep or urgent or queue:
        is_urgent = False
        if keep:
            address = keep.popleft()
        elif urgent:
            address = urgent.popleft()
            if address in urgent_seen:
                continue
            urgent_seen.add(address)
            is_urgent = True
        else:
            address = queue.popleft()
        if address in decoded:
            if is_urgent:
                urgent.extend(direct_edges[address])
            continue
        if len(decoded) == limit:
            frontier.add(address)
            break
        if not image.is_exec(address):
            frontier.add(address)
            continue
        pc, block, direct = address, [], []
        for _ in range(128):
            if not image.is_exec(pc):
                raise ValueError(f"Code falls outside executable sections: {pc:08x}")
            got = list(image.md.disasm(image.data[pc-image.base:pc-image.base+15], pc, count=1))
            if not got:
                raise ValueError(f"Cannot decode {pc:08x}")
            insn = got[0]
            for byte in range(pc, pc + insn.size):
                if byte in instruction_bytes and instruction_bytes[byte] != pc:
                    raise ValueError(f"Overlapping instruction decode: {pc:08x}")
                instruction_bytes[byte] = pc
            block.append(insn)
            if insn.mnemonic in ("push","mov"):
                for operand in insn.operands:
                    if operand.type==X86_OP_IMM:
                        queue.extend(callbacks(operand.imm))
            pc += insn.size
            jump = insn.group(capstone.CS_GRP_JUMP)
            call = insn.group(capstone.CS_GRP_CALL)
            if jump or call:
                if insn.operands and insn.operands[0].type == X86_OP_IMM:
                    direct.append(insn.operands[0].imm & 0xffffffff)
                if call or insn.mnemonic != "jmp":
                    direct.append(pc)
                break
            if insn.group(capstone.CS_GRP_RET) or insn.mnemonic in ("int3", "int", "ud2", "hlt"):
                break
        else:
            direct.append(pc)
        decoded[address] = block
        direct_edges[address] = direct
        (urgent if is_urgent else queue).extend(direct)
    frontier.update(address for pending in (keep,urgent,queue) for address in pending if address not in decoded)
    return decoded, sorted(frontier)


def table_roots(image, start, end):
    if start%4 or end%4 or end<start or end-start>0x100000 or start<image.base or end>image.end:
        raise ValueError("Invalid initializer-table bounds")
    raw=image.data[start-image.base:end-image.base]
    roots=[int.from_bytes(raw[i:i+4],"little") for i in range(0,len(raw),4)]
    roots=[value for value in roots if value]
    if any(not image.is_exec(value) for value in roots):
        raise ValueError("Initializer table contains a non-executable pointer")
    return roots,{"start":hex(start),"end":hex(end),"callbacks":len(roots),"sha256":hashlib.sha256(raw).hexdigest()}


def ilt_entries(image,start,end):
    if start<image.base or end>image.end or end<=start or (end-start)%5 or end-start>0x100000:
        raise ValueError("Invalid incremental-link table bounds")
    for address in range(start,end,5):
        offset=address-image.base
        target=address+5+int.from_bytes(image.data[offset+1:offset+5],"little",signed=True)
        if image.data[offset]!=0xe9 or not image.is_exec(address) or not image.is_exec(target):
            raise ValueError("Incremental-link table is not a sequence of executable relative jumps")
    return set(range(start,end,5)),{"start":hex(start),"end":hex(end),"entries":(end-start)//5,
                                   "sha256":hashlib.sha256(image.data[start-image.base:end-image.base]).hexdigest()}


def retained_roots(report,source_hash,commit):
    if not isinstance(report,dict):
        raise ValueError("Invalid retained block report")
    values=report.get("block_addresses")
    if report.get("source_sha256")!=source_hash or report.get("kit_commit")!=commit:
        raise ValueError("Retained blocks differ from pinned source or translator")
    if not isinstance(values,list) or len(values)>100000 or any(
        not isinstance(value,str) or not re.fullmatch(r"0x[0-9a-f]{1,8}",value) for value in values):
        raise ValueError("Invalid retained block addresses")
    return list(dict.fromkeys(int(value,16) for value in values))


def generate(executable, kit, output, roots, limit, profile=None, tables=(), ilt=None, retained=None):
    profile = profile or json.loads((ROOT / "profiles/startup-initializer.json").read_text())
    source_hash = hashlib.sha256(executable.read_bytes()).hexdigest()
    if source_hash != profile["source_sha256"]:
        raise ValueError("Executable differs from pinned main profile")
    commit = subprocess.check_output(["git", "-C", str(kit), "rev-parse", "HEAD"], text=True).strip()
    if commit != profile["kit_commit"]:
        raise ValueError("Translator commit differs from pinned profile")
    previous=retained_roots(retained,source_hash,commit) if retained is not None else []
    if len(previous)>limit:
        raise ValueError("Block limit is smaller than the retained block count")
    subprocess.run(["git", "-C", str(kit), "diff", "--exit-code", "HEAD", "--"], check=True, capture_output=True)
    if subprocess.check_output(["git", "-C", str(kit), "ls-files", "--others", "--exclude-standard",
                                "--", "tools/recomp", "runtime"], text=True).strip():
        raise ValueError("Untracked translator sources")
    sys.path.insert(0, str(kit / "tools/recomp"))
    module = importlib.import_module("translate")
    class RuntimeImage(module.Image):
        def _size_word(self, nbytes, mnem):
            # The pinned kit's recovery formatter omits its supported vector widths.
            return {16: "xmmword", 32: "ymmword"}.get(nbytes) or super()._size_word(nbytes, mnem)

        def to_insn(self, ci):
            # MOVSD also names the implicit string instruction. Keep the SSE operands.
            if ci.mnemonic=="movsd" and any(op.type==X86_OP_REG and ci.reg_name(op.reg).startswith("xmm") for op in ci.operands):
                operands=[self._op_text(ci,op,"MOVSD") for op in ci.operands]
                return module.Insn(ci.address,"MOVSD",None,operands,f"{ci.address:08x}  MOVSD "+",".join(operands))
            return super().to_insn(ci)

    image = RuntimeImage(str(executable))
    image.md.detail = True
    if any(not image.is_exec(address) for address in previous):
        raise ValueError("Retained block is not executable")
    with pefile.PE(str(executable), fast_load=True) as pe:
        pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_IMPORT"],
                                               pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_TLS"]])
        entry = image.base + pe.OPTIONAL_HEADER.AddressOfEntryPoint
        imports = []
        for dll in pe.DIRECTORY_ENTRY_IMPORT:
            for symbol in dll.imports:
                imports.append({"iat": symbol.address, "dll": dll.dll.decode("ascii"),
                                "name": symbol.name.decode("ascii") if symbol.name else "#" + str(symbol.ordinal)})
        regions = [{"base": image.base, "size": pe.OPTIONAL_HEADER.SizeOfHeaders, "flags": 1}]
        for section in pe.sections:
            regions.append({"base": image.base + section.VirtualAddress,
                            "size": max(section.Misc_VirtualSize, section.SizeOfRawData),
                            "flags": (1 if section.Characteristics & 0x40000000 else 0)
                            | (2 if section.Characteristics & 0x80000000 else 0)})
        tls = None
        if hasattr(pe, "DIRECTORY_ENTRY_TLS"):
            value = pe.DIRECTORY_ENTRY_TLS.struct
            tls = {"start": value.StartAddressOfRawData, "end": value.EndAddressOfRawData,
                   "index": value.AddressOfIndex, "callbacks": value.AddressOfCallBacks,
                   "zero_fill": value.SizeOfZeroFill}
    explicit_roots=list(roots)
    table_metadata=[]
    for start,end in tables:
        callbacks,description=table_roots(image,start,end)
        roots=[*roots,*callbacks];table_metadata.append(description)
    roots=list(dict.fromkeys(roots))
    thunks,ilt_metadata=ilt_entries(image,*ilt) if ilt else (frozenset(),None)
    # Broad table discovery starts after a baseline exists, so its cutoff cannot displace startup code.
    readonly=[(r["base"],r["base"]+r["size"]) for r in regions if r["flags"]==1] if previous else []
    priority=explicit_roots if previous else []
    blocks, frontier = decode_graph(image, [entry,*roots], limit,thunks,readonly,previous,priority)
    if not set(previous).issubset(blocks):
        raise ValueError("Block cutoff omitted retained code")
    class RuntimeTranslator(module.Translator):
        def _emit(self, fn, index, insn, mnemonic, following, live):
            if mnemonic=="MOVSD" and insn.ops:
                dst,src=[module.parse_operand(op) for op in insn.ops]
                if dst.kind=="xmm" and src.kind in ("xmm","mem"):
                    value=f"movsd_{index}"
                    expression=(f"((uint64_t)c->xmm[{src.reg}][0]|((uint64_t)c->xmm[{src.reg}][1]<<32))"
                                if src.kind=="xmm" else f"rd64({module.addr_expr(src)})")
                    lines=[f"uint64_t {value}={expression};",f"c->xmm[{dst.reg}][0]=(uint32_t){value};",
                           f"c->xmm[{dst.reg}][1]=(uint32_t)({value}>>32);"]
                    if src.kind=="mem":lines.append(f"c->xmm[{dst.reg}][2]=c->xmm[{dst.reg}][3]=0;")
                    return lines
                if dst.kind=="mem" and src.kind=="xmm":
                    return [f"wr64({module.addr_expr(dst)},(uint64_t)c->xmm[{src.reg}][0]|((uint64_t)c->xmm[{src.reg}][1]<<32));"]
                raise module.TranslateError("Unsupported scalar MOVSD operands")
            if mnemonic == "PADDD":
                dst,src=[module.parse_operand(op) for op in insn.ops]
                if dst.kind=="xmm" and src.kind in ("mem","xmm"):
                    prefix=f"paddd_{index}"
                    lines=[]
                    if src.kind=="mem":
                        lines.extend([f"uint32_t {prefix}_address = {module.addr_expr(src)};",
                                      f"if ({prefix}_address & 15u) {{ recomp_int(c,13); return; }}",
                                      f"runtime_memory_check({prefix}_address,16u,0);"])
                        values=[f"rd32({prefix}_address+{4*lane}u)" for lane in range(4)]
                    else:
                        values=[f"c->xmm[{src.reg}][{lane}]" for lane in range(4)]
                    lines.append(f"uint32_t {prefix}_source[4] = {{"+",".join(values)+"};")
                    lines.extend(f"c->xmm[{dst.reg}][{lane}] += {prefix}_source[{lane}];" for lane in range(4))
                    return lines
            if mnemonic in ("MOVLPD", "MOVLPS", "MOVHPD", "MOVHPS"):
                # The pinned emitter reads an unused memory destination on stores.
                dst, src = [module.parse_operand(op) for op in insn.ops]
                half = 2 if mnemonic in ("MOVHPD", "MOVHPS") else 0
                if dst.kind == "xmm" and src.kind == "mem":
                    address = module.addr_expr(src)
                    return [f"c->xmm[{dst.reg}][{half+i}] = rd32({address} + {4*i}u);" for i in range(2)]
                if dst.kind == "mem" and src.kind == "xmm":
                    address = module.addr_expr(dst)
                    return [f"wr32({address} + {4*i}u, c->xmm[{src.reg}][{half+i}]);" for i in range(2)]
                raise module.TranslateError("Unsupported half-vector move operands")
            return super()._emit(fn, index, insn, mnemonic, following, live)

        def goto_target(self, fn, target, insn):
            # Every edge returns to the budgeted dispatcher, including self-loops.
            return [f"recomp_jump(c, 0x{target:08x}u); return;"]

        def emit_indirect_jump(self, fn, index, insn, operand):
            return [f"recomp_jump(c, {module.read_op(operand, 32)}); return;"]

    emitter = RuntimeTranslator(image, set(blocks), SimpleNamespace(eager_flags=True, allow_unmodelled=None))
    bodies = defaultdict(lambda:['#include "runtime.h"'])
    metadata = []
    for address, instructions in sorted(blocks.items()):
        body=bodies[f"blocks_{address>>16:04x}.c"]
        end = instructions[-1].address + instructions[-1].size
        try:
            function = module.Function(address, f"block_{address:08x}", end-address,
                                       [image.to_insn(insn) for insn in instructions])
            function.measure(image)
            emitter.prepare(function, strict=True)
            # Host setjmp frames cannot survive block dispatch. Preserve the original
            # guest FS chain; exception dispatch itself stops until implemented.
            function.seh_sites = {}
            function.seh_restores = set()
            lines = emitter.translate(function, ())
            if any("goto L_" in line for line in lines):
                raise ValueError("Local transfer bypasses dispatcher budget")
            lines = [line for line in lines if not re.match(r"^L_[0-9a-f]+: ;$", line)]
            if any("recomp_seh_" in line for line in lines):
                raise ValueError("Host SEH instrumentation exceeds block-runtime contract")
            if any("g_mem" in line or "x87_f" in line for line in lines):
                raise ValueError("Unchecked memory helper needs qualification")
            if any("recomp_unmodelled(" in line for line in lines):
                metadata.append({"va": address, "translation_error": "Emitter inserted an unsupported-instruction guard"})
            body.extend(lines)
        except (module.TranslateError, ValueError) as error:
            body.append(f"void fn_{address:08x}(X86 *c) {{ recomp_unmodelled(c, 0x{address:08x}u); }}")
            metadata.append({"va": address, "translation_error": str(error)})
    table=['#include "runtime.h"']
    table.extend(f"extern void fn_{address:08x}(X86 *c);" for address in sorted(blocks))
    table.append("const RuntimeBlock runtime_blocks[] = {")
    for address, instructions in sorted(blocks.items()):
        table.append(f"{{0x{address:08x}u,0x{instructions[-1].address:08x}u,fn_{address:08x}}},")
    table.extend(["};", f"const uint32_t runtime_block_count = {len(blocks)}u;"])
    bodies["blocks.c"]=table
    output.mkdir(parents=True, exist_ok=True)
    (output / "x86.h").write_text(checked_header((kit / "runtime/x86.h").read_text()), encoding="utf-8")
    units={}
    for filename,lines in sorted(bodies.items()):
        data=("\n".join(lines)+"\n").encode("utf-8")
        (output/filename).write_bytes(data);units[filename]=hashlib.sha256(data).hexdigest()
    header = [f"#define RUNTIME_IMAGE_BASE {image.base}u", f"#define RUNTIME_IMAGE_SIZE {image.size}u",
              f"#define RUNTIME_ENTRY {entry}u", "static const RuntimeRegion image_regions[] = {"]
    header.extend("{%du,%du,%du}," % (r["base"], r["size"], r["flags"]) for r in regions)
    header.extend(["};", "static const RuntimeImport image_imports[] = {"])
    header.extend("{%du,%s,%s}," % (r["iat"], json.dumps(r["dll"]), json.dumps(r["name"])) for r in imports)
    header.extend(["};", "static const RuntimeTls image_tls = {%du,%du,%du,%du,%du};" % (
        tuple(tls[k] for k in ("start", "end", "index", "callbacks", "zero_fill")) if tls else (0, 0, 0, 0, 0))])
    (output / "image_profile.h").write_text("\n".join(header) + "\n", encoding="utf-8")
    (output / "image.bin").write_bytes(image.data)
    report = {"source_sha256": source_hash, "kit_commit": commit, "entry": hex(entry),
              "blocks": len(blocks), "instructions": sum(map(len, blocks.values())),
              "frontier": [hex(x) for x in frontier], "translation_errors": metadata,
              "extra_roots": [hex(x) for x in roots], "eager_flags": True, "seh": "guest_chain_only",
              "image_sha256": hashlib.sha256(image.data).hexdigest(), "full_boot_verified": False}
    report["initializer_tables"]=table_metadata
    report["retained_blocks"]=len(previous)
    report["priority_roots"]=[hex(address) for address in priority]
    report["readonly_callback_recovery"]=bool(previous)
    report["block_addresses"]=[hex(address) for address in sorted(blocks)]
    report["incremental_link_table"]=ilt_metadata
    report["translation_units"]=units
    report["indirect_sites"] = [hex(insn.address) for instructions in blocks.values() for insn in instructions
                                if (insn.group(capstone.CS_GRP_CALL) or insn.group(capstone.CS_GRP_JUMP))
                                and insn.operands and insn.operands[0].type != X86_OP_IMM]
    (output / "preparation.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--kit", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--max-blocks", type=int, default=2048)
    parser.add_argument("--root", action="append", default=[], type=lambda v: int(v, 0))
    parser.add_argument("--init-table", action="append", default=[], type=lambda v: tuple(int(p,0) for p in v.split(":")))
    parser.add_argument("--ilt-range", type=lambda v: tuple(int(p,0) for p in v.split(":")))
    parser.add_argument("--retain-report",type=Path)
    args = parser.parse_args()
    try:
        if not args.output.resolve().is_relative_to(ROOT / ".work") or not 1 <= args.max_blocks <= 100000:
            raise ValueError("Output must be under .work and block limit between 1 and 100000")
        if any(len(table)!=2 for table in args.init_table):
            raise ValueError("Initializer tables use START:END bounds")
        if args.ilt_range and len(args.ilt_range)!=2:
            raise ValueError("Incremental-link table uses START:END bounds")
        retained=json.loads(args.retain_report.read_text()) if args.retain_report else None
        report = generate(args.exe.resolve(), args.kit.resolve(), args.output.resolve(), args.root, args.max_blocks,
                          tables=args.init_table,ilt=args.ilt_range,retained=retained)
        print(json.dumps({key: value for key, value in report.items() if key not in ("frontier", "translation_errors", "indirect_sites", "extra_roots", "translation_units", "block_addresses")}, indent=2))
        print(f"Pending direct targets: {len(report['frontier'])}; guarded translation errors: {len(report['translation_errors'])}")
    except (ValueError, OSError, subprocess.SubprocessError, pefile.PEFormatError) as error:
        print(f"Preparation failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
