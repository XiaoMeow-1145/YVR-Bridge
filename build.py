#!/usr/bin/env python3
"""Build a per-app arm64 OpenXR binding adapter, preserving the original dispatcher."""
from pathlib import Path
import hashlib
import json
import re
import subprocess
import argparse

parser = argparse.ArgumentParser(description="Build the tested YVR 2 per-app OpenXR adapter (Linux x86_64).")
parser.add_argument("--original", type=Path, required=True, help="Your own original arm64 libopenxr_loader.so")
parser.add_argument("--ndk", type=Path, required=True, help="Android NDK root, tested with 27.2.12479018")
parser.add_argument("--openxr-include", type=Path, required=True, help="Directory containing openxr/openxr.h, tested with 1.0.34")
args = parser.parse_args()

ROOT = Path(__file__).resolve().parent
OUT = ROOT / "build"
OUT.mkdir(exist_ok=True)
ORIGINAL = args.original.resolve()
TOOLCHAIN = args.ndk.resolve() / "toolchains/llvm/prebuilt/linux-x86_64/bin"
CLANG = TOOLCHAIN / "clang"
READELF = TOOLCHAIN / "llvm-readelf"
HEADERS = args.openxr_include.resolve()
for required in (ORIGINAL, CLANG, READELF, HEADERS / "openxr/openxr.h", HEADERS / "openxr/openxr_platform.h"):
    if not required.is_file():
        parser.error(f"Required file not found: {required}")
EXPECTED_ORIGINAL = "da9b5977f24241eb6fb790b914bd0d8d70839f59fa826d7f1ea55379bcd65d09"
if hashlib.sha256(ORIGINAL.read_bytes()).hexdigest() != EXPECTED_ORIGINAL:
    parser.error("Untested original loader SHA-256; refusing to build for a different binary. See README compatibility scope.")

def exports(path):
    text = subprocess.check_output([str(READELF), "--dyn-syms", "--wide", str(path)], text=True)
    return sorted({m.group(1).split("@")[0] for line in text.splitlines() if " UND " not in line for m in [re.search(r"\b(xr\w+)(?:@\S+)?$", line)] if m})

names = exports(ORIGINAL)
assert len(names) == 56, f"Unexpected original export set: {names}"
hooks = {"xrGetInstanceProcAddr", "xrSuggestInteractionProfileBindings", "xrCreateAction", "xrGetActionStatePose", "xrGetActionStateFloat", "xrLocateSpace", "xrCreateActionSpace", "xrCreateReferenceSpace", "xrGetSystemProperties", "xrEnumerateInstanceExtensionProperties", "xrLocateViews", "xrEndFrame", "xrCreateSwapchain", "xrEnumerateSwapchainImages", "xrAcquireSwapchainImage", "xrWaitSwapchainImage", "xrReleaseSwapchainImage", "xrDestroySwapchain"}
assert hooks.issubset(names)
header = ["// Generated from the actual original loader export table.", f"#define VD_EXPORT_COUNT {len(names)}"]
header += [f"#define VD_IDX_{name} {i}" for i, name in enumerate(names)]
header += ["static const char *const vd_export_names[VD_EXPORT_COUNT] = {", *[f'    "{name}",' for name in names], "};", ""]
(OUT / "exports.h").write_text("\n".join(header))
assembly = ["// ABI-preserving AArch64 tail-call trampolines: only x16/x17 are scratch.", ".text", ".hidden vd_forward_targets"]
for i, name in enumerate(names):
    if name in hooks:
        continue
    assembly += [".p2align 2", f".global {name}", f".type {name}, %function", f"{name}:", "    adrp x16, vd_forward_targets", "    add x16, x16, :lo12:vd_forward_targets", f"    ldr x17, [x16, #{i * 8}]", "    br x17", f".size {name}, .-{name}"]
assembly += ['.section .note.GNU-stack,"",%progbits', ""]
(OUT / "forward.S").write_text("\n".join(assembly))
(OUT / "exports.map").write_text("{ global: xr*; local: *; };\n")
original = ORIGINAL.read_bytes()
old_soname, new_soname = b"libopenxr_loader.so\0", b"libvd_original.so\0"
assert original.count(old_soname) == 1
renamed = original.replace(old_soname, new_soname.ljust(len(old_soname), b"\0"))
assert len(renamed) == len(original)
(OUT / "libvd_original.so").write_bytes(renamed)
command = [str(CLANG), "--target=aarch64-linux-android29", "-shared", "-fPIC", "-O2", "-g", "-std=c11", "-Wall", "-Wextra", "-Werror", "-fvisibility=hidden", "-I" + str(HEADERS), "-I" + str(OUT), str(ROOT / "adapter.c"), str(OUT / "forward.S"), "-Wl,-soname,libopenxr_loader.so", "-Wl,-Bsymbolic-functions", "-Wl,--no-undefined", "-Wl,-z,relro,-z,now,-z,noexecstack", "-Wl,-z,max-page-size=16384", "-Wl,--version-script=" + str(OUT / "exports.map"), "-ldl", "-llog", "-lEGL", "-lGLESv3", "-o", str(OUT / "libopenxr_loader.so")]
subprocess.run(command, check=True)
assert exports(OUT / "libopenxr_loader.so") == names, "Proxy exports differ"
assert exports(OUT / "libvd_original.so") == names, "Renamed original exports differ"
previous_manifest_path = OUT / "manifest.json"
previous = json.loads(previous_manifest_path.read_text()) if previous_manifest_path.exists() else {}
history = sorted(set(previous.get("adapter_history_sha256", []) + ([previous["adapter_sha256"]] if "adapter_sha256" in previous else [])))
manifest = {"adapter_history_sha256": history, "original_sha256": hashlib.sha256(original).hexdigest(), "renamed_original_sha256": hashlib.sha256(renamed).hexdigest(), "adapter_sha256": hashlib.sha256((OUT / "libopenxr_loader.so").read_bytes()).hexdigest(), "export_count": len(names), "hooks": sorted(hooks), "scope": "YVR only: Pico-to-Touch bindings; per-eye scene fallback; projection texture GPU VerticalFlip; read-only tracking diagnostics", "command": command}
(OUT / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n")
print(json.dumps({k: v for k, v in manifest.items() if k != "command"}, indent=2))
