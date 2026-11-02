"""Qualify the pinned recomp-kit emitter on one original startup initializer."""
import argparse
import hashlib
import importlib
import json
from pathlib import Path
import re
import subprocess
import sys
from types import SimpleNamespace

import capstone
from capstone.x86_const import X86_OP_IMM, X86_OP_MEM
import pefile

ROOT = Path(__file__).resolve().parents[1]
NEGATIVES = {
    "unknown-call": "unknown_call", "unknown-jump": "unknown_jump",
    "unknown-api": "unknown_api", "unknown-return": "unknown_return",
    "memory-read": "memory_read", "memory-write": "memory_write",
    "fixture-write": "fixture_write",
}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def validate_decode(code, address, iat):
    decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    decoder.detail = True
    instructions = list(decoder.disasm(code, address))
    if not instructions or sum(i.size for i in instructions) != len(code):
        raise ValueError("Incomplete instruction decode")
    boundaries = {i.address for i in instructions}
    calls = []
    admitted = {"push", "pop", "mov", "lea", "sub", "and", "cmp", "test", "je", "jne",
                "jmp", "not", "call", "xor", "or", "shl", "ret"}
    for insn in instructions:
        if insn.mnemonic not in admitted:
            raise ValueError("Instruction outside this bounded fixture")
        if insn.group(capstone.CS_GRP_JUMP):
            if insn.operands[0].type != X86_OP_IMM or insn.operands[0].imm not in boundaries:
                raise ValueError("Jump does not target an admitted instruction boundary")
        if insn.mnemonic == "call":
            op = insn.operands[0]
            if op.type != X86_OP_MEM or op.mem.base or op.mem.index or op.mem.segment or op.mem.disp not in iat:
                raise ValueError("Call outside the four admitted IAT slots")
            calls.append(op.mem.disp)
        if insn.mnemonic == "ret" and (insn.operands or insn != instructions[-1]):
            raise ValueError("Only a final plain RET is admitted")
    if calls != iat or instructions[-1].mnemonic != "ret":
        raise ValueError("Fixture must contain the ordered four API call sites and final RET")
    return instructions


def generate(executable, kit, manifest):
    if digest(executable) != manifest["source_sha256"]:
        raise ValueError("Executable hash differs from selected profile")
    commit = subprocess.check_output(["git", "-C", str(kit), "rev-parse", "HEAD"], text=True).strip()
    if commit != manifest["kit_commit"]:
        raise ValueError("recomp-kit commit differs from selected profile")
    # Check every tracked file: imported Python modules can depend on one another.
    subprocess.run(["git", "-C", str(kit), "diff", "--exit-code", "HEAD", "--"], check=True, capture_output=True)
    untracked = subprocess.check_output(["git", "-C", str(kit), "ls-files", "--others", "--exclude-standard",
                                         "--", "tools/recomp", "runtime"], text=True)
    if untracked.strip():
        raise ValueError("Untracked files in recomp-kit source directories")
    with pefile.PE(str(executable), fast_load=True) as pe:
        if pe.FILE_HEADER.Machine != 0x14c:
            raise ValueError("Fixture requires x86 input")
        address = int(manifest["va"], 16)
        code = pe.get_data(address - pe.OPTIONAL_HEADER.ImageBase, manifest["bytes"])
    if hashlib.sha256(code).hexdigest() != manifest["sha256"]:
        raise ValueError("Initializer hash differs from selected profile")
    decoded = validate_decode(code, address, [int(i, 16) for i in manifest["iat"]])
    sys.path.insert(0, str(kit / "tools/recomp"))
    translator = importlib.import_module("translate")
    image = translator.Image(str(executable))
    image.md.detail = True
    function = translator.Function(address, "startup_initializer", len(code), [image.to_insn(i) for i in decoded])
    function.measure(image)
    emitter = translator.Translator(image, {address}, SimpleNamespace(eager_flags=True, allow_unmodelled=None))
    emitter.prepare(function, strict=True)
    generated = "\n".join(emitter.translate(function, ())) + "\n"
    if re.search(r"recomp_(?:unmodelled|interp|unknown)|\b(?:abort|exit)\s*\(", generated):
        raise ValueError("Emitter produced a guard, interpreter or unknown-code path")
    accesses = set(re.findall(r"\b(?:rd|wr)\d+\b", generated))
    if accesses != {"rd32", "wr32"}:
        raise ValueError("Generated memory helpers exceed the checked fixture boundary")
    callouts = set(re.findall(r"\brecomp_\w+(?=\()", generated))
    if callouts != {"recomp_call", "recomp_return"}:
        raise ValueError("Generated callouts exceed the strict fixture boundary")
    return code, generated, len(decoded)


