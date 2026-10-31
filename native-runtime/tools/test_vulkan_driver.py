"""Install the diagnostic APK and probe stock/custom Vulkan in separate processes."""

import argparse
import hashlib
import json
from pathlib import Path
import stat
import subprocess
import tarfile
import tempfile
import time
import zipfile

from test_device import select_device

PACKAGE = "dev.chusan.runtimeprobe"


def extract_driver(archive, destination):
    """Accept driver packages, not flashable system modifications or unsafe archives."""
    total = 0
    count = 0

    def save(name, size, source):
        nonlocal total, count
        total += size
        count += 1
        target = (destination / name).resolve()
        if not target.is_relative_to(destination.resolve()) or total > 200 * 1024 * 1024 or count > 128:
            raise ValueError("Unsafe or oversized driver archive")
        target.parent.mkdir(parents=True, exist_ok=True)
        content = source.read(size + 1)
        if len(content) != size:
            raise ValueError("Driver archive entry has an invalid size")
        target.write_bytes(content)

    if archive.suffix.lower() == ".zip":
        with zipfile.ZipFile(archive) as package:
            for entry in package.infolist():
                if entry.is_dir():
                    continue
                if stat.S_ISLNK(entry.external_attr >> 16):
                    raise ValueError("Driver archive contains a symbolic link")
                with package.open(entry) as source:
                    save(entry.filename, entry.file_size, source)
    else:
        from compression import zstd  # Python 3.14+ for repository .tzst driver packages.
        with zstd.open(archive, "rb") as stream, tarfile.open(fileobj=stream, mode="r|") as package:
            for entry in package:
                if entry.isdir():
                    continue
                if not entry.isfile():
                    raise ValueError("Driver archive contains a non-regular entry")
                with package.extractfile(entry) as source:
                    save(entry.name, entry.size, source)
    metadata = json.loads((destination / "meta.json").read_text(encoding="utf-8"))
    library = metadata["libraryName"]
    if not isinstance(library, str) or "/" in library or "\\" in library or not library.endswith(".so"):
        raise ValueError("Driver library must be a .so basename in meta.json")
    header = (destination / library).read_bytes()[:20]
    if header[:6] != b"\x7fELF\x02\x01" or int.from_bytes(header[18:20], "little") != 183:
        raise ValueError("Custom driver is not an ARM64 ELF library")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--apk", type=Path, required=True)
    parser.add_argument("--driver", type=Path, required=True, help="Adrenotools .zip or repository .tzst package")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--serial")
    args = parser.parse_args()

    def adb(*parts, check=True, binary_input=None, timeout=30):
        result = subprocess.run(["adb", *parts], input=binary_input, capture_output=True,
                                timeout=timeout, check=check)
        return result.stdout.decode("utf-8", errors="replace")

    serial = select_device(adb("devices"), args.serial)

    def shell(*parts, **kwargs):
        return adb("-s", serial, "shell", *parts, **kwargs)

    def run(mode):
        shell("am", "force-stop", PACKAGE)
        shell("run-as", PACKAGE, "rm", "-f", "files/vulkan-probe.json")
        shell("am", "start", "-W", "-n", PACKAGE + "/.NativeProbeActivity", "--es", "driver", mode)
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            text = shell("run-as", PACKAGE, "cat", "files/vulkan-probe.json", check=False)
            if text.strip():
                result = json.loads(text)
                if result.get("error") or result.get("logical_device_created") is not True:
                    raise ValueError(f"{mode} driver probe failed: {result}")
                return result
            time.sleep(0.25)
        raise TimeoutError(f"{mode} driver probe did not publish a result")

    with tempfile.TemporaryDirectory(prefix="chusan-driver-") as temporary:
        extracted = Path(temporary) / "driver"
        extracted.mkdir()
        extract_driver(args.driver, extracted)
        archive = Path(temporary) / "driver.tar"
        with tarfile.open(archive, "w") as package:
            for path in extracted.rglob("*"):
                if path.is_file():
                    package.add(path, arcname=path.relative_to(extracted), recursive=False)
        adb("-s", serial, "install", "-r", "-t", str(args.apk.resolve()), timeout=120)
        shell("am", "start", "-W", "-n", PACKAGE + "/.NativeProbeActivity")
        system = run("system")
        shell("am", "force-stop", PACKAGE)
        shell("run-as", PACKAGE, "mkdir", "-p", "files/driver")
        adb("-s", serial, "exec-in", "run-as", PACKAGE, "tar", "-xf", "-", "-C", "files/driver",
            binary_input=archive.read_bytes())
        custom = run("custom")
        identity = ("driver_id", "driver_name", "driver_info", "driver_version")
        if all(system.get(key) == custom.get(key) for key in identity):
            raise ValueError("Custom request returned the same driver identity; do not assume the hook loaded")
        report = {"system": system, "custom": custom,
                  "added_extensions": sorted(set(custom["extensions"]) - set(system["extensions"])),
                  "driver_archive_sha256": hashlib.sha256(args.driver.read_bytes()).hexdigest(),
                  "draw_verified": False, "dxvk_compatibility_verified": False}
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(json.dumps({"system_driver": system["driver_name"], "system_api": system["api_version"],
                          "custom_driver": custom["driver_name"], "custom_api": custom["api_version"],
                          "added_extensions": report["added_extensions"], "logical_devices_created": True}, indent=2))


if __name__ == "__main__":
    main()
