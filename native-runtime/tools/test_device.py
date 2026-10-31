"""Run the isolated ARM64 proof over ADB and keep the device evidence locally."""

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import uuid


def select_device(output, requested=None):
    devices = [line.split()[:2] for line in output.splitlines()[1:] if line.strip()]
    ready = [serial for serial, state in devices if state == "device"]
    if requested:
        if requested not in ready:
            raise ValueError("The selected ADB device is not connected and authorized")
        return requested
    if len(ready) != 1:
        raise ValueError("Connect and authorize one device, or select it with --serial")
    return ready[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe-dir", type=Path, required=True)
    parser.add_argument("--adb", default=shutil.which("adb") or "adb")
    parser.add_argument("--serial")
    args = parser.parse_args()

    def command(*parts, timeout=20, check=True):
        return subprocess.run([args.adb, *parts], text=True, capture_output=True,
                              timeout=timeout, check=check)

    remote = None
    serial = None
    try:
        root = args.probe_dir.resolve()
        report = json.loads((root / "report.json").read_text(encoding="utf-8"))
        executable = root / "chusan-android-probe"
        if hashlib.sha256(executable.read_bytes()).hexdigest() != report["android_executable_sha256"]:
            raise ValueError("Android probe identity differs from its host report")
        serial = select_device(command("devices").stdout, args.serial)

        def shell(*parts, **kwargs):
            return command("-s", serial, "shell", *parts, **kwargs)

        def prop(name):
            return shell("getprop", name).stdout.strip()

        abis = prop("ro.product.cpu.abilist")
        if "arm64-v8a" not in abis.split(","):
            raise ValueError("This probe needs an arm64-v8a Android device")
        device = {"model": prop("ro.product.model"), "manufacturer": prop("ro.product.manufacturer"),
                  "android_release": prop("ro.build.version.release"), "sdk": prop("ro.build.version.sdk"),
                  "abis": abis, "soc": prop("ro.soc.model"), "board": prop("ro.board.platform")}
        remote = "/data/local/tmp/chusan-probe-" + uuid.uuid4().hex[:12]
        shell("mkdir", remote)
        command("-s", serial, "push", str(executable), remote + "/probe")
        shell("chmod", "700", remote + "/probe")
        result = json.loads(shell(remote + "/probe", timeout=30).stdout)
        if result.get("passed") is not True or result.get("cases") != 4096 or result.get("mismatches") != 0:
            raise ValueError("Android routine comparison did not pass")
        evidence = {"profile": report["profile"], "source_sha256": report["source_sha256"],
                    "routine_va": report["routine_va"], "device": device, "result": result,
                    "android_execution_verified": True, "full_game_translation_verified": False,
                    "android_executable_sha256": report["android_executable_sha256"],
                    "reference_vectors_sha256": report["reference_vectors_sha256"]}
        (root / "device-report.json").write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
        print(json.dumps(evidence, indent=2))
    except (ValueError, OSError, KeyError, subprocess.SubprocessError) as error:
        print(f"Device probe failed: {error}", file=sys.stderr)
        return 1
    finally:
        if remote and serial:
            try:
                command("-s", serial, "shell", "rm", "-f", remote + "/probe", check=False)
                command("-s", serial, "shell", "rmdir", remote, check=False)
            except (OSError, subprocess.SubprocessError):
                pass  # Preserve the test result if the cable disconnects during cleanup.
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
