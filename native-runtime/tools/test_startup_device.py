"""Compare the bounded recomp-kit translation with original-x86 vectors on Android."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys
import uuid

from probe_startup import digest, NEGATIVES
from test_device import select_device


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe-dir", type=Path, required=True)
    parser.add_argument("--adb", default=shutil.which("adb") or "adb")
    parser.add_argument("--serial")
    args = parser.parse_args()
    remote = serial = None

    def command(*parts, timeout=30, check=True):
        return subprocess.run([args.adb, *parts], capture_output=True, text=True, timeout=timeout, check=check)

    try:
        root = args.probe_dir.resolve()
        report = json.loads((root / "report.json").read_text())
        executable, vectors = root / "startup-android", root / "reference-vectors.bin"
        for path, key in ((executable, "android_executable_sha256"), (vectors, "reference_vectors_sha256")):
            if digest(path) != report[key]:
                raise ValueError("Fixture identity differs from host report: " + path.name)
        serial = select_device(command("devices").stdout, args.serial)

        def shell(*parts, **kwargs):
            return command("-s", serial, "shell", *parts, **kwargs)

        def prop(name):
            return shell("getprop", name).stdout.strip()

        device = {key: prop(name) for key, name in {
            "model": "ro.product.model", "abis": "ro.product.cpu.abilist", "soc": "ro.soc.model",
            "sdk": "ro.build.version.sdk", "android_release": "ro.build.version.release"}.items()}
        if "arm64-v8a" not in device["abis"].split(","):
            raise ValueError("An ARM64 Android device is required")
        remote = "/data/local/tmp/chusan-startup-" + uuid.uuid4().hex[:12]
        shell("mkdir", remote)
        command("-s", serial, "push", str(executable), remote + "/probe")
        command("-s", serial, "push", str(vectors), remote + "/vectors")
        shell("chmod", "700", remote + "/probe")
        result = json.loads(shell(remote + "/probe", remote + "/vectors", timeout=60).stdout)
        if result.get("cases") != 4096 or result.get("mismatches") != 0 or result.get("interpreter_linked") is not False:
            raise ValueError("Device comparison did not pass")
        rejected = {}
        for mode, expected in NEGATIVES.items():
            failure = shell(remote + "/probe", "--negative=" + mode, check=False)
            if not failure.returncode or json.loads(failure.stderr).get("error") != expected:
                raise ValueError("Missing device rejection for " + mode)
            rejected[mode] = {"exit_code": failure.returncode, "error": expected}
        evidence = {"source_sha256": report["manifest"]["source_sha256"],
                    "kit_commit": report["manifest"]["kit_commit"], "routine_va": report["manifest"]["va"],
                    "instruction_count": report["instruction_count"], "device": device, "comparison": result,
                    "negative_checks": rejected, "android_executable_sha256": digest(executable),
                    "reference_vectors_sha256": digest(vectors), "android_execution_verified": True,
                    "full_game_translation_verified": False}
        (root / "device-report.json").write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
        print(json.dumps(evidence, indent=2))
    except (ValueError, OSError, KeyError, subprocess.SubprocessError) as error:
        if isinstance(error, subprocess.CalledProcessError) and error.stderr:
            print(error.stderr, file=sys.stderr)
        print(f"Startup device probe failed: {error}", file=sys.stderr)
        return 1
    finally:
        if remote and serial:
            try:
                command("-s", serial, "shell", "rm", "-f", remote + "/probe", remote + "/vectors", check=False)
                command("-s", serial, "shell", "rmdir", remote, check=False)
            except (OSError, subprocess.SubprocessError):
                pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
