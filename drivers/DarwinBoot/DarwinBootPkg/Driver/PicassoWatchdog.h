// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef DARWINBOOT_PICASSO_WATCHDOG_H_
#define DARWINBOOT_PICASSO_WATCHDOG_H_

//
// Picasso APSS watchdog. The counter is 20 bits at 32768 Hz, so 0xfffff is
// the longest representable interval (32 seconds). Keep this armed across the
// UEFI-to-XNU handoff: XNU pets or re-arms the same controller, while a hang
// before that point still returns to the Fastboot reason set above.
//
#define DARWINBOOT_APSS_WDT_BASE  0x17C10000U
#define DARWINBOOT_APSS_WDT_RST   0x04U
#define DARWINBOOT_APSS_WDT_EN    0x08U
#define DARWINBOOT_APSS_WDT_STATUS 0x0CU
#define DARWINBOOT_APSS_WDT_BARK  0x10U
#define DARWINBOOT_APSS_WDT_BITE  0x14U
#define DARWINBOOT_APSS_WDT_TICKS 0xFFFFFU

STATIC
UINT32
DarwinBootWdtRead (
  IN UINT32  Offset
  )
{
  MemoryFence ();
  return *(volatile UINT32 *)(UINTN)(DARWINBOOT_APSS_WDT_BASE + Offset);
}

STATIC
VOID
DarwinBootWdtWrite (
  IN UINT32  Offset,
  IN UINT32  Value
  )
{
  *(volatile UINT32 *)(UINTN)(DARWINBOOT_APSS_WDT_BASE + Offset) = Value;
  MemoryFence ();
}

STATIC
BOOLEAN
ArmPicassoWatchdog (
  IN BOOLEAN  FastbootReasonArmed
  )
{
  UINT32  Bark;
  UINT32  Bite;
  UINT32  Control;
  UINT32  Status;

  if (!FastbootReasonArmed) {
    Print (L"DARWINBOOT: watchdog not armed because the Fastboot reason failed\n");
    return FALSE;
  }

  DarwinBootWdtWrite (DARWINBOOT_APSS_WDT_EN, 0);
  DarwinBootWdtWrite (DARWINBOOT_APSS_WDT_RST, 1);
  DarwinBootWdtWrite (DARWINBOOT_APSS_WDT_BARK, DARWINBOOT_APSS_WDT_TICKS);
  DarwinBootWdtWrite (DARWINBOOT_APSS_WDT_BITE, DARWINBOOT_APSS_WDT_TICKS);
  DarwinBootWdtWrite (DARWINBOOT_APSS_WDT_EN, 1);

  Bark = DarwinBootWdtRead (DARWINBOOT_APSS_WDT_BARK);
  Bite = DarwinBootWdtRead (DARWINBOOT_APSS_WDT_BITE);
  Control = DarwinBootWdtRead (DARWINBOOT_APSS_WDT_EN);
  Status = DarwinBootWdtRead (DARWINBOOT_APSS_WDT_STATUS);
  if ((Bark != DARWINBOOT_APSS_WDT_TICKS) ||
      (Bite != DARWINBOOT_APSS_WDT_TICKS) ||
      ((Control & 1U) == 0)) {
    DarwinBootWdtWrite (DARWINBOOT_APSS_WDT_EN, 0);
    Print (L"DARWINBOOT: watchdog rejected bark=%x bite=%x ctl=%x status=%x\n",
           Bark, Bite, Control, Status);
    return FALSE;
  }

  Print (L"DARWINBOOT: watchdog armed 32000 ms bark=%x bite=%x ctl=%x status=%x\n",
         Bark, Bite, Control, Status);
  return TRUE;
}

#endif
