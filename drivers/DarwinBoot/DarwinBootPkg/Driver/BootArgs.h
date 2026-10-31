// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef DARWIN_BOOT_ARGS_H_
#define DARWIN_BOOT_ARGS_H_

#include <Base.h>

// Darwin 23 pexpert/pexpert/arm64/boot.h ABI; all fields are little-endian.
typedef struct {
  UINT16  Revision;
  UINT16  Version;
  UINT32  Padding;
  UINT64  VirtualBase;
  UINT64  PhysicalBase;
  UINT64  MemorySize;
  UINT64  TopOfKernelData;
  UINT64  Video[6];
  UINT32  MachineType;
  UINT32  TreePadding;
  UINT64  DeviceTree;
  UINT32  DeviceTreeLength;
  CHAR8   CommandLine[1024];
  UINT32  FlagsPadding;
  UINT64  BootFlags;
  UINT64  MemorySizeActual;
} DARWIN_BOOT_ARGS;

STATIC_ASSERT (OFFSET_OF (DARWIN_BOOT_ARGS, DeviceTree) == 0x60, "DT ABI");
STATIC_ASSERT (OFFSET_OF (DARWIN_BOOT_ARGS, Video) == 0x28, "video ABI");
STATIC_ASSERT (OFFSET_OF (DARWIN_BOOT_ARGS, CommandLine) == 0x6c, "command line ABI");
STATIC_ASSERT (OFFSET_OF (DARWIN_BOOT_ARGS, MemorySizeActual) == 0x478, "memory ABI");
STATIC_ASSERT (sizeof (DARWIN_BOOT_ARGS) == 0x480, "boot_args ABI");

#endif
