#!/usr/bin/env python3
"""Build the owned CPU/GOP NativeLegacyDisplay runtime archive.

The original class and its audited bindings are preserved. The owned subclass
is entered only through the explicit CDC commands; it never publishes at boot.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
DEVICE = Path(os.environ.get("UNIXNU_DEVICE", ROOT))
OUT = ROOT / "build/native-legacy-display"
DRIVER = DEVICE / "drivers/NativeMobileDisplay"
KEYSTORE = DEVICE / "drivers/AppleKeyStoreState"
RECORD = ROOT / (
    "build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/"
    "iokit/DEVELOPMENT/IOPlatformExpert.cpo.json"
)
CANDIDATE = ROOT / "build/native-prelink/native-owner-mobile-graphics-candidate.macho"
AUDIT_JSON = ROOT / "build/display-analysis/mobile-graphics-legacy-abi.json"

sys.path.insert(0, str(ROOT / "kits/NativePrelink"))
from native_prelink import backed_offset, macho  # noqa: E402


def run(command, **kwargs):
    return subprocess.check_output([str(value) for value in command], **kwargs)


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def source_name(path):
    path = Path(path)
    for base in (ROOT, DEVICE):
        try:
            return str(path.relative_to(base))
        except ValueError:
            pass
    return str(path)


def require(condition, message):
    if not condition:
        raise SystemExit(message)


def compile_source(source, destination, extra_flags):
    record = json.loads(RECORD.read_text().strip().rstrip(","))
    command = []
    arguments = record["arguments"]
    index = 0
    while index < len(arguments):
        value = arguments[index]
        if value == record["file"]:
            command.append(str(Path(source).resolve()))
        elif value == "-o":
            command.extend(["-o", str(Path(destination).resolve())])
            index += 1
        elif value == "-Xclang" and arguments[index + 1].startswith("-W"):
            index += 1
        elif not value.startswith("-W"):
            command.append(value)
        index += 1
    command.extend([
        "-Wall",
        "-Wextra",
        "-Wno-unused-parameter",
        "-Wno-c99-designator",
        "-Wno-reorder-init-list",
        "-Wno-deprecated-enum-enum-conversion",
        "-Wno-unnecessary-virtual-specifier",
        "-Wno-unused-variable",
        "-Wno-unused-private-field",
        "-I",
        str(ROOT / ".cache/xnu-work/osfmk"),
        "-I",
        str(OUT),
    ])
    host_sdk = subprocess.run(
        ["xcrun", "--sdk", "macosx", "--show-sdk-path"],
        capture_output=True, text=True
    )
    if host_sdk.returncode == 0 and host_sdk.stdout.strip():
        command.extend(
            ["-isystem", str(Path(host_sdk.stdout.strip()) / "usr/include")]
        )
    command.extend(extra_flags)
    destination.parent.mkdir(parents=True, exist_ok=True)
    result = subprocess.run(
        command, cwd=record["directory"], capture_output=True, text=True
    )
    if result.returncode:
        raise SystemExit(
            f"Failed to compile {source}:\n{result.stdout}{result.stderr}"
        )


def generate_abi_header(audit):
    pure = audit["abstract_slots"]
    header = """// SPDX-License-Identifier: BSD-2-Clause-Patent
