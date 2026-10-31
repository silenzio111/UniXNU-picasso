// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/UniXNULoader.h>
#include <Protocol/LoadedImage.h>
#include "../DarwinBootPkg/Driver/BootVideo.h"

STATIC
EFI_STATUS
ValidateVideo (VOID)
{
  EFI_GRAPHICS_OUTPUT_PROTOCOL             Gop;
  EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE        Mode;
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION     Info;
  EFI_STATUS                              Status;
  EFI_STATUS                              Expected;
  UINTN                                   Index;
  UINT64                                  Video[6];

  ZeroMem (&Gop, sizeof (Gop));
  for (Index = 0; Index < 8; Index++) {
    ZeroMem (&Info, sizeof (Info));
    ZeroMem (&Mode, sizeof (Mode));
    Info.HorizontalResolution = 8;
    Info.VerticalResolution = 8;
    Info.PixelsPerScanLine = 8;
    Info.PixelFormat = PixelBlueGreenRedReserved8BitPerColor;
    Mode.Info = &Info;
    Mode.SizeOfInfo = sizeof (Info);
    Mode.FrameBufferBase = 0x80000000; // Synthetic input; never accessed.
    Mode.FrameBufferSize = 256;
    Gop.Mode = &Mode;
    Expected = EFI_COMPROMISED_DATA;
    switch (Index) {
      case 0: Gop.Mode = NULL; break;
      case 1: Mode.Info = NULL; break;
      case 2: Info.PixelFormat = PixelRedGreenBlueReserved8BitPerColor; Expected = EFI_UNSUPPORTED; break;
      case 3: Info.PixelsPerScanLine = 7; break;
      case 4: Info.VerticalResolution = 0; break;
      case 5: Mode.FrameBufferBase = 0; break;
      case 6: Mode.FrameBufferSize = 255; break;
      case 7: Mode.FrameBufferBase = MAX_UINT64 - 15; break;
    }
    Status = DarwinBootVideoFromGop (&Gop, Video);
    if (Status != Expected) { break; }
  }
  if (Index != 8) { return EFI_DEVICE_ERROR; }
  Print (L"DARWINBOOT-VIDEO-VALIDATION: 8 malformed/unsupported GOP field sets refused PASS\n");
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
ValidationEntry (EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_STATUS Status;
  EFI_STATUS Expected[] = { EFI_UNSUPPORTED, EFI_LOAD_ERROR, EFI_LOAD_ERROR,
                            EFI_LOAD_ERROR, EFI_COMPROMISED_DATA, EFI_NOT_FOUND };
  CHAR16 *Paths[] = { L"bad-subtype", L"bad-command", L"bad-overlap",
                     L"bad-entry", L"bad-adt", L"missing-adt" };
  CHAR16 Path[128];
  EFI_LOADED_IMAGE_PROTOCOL *Loaded;
  UNIXNU_LOADER_PROTOCOL *Loader;
  UNIXNU_LOADER_BOOT_REQUEST Request;
  UINTN Index;
  UINT8 RamdiskByte = 0x5a;
  CHAR16 *RamdiskPaths[] = { L"missing-ramdisk-adt", L"stale-ramdisk-adt",
                            L"bad-ramdisk-size", L"duplicate-ramdisk" };
  CHAR16 *TrustPaths[] = { L"missing-trustcache", L"orphan-trustcache", L"stale-trustcache",
                          L"duplicate-trustcache", L"bad-trustcache-version", L"bad-trustcache-count",
                          L"bad-trustcache-offset", L"bad-trustcache-order" };

  CHAR16 *MemoryPaths[] = { L"bad-memory-size", L"small-memory-window", L"unaligned-memory-window",
                           L"large-memory-window", L"duplicate-memory-window", L"misplaced-memory-window" };

  (VOID)SystemTable;
  Status = gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Loaded);
  if (EFI_ERROR (Status)) { return Status; }
  Status = gBS->LocateProtocol (&gUniXNULoaderProtocolGuid, NULL, (VOID **)&Loader);
  if (EFI_ERROR (Status)) { return Status; }
  ZeroMem (&Request, sizeof (Request));
  Request.Volume = Loaded->DeviceHandle;
  Request.KernelPath = L"\\EFI\\UniXNU\\Payloads\\kernel.macho";
  if (Loader->Boot (Loader, NULL) != EFI_INVALID_PARAMETER) { goto Failed; }
  Request.KextCount = 1;
  if (Loader->Boot (Loader, &Request) != EFI_UNSUPPORTED) { goto Failed; }
  Request.KextCount = 0;
  Request.DeviceTreeSize = 1;
  if (Loader->Boot (Loader, &Request) != EFI_UNSUPPORTED) { goto Failed; }
  Request.DeviceTreeSize = 0;
  Request.RamdiskSize = 1;
  if (Loader->Boot (Loader, &Request) != EFI_BAD_BUFFER_SIZE) { goto Failed; }
  Request.RamdiskSize = 0;
  Request.Ramdisk = &RamdiskByte;
  if (Loader->Boot (Loader, &Request) != EFI_BAD_BUFFER_SIZE) { goto Failed; }
  Request.RamdiskSize = SIZE_256MB + 1;
  if (Loader->Boot (Loader, &Request) != EFI_BAD_BUFFER_SIZE) { goto Failed; }
  Request.RamdiskSize = 1;
  for (Index = 0; Index < ARRAY_SIZE (RamdiskPaths); Index++) {
    UnicodeSPrint (Path, sizeof (Path), L"\\EFI\\UniXNU\\Payloads\\%s.macho", RamdiskPaths[Index]);
    Request.KernelPath = Path;
    if (Loader->Boot (Loader, &Request) != EFI_COMPROMISED_DATA) { goto Failed; }
  }
  Request.KernelPath = L"\\EFI\\UniXNU\\Payloads\\kernel.macho";
  Request.Ramdisk = NULL;
  Request.RamdiskSize = 0;
  if (Loader->Boot (Loader, &Request) != EFI_UNSUPPORTED) { goto Failed; }
  Request.BootArgs = L"\x100";
  if (Loader->Boot (Loader, &Request) != EFI_BAD_BUFFER_SIZE) { goto Failed; }
  Request.BootArgs = NULL;
  for (Index = 0; Index < ARRAY_SIZE (TrustPaths); Index++) {
    UnicodeSPrint (Path, sizeof (Path), L"\\EFI\\UniXNU\\Payloads\\%s.macho", TrustPaths[Index]);
    Request.KernelPath = Path;
    if (Loader->Boot (Loader, &Request) != EFI_COMPROMISED_DATA) { goto Failed; }
  }
  Request.KernelPath = L"\\EFI\\UniXNU\\Payloads\\unavailable-memory-window.macho";
  if (Loader->Boot (Loader, &Request) != EFI_OUT_OF_RESOURCES) { goto Failed; }
  for (Index = 0; Index < ARRAY_SIZE (MemoryPaths); Index++) {
    UnicodeSPrint (Path, sizeof (Path), L"\\EFI\\UniXNU\\Payloads\\%s.macho", MemoryPaths[Index]);
    Request.KernelPath = Path;
    if (Loader->Boot (Loader, &Request) != EFI_COMPROMISED_DATA) { goto Failed; }
  }
  for (Index = 0; Index < ARRAY_SIZE (Paths); Index++) {
    UnicodeSPrint (Path, sizeof (Path), L"\\EFI\\UniXNU\\Payloads\\%s.macho", Paths[Index]);
    Request.KernelPath = Path;
    Status = Loader->Boot (Loader, &Request);
    if (Status != Expected[Index]) {
      Print (L"DARWINBOOT-VALIDATION: %s returned %r expected %r\n", Paths[Index], Status, Expected[Index]);
      goto Failed;
    }
  }
  Print (L"DARWINBOOT-VALIDATION: 33 rejected requests; firmware retained PASS\n");
  if (EFI_ERROR (ValidateVideo ())) { goto Failed; }
  return EFI_SUCCESS;
Failed:
  Print (L"DARWINBOOT-VALIDATION: FAIL\n");
  return EFI_DEVICE_ERROR;
}
