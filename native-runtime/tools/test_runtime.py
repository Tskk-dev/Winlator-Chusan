"""End-to-end synthetic x86 tests for the compiled block runtime; no game input."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import uuid

from prepare_runtime import generate, ROOT
from run_runtime import build, freeze_source, run_stop
from test_device import select_device

sys.path.insert(0, str(ROOT / "tests"))
from test_inventory import fixture


def scenarios():
    loop = bytearray.fromhex("b800000000b905000000e8110000004975f8c3")
    loop.extend(bytes(0x20-len(loop)))
    loop.extend(bytes.fromhex("01c8c3"))
    yield "loop-call", {0: loop}, [], False, None, "entry_returned", "0x0000000f"
    yield "budget", {0: bytes.fromhex("ebfe")}, [], False, None, "block_limit", "0x00401000"
    yield "header-guard", {0: bytes.fromhex("bf00000000b82a000000aac3")}, [], False, None, "write_bounds", "0x00000000"
    yield "readonly", {0: bytes.fromhex("c705c01140002a000000c3")}, [], False, None, "write_permission", "0x004011c0"
    fp_control = bytes.fromhex("680000030068000001006a00ff155811400083c40cd93dc01140000fb705c0114000c3")
    yield "fp-control-53", {0: fp_control}, [], True, "_controlfp_s", "entry_returned", "0x0000027f"
    simd_copy = bytes.fromhex("f30f6f05c0114000f30f7f05d0114000a1dc114000c3")
    yield "simd-copy", {0: simd_copy, 0x1c0: struct.pack("<IIII", 1, 2, 3, 0x10203040)}, [], True, None, "entry_returned", "0x10203040"
    half_store = bytes.fromhex("f30f6f05c01140000f1305d01140000f1705d8114000a1dc114000c3")
    yield "simd-half-store", {0: half_store, 0x1c0: struct.pack("<IIII", 1, 2, 3, 0x10203040)}, [], True, None, "entry_returned", "0x10203040"
    critical = bytes.fromhex("6a0068a00f00006800134000ff155811400083c40ca104134000c3")
    yield "critical-init", {0: critical}, [], True, "__vcrt_InitializeCriticalSectionEx", "entry_returned", "0xffffffff"
    module = bytes.fromhex("6800134000ff155811400085c00f95c00fb6c0c3")
    yield "kernel-module", {0: module, 0x300: "kernel32.dll\0".encode("utf-16-le")}, [], False, "GetModuleHandleW", "entry_returned", "0x00000001"
    optional = bytes.fromhex("6800134000680000d80fff155811400064a134000000c3")
    yield "optional-export", {0: optional, 0x300: b"InitializeConditionVariable\0"}, [], False, "GetProcAddress", "entry_returned", "0x0000007f"
    for checked in (True, False):
        code = bytes.fromhex("688c1140006880114000ff155811400083c408")
        if checked:
            code += bytes.fromhex("83f8077506")
        code += bytes.fromhex("a1c0114000c3b8ad0b0000c3")
        body0 = bytes.fromhex("ff05c011400031c0c3")
        body1 = bytes.fromhex("ff05c0114000b807000000c3")
        body2 = bytes.fromhex("cc") if checked else bytes.fromhex("ff05c0114000b80b000000c3")
        patches = {0: code, 0x40: body0, 0x60: body1, 0x80: body2,
                   0x180: struct.pack("<III", 0x401040, 0x401060, 0x401080), 0x1c0: bytes(4)}
        yield ("initterm-e" if checked else "initterm"), patches, [0x401040, 0x401060, 0x401080], True, (
            "_initterm_e" if checked else "_initterm"), "entry_returned", ("0x00000002" if checked else "0x00000003")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kit", type=Path, required=True)
    parser.add_argument("--clang", type=Path, required=True)
    parser.add_argument("--android-clang", type=Path)
    parser.add_argument("--adb")
    parser.add_argument("--serial")
    args = parser.parse_args()
    if args.adb and not args.android_clang:
        parser.error("--adb requires --android-clang")
    kit = args.kit.resolve()
    profile = json.loads((ROOT / "profiles/startup-initializer.json").read_text())
    results = {}
    result_path = ROOT / ".work/runtime-tests/results.json"
    result_path.parent.mkdir(parents=True, exist_ok=True)
    result_path.write_text("{}\n")
    serial = None
    if args.adb:
        serial = select_device(subprocess.check_output([args.adb, "devices"], text=True), args.serial)
    for name, patches, roots, writable, imported, expected_stop, expected_target in scenarios():
        output = ROOT / ".work/runtime-tests" / name
        output.mkdir(parents=True, exist_ok=True)
        data = bytearray(fixture())
        for offset, patch in patches.items():
            data[0x200+offset:0x200+offset+len(patch)] = patch
        if writable:
            struct.pack_into("<I", data, 0x178+36, 0xe0000020)
        if imported:
            data[0x392:0x3d2] = imported.encode().ljust(64, b"\0")
        source = output / "synthetic.exe"
        source.write_bytes(data)
        profile["source_sha256"] = hashlib.sha256(data).hexdigest()
        prepared = generate(source, kit, output, roots, 64, profile)
        if prepared["translation_errors"] or prepared["frontier"]:
            raise ValueError(f"Synthetic closure incomplete: {name}")
        freeze_source(output)
        target = ["--target=x86_64-w64-windows-gnu"] if os.name == "nt" else []
        executable = build(args.clang.resolve(), target, kit, output)
        observed = run_stop([str(executable), str(output / "image.bin")], output / "host.log")
        assert (observed["stop"], observed["target"]) == (expected_stop, expected_target), (name, observed)
        results[name] = {"host": observed}
        if serial:
            android = build(args.android_clang.resolve(), [], kit, output, android=True)
            remote = "/data/local/tmp/chusan-synthetic-" + uuid.uuid4().hex[:12]
            command = [args.adb, "-s", serial]
            def adb(*parts, check=True):
                return subprocess.run([*command, *parts], check=check, capture_output=True, text=True, timeout=30)
            try:
                adb("shell", "mkdir", remote)
                adb("push", str(android), remote + "/runtime")
                adb("push", str(output / "image.bin"), remote + "/image.bin")
                adb("shell", "chmod", "700", remote + "/runtime")
                observed = run_stop([*command, "shell", remote + "/runtime", remote + "/image.bin"], output / "android.log")
                assert (observed["stop"], observed["target"]) == (expected_stop, expected_target), (name, observed)
                results[name]["android"] = observed
            finally:
                adb("shell", "rm", "-f", remote + "/runtime", remote + "/image.bin", check=False)
                adb("shell", "rmdir", remote, check=False)
        result_path.write_text(json.dumps(results, indent=2) + "\n")
    print(json.dumps({"scenarios_passed": len(results), "platforms": ["host", "android"] if serial else ["host"]}))


if __name__ == "__main__":
    main()