// Opaque ABI storage for the exact original class, not source-level access
// to its private fields. The actual original metadata supplies its full
// Framebuffer/Service/IOService ancestry. Only qualified base prototypes may
// be called from source; opaque entries are original dispatch targets only.
#pragma once
#include <IOKit/IOService.h>
#include <libkern/libkern.h>
struct OriginalLegacySwapRequest;
class IOMobileFramebufferLegacy : public IOService {
 OSDeclareAbstractStructors(IOMobileFramebufferLegacy)
 unsigned char originalPrivate[0x18a0-sizeof(IOService)];
public:
 // Both fields are qualified by the complete original slot-249 getter.
 // This modifies this object's own mode state, never a vtable or executable.
 void installBootDimensions(uint32_t width,uint32_t height) {
  *reinterpret_cast<uint32_t *>(reinterpret_cast<unsigned char *>(this)+5808)=width;
  *reinterpret_cast<uint32_t *>(reinterpret_cast<unsigned char *>(this)+5812)=height;
 }
 const OriginalLegacySwapRequest *pendingSwapRequest() const {
  const OriginalLegacySwapRequest *request=nullptr;
  memcpy(&request,reinterpret_cast<const unsigned char *>(this)+5480,sizeof(request));return request;
 }
 // The original family queues swaps in a tail queue. Taking the head moves
 // the next request, if any, into place before calling slot 295.
 bool activateOnlyPendingSwap(const OriginalLegacySwapRequest *request) {
  auto self=reinterpret_cast<unsigned char *>(this);
  auto bytes=reinterpret_cast<const unsigned char *>(request);
  const void *current=nullptr,*head=nullptr,*tail=nullptr,*next=nullptr,*previous=nullptr;
  memcpy(&current,self+5472,8);memcpy(&head,self+5480,8);memcpy(&tail,self+5488,8);
  if(!request || current || head!=request)return false;
  memcpy(&next,bytes+2960,8);memcpy(&previous,bytes+2968,8);
  if(previous!=self+5480)return false;
  if(next) {
   const void *link=self+5480;
   memcpy(const_cast<unsigned char *>(reinterpret_cast<const unsigned char *>(next))+2968,&link,8);
  } else if(tail!=bytes+2960)return false;
  memcpy(self+5472,&request,8);
  if(next) {memcpy(self+5480,&next,8);}
  else {const void *empty=nullptr,*emptyTail=self+5480;memcpy(self+5480,&empty,8);memcpy(self+5488,&emptyTail,8);}
  return true;
 }
 void free() override;
 bool init(OSDictionary *properties=nullptr) override;
 IOReturn setProperties(OSObject *) override;
 void systemWillShutdown(IOOptionBits) override;
 IOService *probe(IOService *,SInt32 *) override;
 bool start(IOService *) override;
 void stop(IOService *) override;
 IOWorkLoop *getWorkLoop() const override;
 IOReturn newUserClient(task_t,void *,UInt32,IOUserClient **) override;
 IOReturn setPowerState(unsigned long,IOService *) override;
"""
    for slot in range(168, 358):
        if slot == 184:
            header += " virtual bool isMainDisplay();\n"
        elif slot == 188:
            header += " virtual IOReturn getDotPitch(uint32_t *);\n"
        elif slot == 213:
            header += (
                " virtual IOReturn supportedFrameInfo(uint32_t,uint32_t *,char *,"
                "uint32_t *)=0;\n"
            )
        elif slot == 249:
            header += " virtual void getDisplayDimensions(uint32_t *,uint32_t *);\n"
        elif slot == 290:
            header += " virtual void kickSwapQueue();\n"
        elif slot == 296:
            header += " virtual bool validateSwap(const OriginalLegacySwapRequest *);\n"
        elif slot == 295:
            header += " virtual void completeCurrentSwap();\n"
        elif slot == 337:
            header += (
                " virtual IOReturn collectCompletionReports(uint32_t,void *,"
                "uint32_t *,uint32_t)=0;\n"
            )
        elif slot == 288:
            header += " virtual bool startHardware();\n"
        elif slot == 301:
            header += " virtual bool getRefreshAttributes(uint64_t *,bool);\n"
        elif slot in pure:
            header += (
                f" virtual __attribute__((noreturn)) void opaque{slot}()=0;\n"
            )
        else:
            header += (
                ' virtual __attribute__((deprecated("Unclassified ABI: never call '
                f'from source"))) void opaque{slot}();\n'
            )
    header += (
        '};\nstatic_assert(sizeof(IOMobileFramebufferLegacy)==0x18a0,'
        '"Actual original instance extent");\n'
    )
    path = OUT / "OriginalLegacyFramebufferABI.h"
    path.write_text(header)
    return path


def object_symbols_and_vtable(object_path):
    raw = object_path.read_bytes()
    image = macho(raw)
    _, command, _ = next(
        value for value in image["commands"] if value[0] == 2
    )
    symoff, count, stringoff, _ = struct.unpack_from("<4I", raw, command + 8)
    symbols = []
    for index in range(count):
        name_offset, kind, section, _, value = struct.unpack_from(
            "<IBBHQ", raw, symoff + index * 16
        )
        name = raw[
            stringoff + name_offset : raw.index(b"\0", stringoff + name_offset)
        ].decode()
        symbols.append((name, value, kind))
    vtable = next(
        value for name, value, _ in symbols
        if name == "__ZTV19NativeLegacyDisplay"
    )
    relocations = {}
    for segment in image["segments"]:
        for target in segment["sections"]:
            if target["va"] <= vtable < target["va"] + target["size"]:
                relocation_offset, relocation_count = struct.unpack_from(
                    "<2I", raw, target["command_offset"] + 56
                )
                for index in range(relocation_count):
                    offset, bits = struct.unpack_from(
                        "<2I", raw, relocation_offset + index * 8
                    )
                    if (
                        bits >> 27 & 1
                        and bits >> 28 == 0
                        and bits >> 25 & 3 == 3
                    ):
                        relocations[target["va"] + offset] = symbols[
                            bits & 0xFFFFFF
                        ][0]
    compiled = [
        relocations.get(vtable + 16 + slot * 8) for slot in range(358)
    ]
    require(
        all(compiled) and vtable + 16 + 358 * 8 not in relocations,
        "Exact owned 358-entry vtable",
    )
    return raw, image, symbols, compiled


def validate_original_code(candidate, audit):
    raw = candidate.read_bytes()
    image = macho(raw)

    def read(address, length):
        offset = backed_offset(image["segments"], address, length, len(raw))
        return raw[offset : offset + length]

    for name, address, field, wide in (
        ("descriptor", 0xFFFFFFF004B9EAD0, 48, True),
        ("width", 0xFFFFFFF004B9EBBC, 88, True),
        ("height", 0xFFFFFFF004B9EBC4, 96, True),
        ("stride", 0xFFFFFFF004B9EBD4, 104, True),
        ("allocation", 0xFFFFFFF004B9EBFC, 144, True),
        ("format", 0xFFFFFFF004B9EBAC, 128, False),
        ("planes", 0xFFFFFFF004B9EBF4, 152, False),
    ):
        expected = (0xF9400000 if wide else 0xB9400000) | (
            (field // (8 if wide else 4)) << 10
        )
        require(
            read(address, 8) == struct.pack("<2I", expected, 0xD65F03C0),
            f"Exact original IOSurface getter {name}",
        )
    require(
        read(0xFFFFFFF005796720, 16)
        == bytes.fromhex("ff0302d1fd7b07a9fdc30191a0831ff8")
        and read(0xFFFFFFF005796830, 4) == bytes.fromhex("08ed4af9")
        and read(0xFFFFFFF00579685c, 16)
        == bytes.fromhex("e2230091c3008052240080521684ff97")
        and read(0xFFFFFFF005795fbc, 4) == bytes.fromhex("0ab182d2"),
        "Original vsync sender and six-word client message",
    )
    require(
        read(0xFFFFFFF00579EBB8, 16)
        == bytes.fromhex("ff0302d1fd7b07a9fdc30191a0031ff8")
        and read(0xFFFFFFF00579ED54, 16)
        == bytes.fromhex("086540f90173ff90218c0c9100013fd6"),
        "Original setDisplayRefreshProperties publication",
    )
    require(
        int(audit["methods"][295]["address"], 16) == 0xFFFFFFF005795B88
        and read(0xFFFFFFF005795B98, 16)
        == bytes.fromhex("e00740f9280080520101001272d3ff97"),
        "Original slot-295 completion contract",
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    require(CANDIDATE.is_file(), f"missing original candidate: {CANDIDATE}")
    # `unixnu tree` refreshes scripts/, but the remaining inputs are linked
    # sources. Copy the driver/header here so a build from the assembled tree
    # never compiles a stale .cache checkout.
    shutil.copy2(DRIVER / "NativeLegacyDisplay.cpp", OUT / "NativeLegacyDisplay.cpp")
    shutil.copy2(DRIVER / "NativeLegacyRequest.h", OUT / "NativeLegacyRequest.h")
    shutil.copy2(DRIVER / "PicassoGop.h", OUT / "PicassoGop.h")

    run(
        [sys.executable, ROOT / "scripts/audit-mobile-graphics-legacy.py"],
        cwd=ROOT,
    )
    audit = json.loads(AUDIT_JSON.read_text())
    require(
        audit["candidate_sha256"] == sha256(CANDIDATE),
        "Audit and original candidate hashes differ",
    )
    require(
        audit["legacy_virtual_entries"] == 358
        and audit["legacy_instance_bytes"] == 0x18A0
        and audit["abstract_slots"] == [211, 213, 245, 246, 306, 307, 337, 338, 345],
        "Unexpected original Legacy ABI",
    )
    validate_original_code(CANDIDATE, audit)

    generated_header = generate_abi_header(audit)
    source = OUT / "NativeLegacyDisplay.cpp"
    obj = OUT / "NativeLegacyDisplay.o"
    compile_source(source, obj, [])
    raw, image, symbols, compiled = object_symbols_and_vtable(obj)

    kernel_report_path = ROOT / "kernel/kernel.json"
    if not kernel_report_path.is_file():
        kernel_report_path = ROOT.parent / "kernel/kernel.json"
    kernel_report = json.loads(kernel_report_path.read_text())
    graphics_data_const = next(
        extent
        for extent in kernel_report["extents"]
        if extent["module"] == "com.apple.iokit.IOMobileGraphicsFamily"
        and extent["segment"] == "__DATA_CONST"
    )
    # The current kernel's fully resolved Legacy symbol is authoritative. The
    # old prelinked image had a different class selected at +0x6350; the
    # IOMobileGraphicsFamily copy carries the audited object at the final
    # prelink address after the symbol is resolved.
    legacy_gmeta = 0xFFFFFFF006B35940
    require(
        graphics_data_const["va"] == 0xFFFFFFF005F33840,
        "Unexpected IOMobileGraphicsFamily __DATA_CONST layout",
    )
    bindings = {
        "__ZN25IOMobileFramebufferLegacyC2EPK11OSMetaClass":
            0xFFFFFFF0057865CC,
        "__ZN25IOMobileFramebufferLegacyD2Ev": 0xFFFFFFF005786618,
        "__ZN25IOMobileFramebufferLegacy10gMetaClassE": legacy_gmeta,
        "__ZTV25IOMobileFramebufferLegacy":
            int(audit["legacy_vptr"], 16) - 16,
        "_native_legacy_surface_descriptor": 0xFFFFFFF004B9EAD0,
        "_native_legacy_surface_width": 0xFFFFFFF004B9EBBC,
        "_native_legacy_surface_height": 0xFFFFFFF004B9EBC4,
        "_native_legacy_surface_stride": 0xFFFFFFF004B9EBD4,
        "_native_legacy_surface_allocation": 0xFFFFFFF004B9EBFC,
        "_native_legacy_surface_format": 0xFFFFFFF004B9EBAC,
        "_native_legacy_surface_planes": 0xFFFFFFF004B9EBF4,
        "_native_legacy_vsync_notify": 0xFFFFFFF005796720,
        "_native_legacy_set_refresh_properties": 0xFFFFFFF00579EBB8,
    }
    owned = []
    inherited = []
    qualified_owned = {
        0, 1, 7, 14, 84, 86, 87, 141, 155, 184, 188, 288, 290, 296, 301,
        211, 213, 245, 246, 306, 307, 337, 338, 345,
    }
    for slot, name in enumerate(compiled):
        method_address = int(audit["methods"][slot]["address"], 16)
        if name.startswith(
            ("__ZN19NativeLegacyDisplay", "__ZNK19NativeLegacyDisplay")
        ):
            owned.append(slot)
            require(
                slot in qualified_owned,
                f"NativeLegacyDisplay unexpectedly overrides slot {slot}",
            )
        elif "25IOMobileFramebufferLegacy" in name:
            bindings[name] = method_address
        else:
            inherited.append(slot)
    require(
        owned == sorted(qualified_owned),
        "NativeLegacyDisplay override set differs from the qualified ABI",
    )

    undefined_legacy = {
        name
        for name, _, kind in symbols
        if kind == 1 and "25IOMobileFramebufferLegacy" in name
    }
    require(
        undefined_legacy == {
            name for name in bindings if "25IOMobileFramebufferLegacy" in name
        },
        "Original Legacy imports and generated bindings differ",
    )

    assembly = OUT / "original-bindings.S"
    assembly.write_text(
        "".join(
            f".globl {name}\n.set {name}, {address:#x}\n"
            for name, address in sorted(bindings.items())
        )
    )
    bindings_object = OUT / "original-bindings.o"
    subprocess.run(
        [
            "xcrun",
            "clang",
            "-target",
            "arm64-apple-ios17.0",
            "-c",
            str(assembly),
            "-o",
            str(bindings_object),
        ],
        check=True,
    )

    llvm = Path(run(["brew", "--prefix", "llvm"], text=True).strip()) / "bin"
    late = OUT / "NativeLegacyDisplay.late.o"
    subprocess.run(
        [
            str(llvm / "llvm-objcopy"),
            "--remove-section=__DATA,__mod_init_func",
            "--globalize-symbol=__GLOBAL__sub_I_NativeLegacyDisplay.cpp",
            str(obj),
            str(late),
        ],
        check=True,
    )

    def text_bytes(data, info):
        section = next(
            item
            for segment in info["segments"]
            for item in segment["sections"]
            if item["name"] == "__text"
        )
        return data[
            section["fileoff"] : section["fileoff"] + section["size"]
        ]

    require(
        text_bytes(raw, image)
        == text_bytes(late.read_bytes(), macho(late.read_bytes())),
        "Late staging changed NativeLegacyDisplay instructions",
    )

    support_source = DRIVER / "PicassoDisplaySupport.cpp"
    support_object = OUT / "picasso-display-support.o"
    compile_source(
        support_source,
        support_object,
        ["-I", str(DRIVER)],
    )
    support_objects = [support_object]
    keyevents_source = KEYSTORE / "NativeKeyStoreEvents.cpp"
    keyevents_object = OUT / "NativeKeyStoreEvents.o"
    compile_source(
        keyevents_source,
        keyevents_object,
        ["-I", str(KEYSTORE)],
    )
    keyevents_late = OUT / "NativeKeyStoreEvents.late.o"
    subprocess.run(
        [
            str(llvm / "llvm-objcopy"),
            "--remove-section=__DATA,__mod_init_func",
            "--globalize-symbol=__GLOBAL__sub_I_NativeKeyStoreEvents.cpp",
            str(keyevents_object),
            str(keyevents_late),
        ],
        check=True,
    )
    keystore_source = KEYSTORE / "PicassoKeyStore.cpp"
    keystore_object = OUT / "PicassoKeyStore.o"
    compile_source(
        keystore_source,
        keystore_object,
        ["-I", str(KEYSTORE)],
    )
    build_mode = "runtime support; display and ephemeral keystore state through CDC"

    archive = OUT / "libNativeLegacyDisplay.late.a"
    archive.unlink(missing_ok=True)
    subprocess.run(
        [
            "xcrun",
            "ar",
            "rcs",
            str(archive),
            str(late),
            str(bindings_object),
            str(keyevents_late),
            str(keystore_object),
            *[str(path) for path in support_objects],
        ],
        check=True,
    )

    sources = [
        Path(__file__).resolve(),
        source,
        DRIVER / "NativeLegacyRequest.h",
        generated_header,
        ROOT / "scripts/audit-mobile-graphics-legacy.py",
        AUDIT_JSON,
        support_source,
        DRIVER / "PicassoGop.h",
        keyevents_source,
        keystore_source,
    ]
    report = {
        "mode": build_mode,
        "runtime_qualified": False,
        "hardware_tested": False,
        "original_instructions_modified": False,
        "static_initializer_removed": True,
        "original_class_bindings": {
            name: hex(address) for name, address in sorted(bindings.items())
        },
        "owned_slots": owned,
        "inherited_native_slots": inherited,
        "virtual_entries": 358,
        "abstract_slots": audit["abstract_slots"],
        "candidate_sha256": sha256(CANDIDATE),
        "object_sha256": sha256(obj),
        "late_object_sha256": sha256(late),
        "keystore_object_sha256": sha256(keystore_object),
        "keystore_late_object_sha256": sha256(keyevents_late),
        "archive_sha256": sha256(archive),
        "source_sha256": {
            source_name(path): sha256(path) for path in sources
        },
    }
    (OUT / "build-report.json").write_text(
        json.dumps(report, indent=2) + "\n"
    )
    print(
        "NativeLegacyDisplay: CPU/GOP runtime archive built "
        f"({len(bindings)} original bindings, {len(owned)} owned slots)"
    )


if __name__ == "__main__":
    main()
