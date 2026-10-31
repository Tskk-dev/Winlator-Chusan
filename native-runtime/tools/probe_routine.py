"""Bounded translation experiment, not a general x86 translator or game launcher."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import struct
import sys

import capstone
from capstone.x86_const import X86_OP_IMM, X86_OP_MEM, X86_OP_REG, X86_REG_EAX, X86_REG_ECX
import pefile


def emit_probe(code, address):
    decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    decoder.detail = True
    instructions = list(decoder.disasm(code, address))
    if len(instructions) != 4 or sum(i.size for i in instructions) != len(code):
        raise ValueError("Probe requires exactly two MOV loads, ADD EAX,imm32 and plain RET")
    offsets = []
    for insn, base in zip(instructions[:2], (X86_REG_ECX, X86_REG_EAX)):
        operands = insn.operands
        if (insn.mnemonic != "mov" or len(operands) != 2
                or operands[0].type != X86_OP_REG or operands[0].reg != X86_REG_EAX
                or operands[0].size != 4 or operands[1].type != X86_OP_MEM
                or operands[1].size != 4 or operands[1].mem.base != base
                or operands[1].mem.index or operands[1].mem.segment):
            raise ValueError("Unsupported MOV operand or address form")
        offsets.append(operands[1].mem.disp & 0xFFFFFFFF)
    add, ret = instructions[2:]
    if (add.mnemonic != "add" or len(add.operands) != 2
            or add.operands[0].type != X86_OP_REG or add.operands[0].reg != X86_REG_EAX
            or add.operands[0].size != 4 or add.operands[1].type != X86_OP_IMM
            or ret.mnemonic != "ret" or ret.operands):
        raise ValueError("Unsupported arithmetic or return form")
    immediate = add.operands[1].imm & 0xFFFFFFFF
    # No libc, native pointer embedded in a guest address, or executable game bytes.
    return f"""typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long long u64;
_Static_assert(sizeof(u32) == 4, "32-bit guest words required");
static int load32(const u8 *memory, u32 base, u32 size, u32 address, u32 *out) {{
    if (address < base || (u64)(address - base) + 4 > size) return 0;
    const u8 *p = memory + (address - base);
    *out = (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
    return 1;
}}
int chusan_probe(const u8 *memory, u32 base, u32 size, u32 ecx, u32 *eax, u32 *flags) {{
    u32 lhs;
    if (!load32(memory, base, size, ecx + {offsets[0]}u, &lhs)) return 0;
    if (!load32(memory, base, size, lhs + {offsets[1]}u, &lhs)) return 0;
    const u32 rhs = {immediate}u, result = lhs + rhs;
    u32 parity = (result ^ (result >> 4)) & 15u;
    *eax = result;
    *flags = (result < lhs) | (((0x9669u >> parity) & 1u) << 2)
        | (((lhs ^ rhs ^ result) & 16u)) | ((result == 0) << 6)
        | ((result >> 31) << 7) | (((~(lhs ^ rhs) & (lhs ^ result)) >> 31) << 11);
    return 1;
}}
"""


def source_routine(executable, profile):
    expected = next((item for item in profile["binaries"] if item["file"] == "chusanApp.exe"), None)
    if expected is None:
        raise ValueError("Profile does not name chusanApp.exe")
    data = executable.read_bytes()
    if hashlib.sha256(data).hexdigest() != expected["sha256"]:
        raise ValueError("Executable hash differs from selected profile")
    probe = profile["routine_probe"]
    address = int(probe["va"], 16)
    with pefile.PE(data=data, fast_load=True) as pe:
        if pe.FILE_HEADER.Machine != 0x14C:
            raise ValueError("This probe requires the 32-bit x86 game")
        code = pe.get_data(address - pe.OPTIONAL_HEADER.ImageBase, probe["bytes"])
    if hashlib.sha256(code).hexdigest() != probe["sha256"]:
        raise ValueError("Routine bytes differ from selected profile")
    return code, address


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--profile", type=Path, required=True)
    parser.add_argument("--clang", type=Path, required=True,
                        help="llvm-mingw clang with its i686 Windows runtime")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if sys.platform != "win32":
        parser.error("The original-byte reference runner currently requires 32-bit Windows execution")
    try:
        profile = json.loads(args.profile.read_text(encoding="utf-8"))
        code, address = source_routine(args.exe, profile)
        generated = emit_probe(code, address)
        output = args.output.resolve()
        if output.is_relative_to(args.exe.parent.resolve()):
            raise ValueError("Keep generated files outside the game installation")
        output.mkdir(parents=True, exist_ok=True)
        source = output / "routine.c"
        source.write_text(generated, encoding="utf-8")
        (output / "routine.bin").write_bytes(code)
        compiler = str(args.clang.resolve())
        compiled = output / "routine-x86.o"
        subprocess.run([compiler, "--target=i686-w64-windows-gnu", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-c", str(source), "-o", str(compiled)], check=True)
        runner = output / "reference.exe"
        host_source = Path(__file__).resolve().parents[1] / "probes/reference_x86.cpp"
        subprocess.run([compiler, "--target=i686-w64-windows-gnu", "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror", "-static",
                        str(host_source), str(compiled), "-lstdc++", "-o", str(runner)], check=True)
        vectors_path = output / "reference-vectors.bin"
        result = subprocess.run([str(runner), str(output / "routine.bin"), str(vectors_path)], check=True,
                                capture_output=True, text=True, timeout=30)
        comparison = json.loads(result.stdout)
        vectors = vectors_path.read_bytes()
        if len(vectors) != 4096 * 12:
            raise ValueError("Original-byte reference did not produce all 4096 vector records")
        header = "static const unsigned int probe_vectors[][3] = {\n"
        header += "".join("{%du,%du,%du},\n" % record for record in struct.iter_unpack("<III", vectors))
        (output / "reference_vectors.h").write_text(header + "};\n", encoding="ascii")
        arm64 = output / "routine-android-arm64.o"
        subprocess.run([compiler, "--no-default-config", "--target=aarch64-linux-android29", "-ffreestanding", "-nostdinc",
                        "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-c", str(source), "-o", str(arm64)], check=True)
        elf = arm64.read_bytes()
        if elf[:6] != b"\x7fELF\x02\x01" or int.from_bytes(elf[18:20], "little") != 183:
            raise ValueError("Cross-compiler did not produce an AArch64 ELF object")
        android = output / "chusan-android-probe"
        android_source = Path(__file__).resolve().parents[1] / "probes/android_arm64.c"
        subprocess.run([compiler, "--no-default-config", "--target=aarch64-linux-android29", "-ffreestanding",
                        "-nostdinc", "-nostdlib", "-static", "-fno-stack-protector", "-fuse-ld=lld",
                        "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-Wl,--entry=_start",
                        "-Wl,-z,max-page-size=16384", "-I", str(output), str(android_source), str(arm64),
                        "-o", str(android)], check=True)
        report = {"profile": profile["id"], "source_sha256": next(item["sha256"] for item in profile["binaries"]
                                                               if item["file"] == "chusanApp.exe"),
                  "routine_va": hex(address), "routine_bytes": len(code), "comparison": comparison,
                  "android_arm64_object_compiled": True, "android_execution_verified": False,
                  "android_executable_sha256": hashlib.sha256(android.read_bytes()).hexdigest(),
                  "reference_vectors_sha256": hashlib.sha256(vectors).hexdigest(),
                  "full_game_translation_verified": False}
        (output / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(json.dumps(report, indent=2))
    except (ValueError, OSError, KeyError, TypeError, subprocess.SubprocessError, pefile.PEFormatError) as error:
        print(f"Probe failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
