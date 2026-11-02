"""Record PE identities and declared dependencies without executing the game."""

import argparse
import hashlib
import json
from pathlib import Path
import sys

import pefile


def inspect_binary(path):
    data = path.read_bytes()
    try:
        pe = pefile.PE(data=data, fast_load=True)
        pe.parse_data_directories(directories=[1, 9, 13])
    except pefile.PEFormatError as error:
        raise ValueError(f"{path.name}: invalid PE image: {error}") from error
    with pe:
        imports = []
        for attribute, delayed in (("DIRECTORY_ENTRY_IMPORT", False),
                                   ("DIRECTORY_ENTRY_DELAY_IMPORT", True)):
            for entry in getattr(pe, attribute, []):
                imports.append({
                    "dll": entry.dll.decode("ascii", errors="replace"),
                    "delayed": delayed,
                    "symbols": [symbol.name.decode("ascii", errors="replace")
                                if symbol.name else f"#{symbol.ordinal}"
                                for symbol in entry.imports],
                })
        imports.sort(key=lambda entry: (entry["dll"].lower(), entry["delayed"]))
        bits = {0x10B: 32, 0x20B: 64}.get(pe.OPTIONAL_HEADER.Magic)
        if bits is None:
            raise ValueError(f"{path.name}: unsupported PE optional-header format")
        machine = pe.FILE_HEADER.Machine
        return {
            "file": path.name,
            "bytes": len(data),
            "sha256": hashlib.sha256(data).hexdigest(),
            "machine": f"0x{machine:04x}",
            "architecture": {0x14C: "x86", 0x8664: "x86_64", 0xAA64: "arm64"}.get(machine, "unknown"),
            "pe_bits": bits,
            "image_base": f"0x{pe.OPTIONAL_HEADER.ImageBase:x}",
            "entry_rva": f"0x{pe.OPTIONAL_HEADER.AddressOfEntryPoint:x}",
            "image_bytes": pe.OPTIONAL_HEADER.SizeOfImage,
            "tls_directory_present": hasattr(pe, "DIRECTORY_ENTRY_TLS"),
            "imports": imports,
            "import_count": sum(len(entry["symbols"]) for entry in imports),
            "dynamic_loading_declared": any(
                symbol == "GetProcAddress" or symbol.startswith("LoadLibrary")
                for entry in imports for symbol in entry["symbols"]),
            "sections": [{
                "name": section.Name.rstrip(b"\0").decode("ascii", errors="replace"),
                "rva": f"0x{section.VirtualAddress:x}",
                "virtual_bytes": section.Misc_VirtualSize,
                "raw_bytes": section.SizeOfRawData,
                "executable": bool(section.Characteristics & 0x20000000),
            } for section in pe.sections],
        }


def verify_profile(binaries, profile):
    """A profile is an identity check, never evidence of translated execution."""
    actual = {binary["file"].lower(): binary for binary in binaries}
    if not profile["binaries"]:
        raise ValueError("The selected profile has no binary identities")
    failures = []
    for expected in profile["binaries"]:
        name = expected["file"]
        if "/" in name or "\\" in name or name in ("", ".", ".."):
            raise ValueError("Profile filenames must be plain basenames")
        observed = actual.get(name.lower())
        if observed is None:
            failures.append(f"Missing input: {name}")
            continue
        for field in ("sha256", "architecture"):
            if observed[field] != expected[field]:
                failures.append(f"{name}: {field} does not match the selected profile")
    return failures


def inventory(directory, profile=None):
    if not directory.is_dir():
        raise ValueError("The input directory does not exist")
    paths = sorted((path for path in directory.iterdir()
                    if path.is_file() and path.suffix.lower() in (".exe", ".dll")),
                   key=lambda path: path.name.lower())
    if not paths:
        raise ValueError("No EXE or DLL inputs found")
    binaries = [inspect_binary(path) for path in paths]
    return {
        "schema": 1,
        "scope": "Top-level EXE/DLL files; static imports are a lower bound on runtime dependencies.",
        "binary_count": len(binaries),
        "binaries": binaries,
        "profile_failures": verify_profile(binaries, profile) if profile else [],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin-dir", type=Path, required=True)
    parser.add_argument("--profile", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        profile = json.loads(args.profile.read_text(encoding="utf-8")) if args.profile else None
        report = inventory(args.bin_dir, profile)
        if args.output.resolve().is_relative_to(args.bin_dir.resolve()):
            raise ValueError("Write the report outside the source installation")
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    except (ValueError, OSError, KeyError, TypeError) as error:
        print(f"Inventory failed: {error}", file=sys.stderr)
        return 1
    print(f"Inspected {report['binary_count']} PE files; report: {args.output}")
    for failure in report["profile_failures"]:
        print(failure, file=sys.stderr)
    return 2 if report["profile_failures"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
