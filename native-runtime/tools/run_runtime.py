"""Build and run the bounded native startup runtime, recording its first stop."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import uuid

from test_device import select_device

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def freeze_source(output):
    # Both platform builds must consume the same bytes if work continues during compilation.
    (output / "runtime-source.c").write_bytes((ROOT / "runtime/runtime.c").read_bytes())


def run_stop(command, log, timeout=60, env=None):
    result = subprocess.run(command, capture_output=True, text=True, timeout=timeout,env=env)
    log.write_text(result.stderr, encoding="utf-8")
    if result.returncode != 20:
        raise ValueError(f"Runtime exited {result.returncode}: {result.stderr[-1500:]}")
    report = json.loads(result.stdout)
    if "stop" not in report or report.get("full_boot_verified") is not False:
        raise ValueError("Runtime did not publish an explicit stop")
    return report


def build(compiler, target, kit, output, android=False):
    flags = ["-std=c11", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
             "-I", str(ROOT / "runtime"), "-I", str(output), "-I", str(kit / "runtime")]
    prefix = "android" if android else "windows"
    command = [str(compiler), *target, *flags]
    preparation=json.loads((output/"preparation.json").read_text())
    units=preparation.get("translation_units",{"blocks.c":sha(output/"blocks.c")})
    objects=[]
    for filename,expected in sorted(units.items()):
        if not re.fullmatch(r"blocks(?:_[0-9a-f]{4})?\.c",filename) or sha(output/filename)!=expected:
            raise ValueError("Invalid or changed prepared translation unit")
        stamp = output / (prefix + "-" + Path(filename).stem + ".sha256")
        identity = hashlib.sha256((expected + sha(ROOT / "runtime/runtime.h")
                                   + sha(output / "x86.h") + str(command)).encode()).hexdigest()
        object_file = output / (prefix + "-" + Path(filename).stem + ".o")
        if not object_file.exists() or not stamp.exists() or stamp.read_text() != identity:
            subprocess.run([*command, "-c", str(output / filename), "-o", str(object_file)], check=True)
            stamp.write_text(identity)
        objects.append(str(object_file))
    executable = output / ("runtime-android" if android else "runtime.exe")
    arguments=[*target,*flags,str(output/"runtime-source.c"),*objects,"-lm",
               *(["-Wl,-z,max-page-size=16384"] if android else []),"-o",str(executable)]
    response=output/(prefix+"-link.rsp")
    # Both clang and the llvm-mingw wrapper read this file; forward slashes survive either parser.
    response.write_text("\n".join(json.dumps(argument.replace("\\","/") if os.name=="nt" else argument,
                                           ensure_ascii=False) for argument in arguments),encoding="utf-8")
    subprocess.run([str(compiler),"--rsp-quoting=posix","@"+str(response)],check=True)
    return executable


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prepare-dir", type=Path, required=True)
    parser.add_argument("--kit", type=Path, required=True)
    parser.add_argument("--clang", type=Path, required=True)
    parser.add_argument("--android-clang", type=Path)
    parser.add_argument("--host-only", action="store_true")
    parser.add_argument("--adb")
    parser.add_argument("--serial")
    args = parser.parse_args()
    if args.host_only and args.adb:
        parser.error("--host-only cannot be combined with --adb")
    if not args.host_only and not args.android_clang:
        parser.error("Provide --android-clang or use --host-only")
    remote = serial = None

    def adb(*parts, check=True):
        return subprocess.run([args.adb, *parts], check=check, capture_output=True, text=True, timeout=60)

    try:
        output, kit = args.prepare_dir.resolve(), args.kit.resolve()
        if not output.is_relative_to(ROOT / ".work"):
            raise ValueError("Prepared runtime must be under native-runtime/.work")
        (output / "run-report.json").unlink(missing_ok=True)
        preparation = json.loads((output / "preparation.json").read_text())
        if sha(output / "image.bin") != preparation["image_sha256"]:
            raise ValueError("Image differs from preparation report")
        freeze_source(output)
        windows = build(args.clang.resolve(), ["--target=x86_64-w64-windows-gnu"], kit, output)
        report = {"source_sha256": preparation["source_sha256"], "kit_commit": preparation["kit_commit"],
                  "prepared_blocks": preparation["blocks"], "prepared_instructions": preparation["instructions"],
                  "windows": run_stop([str(windows), str(output / "image.bin")], output / "windows-stderr.log"),
                  "image_sha256": preparation["image_sha256"], "full_boot_verified": False}
        if not args.host_only:
            android = build(args.android_clang.resolve(), [], kit, output, android=True)
            report["android_executable_sha256"] = sha(android)
        report["blocks_c_sha256"] = sha(output / "blocks.c")
        report["translation_units"]=preparation.get("translation_units",{})
        report["runtime_source_sha256"] = sha(output / "runtime-source.c")
        if args.adb:
            serial = select_device(adb("devices").stdout, args.serial)
            remote = "/data/local/tmp/chusan-runtime-" + uuid.uuid4().hex[:12]
            adb("-s", serial, "shell", "mkdir", remote)
            adb("-s", serial, "push", str(android), remote + "/runtime")
            adb("-s", serial, "push", str(output / "image.bin"), remote + "/image.bin")
            adb("-s", serial, "shell", "chmod", "700", remote + "/runtime")
            for name, expected in (("runtime", sha(android)), ("image.bin", preparation["image_sha256"])):
                if adb("-s", serial, "shell", "sha256sum", remote + "/" + name).stdout.split()[0] != expected:
                    raise ValueError("Device artifact hash mismatch")
            report["android"] = run_stop([args.adb, "-s", serial, "shell", remote + "/runtime", remote + "/image.bin"],
                                          output / "android-stderr.log")
            report["device"] = {key: adb("-s", serial, "shell", "getprop", prop).stdout.strip()
                                for key, prop in {"model": "ro.product.model", "soc": "ro.soc.model",
                                                  "sdk": "ro.build.version.sdk"}.items()}
        (output / "run-report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(json.dumps({key:value for key,value in report.items() if key!="translation_units"}, indent=2))
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        print(f"Runtime bring-up failed: {error}", file=sys.stderr)
        return 1
    finally:
        if serial and remote:
            try:
                adb("-s", serial, "shell", "rm", "-f", remote + "/runtime", remote + "/image.bin", check=False)
                adb("-s", serial, "shell", "rmdir", remote, check=False)
            except (OSError, subprocess.SubprocessError):
                pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
