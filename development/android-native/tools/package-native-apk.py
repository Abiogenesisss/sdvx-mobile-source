"""Assemble fresh Gradle outputs without thousands of per-asset intermediate JARs.

Gradle owns resource linking, DEX, native stripping and asset merging. This step
only combines their completed outputs; Build-SDVX.ps1 aligns and signs it.
"""
import argparse
from pathlib import Path
import zipfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("staging", type=Path)
parser.add_argument("output", type=Path)
parser.add_argument("abi", choices=("x86_64", "arm64-v8a"))
args = parser.parse_args()
build = args.staging / "build"
resources = build / "intermediates/linked_resources_binary_format/debug/processDebugResources/linked-resources-binary-format-debug.ap_"
native = build / f"intermediates/stripped_native_libs/debug/stripDebugDebugSymbols/out/lib/{args.abi}"
dex = build / "intermediates/dex/debug"
project_dex = sorted((dex / "mergeProjectDexDebug").glob("*/classes*.dex"))
library_dex = sorted((dex / "mergeLibDexDebug").glob("classes*.dex"))
external_dex = sorted((dex / "mergeExtDexDebug").glob("classes*.dex"))
java_resources = build / "intermediates/merged_java_res/debug/mergeDebugJavaResource/base.jar"
assets = build / "nabla-assets"
libraries = {p.name for p in native.glob("*.so")}
if not project_dex or not external_dex or libraries != {
    "libmain.so", "libSDL3.so", "libc++_shared.so", "libbass.so", "libbass_fx.so",
    "libavcodec.so", "libavformat.so", "libavutil.so", "libswresample.so",
} or not (assets / "crew/grace/model/grace_ver7.moc3").is_file():
    raise RuntimeError("Required current native build outputs are missing")

stored = {".wav", ".flac", ".s3v", ".webp", ".png", ".jpg", ".jpeg", ".ogg", ".mp3", ".dat", ".zip"}
args.output.parent.mkdir(parents=True, exist_ok=True)
# APK tools read unsigned 32-bit ZIP offsets. Python switches to ZIP64 at 2 GiB,
# even though classic ZIP offsets can go up to the reserved 0xFFFFFFFF marker.
zipfile.ZIP64_LIMIT = 0xFFFFFFFE
with zipfile.ZipFile(args.output, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=2, allowZip64=False) as apk:
    with zipfile.ZipFile(resources) as source:
        for entry in source.infolist():
            if not entry.is_dir():
                apk.writestr(entry.filename, source.read(entry), compress_type=entry.compress_type)
    with zipfile.ZipFile(java_resources) as source:
        for entry in source.infolist():
            if not entry.is_dir() and entry.filename != "META-INF/MANIFEST.MF":
                apk.writestr(entry.filename, source.read(entry), compress_type=entry.compress_type)
    for index, file in enumerate(project_dex + library_dex + external_dex, 1):
        apk.write(file, "classes.dex" if index == 1 else f"classes{index}.dex")
    for file in sorted(native.glob("*.so")):
        apk.write(file, f"lib/{args.abi}/{file.name}", compress_type=zipfile.ZIP_STORED)
    count = 0
    for file in sorted(p for p in assets.rglob("*") if p.is_file()):
        apk.write(file, "assets/" + file.relative_to(assets).as_posix(),
                  compress_type=zipfile.ZIP_STORED if file.suffix.lower() in stored else zipfile.ZIP_DEFLATED)
        count += 1
print(f"Assembled {args.abi}: {count} current assets, {args.output.stat().st_size:,} bytes")