def compare(command):
    result = subprocess.run(command, check=True, capture_output=True, text=True, timeout=60)
    report = json.loads(result.stdout)
    if report.get("cases") != 4096 or report.get("mismatches") != 0:
        raise ValueError("Comparison did not pass all original reference vectors")
    return report


def negatives(command):
    reports = {}
    for mode, expected in NEGATIVES.items():
        result = subprocess.run([*command, "--negative=" + mode], capture_output=True, text=True, timeout=20)
        if result.returncode == 0 or json.loads(result.stderr).get("error") != expected:
            raise ValueError("Missing terminal rejection for " + mode)
        reports[mode] = {"exit_code": result.returncode, "error": expected}
    return reports


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--kit", type=Path, required=True)
    parser.add_argument("--clang", type=Path, required=True, help="llvm-mingw clang")
    parser.add_argument("--android-clang", type=Path, required=True, help="NDK aarch64-linux-android29-clang")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        if sys.platform != "win32":
            raise ValueError("Original x86 reference currently needs Windows")
        output = args.output.resolve()
        if output.is_relative_to(args.exe.resolve().parent) or not output.is_relative_to(ROOT / ".work"):
            raise ValueError("Generated output must stay under native-runtime/.work")
        manifest = json.loads((ROOT / "profiles/startup-initializer.json").read_text())
        code, generated, instruction_count = generate(args.exe.resolve(), args.kit.resolve(), manifest)
        output.mkdir(parents=True, exist_ok=True)
        (output / "initializer.bin").write_bytes(code)
        (output / "initializer.c").write_text(generated, encoding="utf-8")
        compiler = str(args.clang.resolve())
        common = ["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "probes")]
        oracle = output / "startup-original.exe"
        subprocess.run([compiler, "--target=i686-w64-windows-gnu", *common,
                        str(ROOT / "probes/startup_original.c"), str(ROOT / "probes/startup_original.S"),
                        "-Wl,--image-base,0x30000000,--disable-dynamicbase,--large-address-aware,--stack,65536",
                        "-o", str(oracle)], check=True)
        vectors = output / "reference-vectors.bin"
        reference = subprocess.run([str(oracle), str(output / "initializer.bin"), str(vectors)],
                                   check=True, text=True, capture_output=True, timeout=60)
        reference = json.loads(reference.stdout)
        includes = ["-I", str(args.kit.resolve() / "runtime"), "-I", str(output)]
        host = output / "startup-translated.exe"
        source = str(ROOT / "probes/startup_translated.c")
        subprocess.run([compiler, "--target=x86_64-w64-windows-gnu", *common, *includes, source, "-o", str(host)], check=True)
        host_result = compare([str(host), str(vectors)])
        rejected = negatives([str(host)])
        android = output / "startup-android"
        subprocess.run([str(args.android_clang.resolve()), *common, *includes, source, "-lm",
                        "-Wl,-z,max-page-size=16384", "-o", str(android)], check=True)
        report = {"manifest": manifest, "instruction_count": instruction_count, "eager_flags": True,
                  "reference": reference, "windows_x64_translation": host_result, "negative_checks": rejected,
                  "consumer_source_sha256": {p.name: digest(p) for p in [
                      ROOT / "probes/startup_fixture.h", ROOT / "probes/startup_original.c",
                      ROOT / "probes/startup_original.S", ROOT / "probes/startup_translated.c",
                      Path(__file__).resolve()]},
                  "emitted_c_sha256": digest(output / "initializer.c"),
                  "kit_instruction_header_sha256": digest(args.kit.resolve() / "runtime/x86.h"),
                  "linked_kit_runtime_sources": [],
                  "android_executable_sha256": digest(android), "reference_vectors_sha256": digest(vectors),
                  "android_execution_verified": False, "full_game_translation_verified": False}
        (output / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(json.dumps(report, indent=2))
    except (ValueError, OSError, KeyError, subprocess.SubprocessError, pefile.PEFormatError) as error:
        if isinstance(error, subprocess.CalledProcessError) and error.stderr:
            print(error.stderr, file=sys.stderr)
        print(f"Startup probe failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
