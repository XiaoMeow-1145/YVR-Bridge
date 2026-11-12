#!/usr/bin/env python3
"""Install/rollback the reversible per-application OpenXR input adapter."""
from pathlib import Path
import hashlib
import json
import shlex
import subprocess
import argparse
import re

ROOT = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description="Deploy/status/rollback the YVR 2 adapter for an existing app installation.")
parser.add_argument("action", choices=("status", "deploy", "rollback"), nargs="?", default="status")
parser.add_argument("--serial", required=True, help="adb device serial; never inferred")
parser.add_argument("--package", default="VirtualDesktop.AndroidPlus")
parser.add_argument("--build-dir", type=Path, default=ROOT / "build")
parser.add_argument("--backup-dir", help="Existing original backup directory under /data/local/tmp, if migrating an older install")
args = parser.parse_args()
PACKAGE = args.package
if not re.fullmatch(r"[A-Za-z0-9_]+(?:\.[A-Za-z0-9_]+)+", PACKAGE):
    parser.error("Invalid Android package name")
BUILD = args.build_dir.resolve()
MANIFEST = json.loads((BUILD / "manifest.json").read_text())
BACKUP = args.backup_dir or "/data/local/tmp/vd_yvr2_backup_" + MANIFEST["original_sha256"][:12]
if not BACKUP.startswith("/data/local/tmp/") or ".." in BACKUP.split("/"):
    parser.error("Backup must be an absolute directory under /data/local/tmp without '..'")
STAGE = "/data/local/tmp/vd_yvr2_stage_" + MANIFEST["adapter_sha256"][:12]

args_serial = args.serial

def adb(*args, binary=False):
    try:
        return subprocess.check_output(["adb", "-s", args_serial, *args], text=not binary)
    except subprocess.CalledProcessError as error:
        raise SystemExit("adb command failed; verify --serial, connection and device authorization before retrying") from error

def require(condition, message):
    if not condition:
        raise SystemExit(message)

def root(command, binary=False):
    return adb("exec-out" if binary else "shell", "su -c " + shlex.quote(command), binary=binary)

def sha(data):
    return hashlib.sha256(data).hexdigest()

def contents(path):
    # Ordinary shell reads avoid Magisk authorization dialogs stealing VR focus.
    return adb("exec-out", "cat " + shlex.quote(path), binary=True)

paths = [line.removeprefix("package:") for line in adb("shell", "pm path " + shlex.quote(PACKAGE)).splitlines() if line.startswith("package:")]
base_paths = [path for path in paths if path.endswith("/base.apk")]
if len(base_paths) != 1 or not base_paths[0].startswith("/data/app/"):
    raise SystemExit("Expected one installed /data/app/.../base.apk; inspect this installation before using this script")
LIB = str(Path(base_paths[0]).parent / "lib/arm64")
action = args.action
if action == "deploy":
    for name, key in (("libopenxr_loader.so", "adapter_sha256"), ("libvd_original.so", "renamed_original_sha256")):
        if sha((BUILD / name).read_bytes()) != MANIFEST[key]:
            raise SystemExit("Local build hash mismatch: " + name)
current_path = LIB + "/libopenxr_loader.so"
current = contents(current_path)
current_hash = sha(current)
print("Current loader:", current_hash)
print("Installed native library directory:", LIB)
if action == "deploy":
    require(current_hash in [MANIFEST["original_sha256"], MANIFEST["adapter_sha256"], *MANIFEST.get("adapter_history_sha256", [])], "Unexpected existing loader; refusing to overwrite")
    adb("shell", "am force-stop " + PACKAGE)
    root("mkdir -p " + shlex.quote(BACKUP) + " " + shlex.quote(STAGE) + " && chown shell:shell " + shlex.quote(STAGE) + " && chmod 755 " + shlex.quote(STAGE))
    exists = root("if [ -f " + shlex.quote(BACKUP + "/libopenxr_loader.so") + " ]; then echo yes; else echo no; fi").strip()
    if exists == "yes":
        require(sha(contents(BACKUP + "/libopenxr_loader.so")) == MANIFEST["original_sha256"], "Unexpected backup")
    else:
        require(current_hash == MANIFEST["original_sha256"], "Cannot back up proxy as original")
        root("cp -p " + shlex.quote(current_path) + " " + shlex.quote(BACKUP + "/libopenxr_loader.so"))
        require(sha(contents(BACKUP + "/libopenxr_loader.so")) == MANIFEST["original_sha256"], "Backup hash mismatch")
    (BUILD / "original-device-backup.so").write_bytes(contents(BACKUP + "/libopenxr_loader.so"))
    for name in ("libvd_original.so", "libopenxr_loader.so"):
        adb("push", str(BUILD / name), STAGE + "/" + name)
        target = LIB + "/" + name
        root("cp " + shlex.quote(STAGE + "/" + name) + " " + shlex.quote(target) + " && chown system:system " + shlex.quote(target) + " && chmod 755 " + shlex.quote(target) + " && chcon u:object_r:apk_data_file:s0 " + shlex.quote(target))
        expected = MANIFEST["renamed_original_sha256" if name == "libvd_original.so" else "adapter_sha256"]
        require(sha(contents(target)) == expected, "Deployed hash mismatch: " + name)
    print("Deployed with verified hashes; original backup:", BACKUP)
elif action == "rollback":
    require(sha(contents(BACKUP + "/libopenxr_loader.so")) == MANIFEST["original_sha256"], "Backup hash mismatch")
    require(current_hash in [MANIFEST["original_sha256"], MANIFEST["adapter_sha256"], *MANIFEST.get("adapter_history_sha256", [])], "Unexpected loader; inspect before rollback")
    adb("shell", "am force-stop " + PACKAGE)
    root("cp " + shlex.quote(BACKUP + "/libopenxr_loader.so") + " " + shlex.quote(current_path) + " && chown system:system " + shlex.quote(current_path) + " && chmod 755 " + shlex.quote(current_path) + " && chcon u:object_r:apk_data_file:s0 " + shlex.quote(current_path))
    require(sha(contents(current_path)) == MANIFEST["original_sha256"], "Restored loader hash mismatch")
    print("Restored original loader. The unused renamed original can remain safely.")
elif action != "status":
    raise SystemExit("Use deploy, rollback, or status")
print(adb("shell", "ls -lZ " + shlex.quote(current_path)))
