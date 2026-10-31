// SPDX-License-Identifier: BSD-2-Clause-Patent
// Device-level Picasso handoff; no optional PCD may silently omit this path.
#include "PmicPwrOn.h"

STATIC EFI_GUID  mPmicPwrOnProtocolGuid = DARWINBOOT_PMIC_PWRON_PROTOCOL_GUID;

STATIC
BOOLEAN
SetPanicResetReason (
  VOID
  )
{
  DARWINBOOT_PMIC_PWRON_PROTOCOL  *PwrOn;
  UINT8                            Spare;
  EFI_STATUS                       Status;

  Status = gBS->LocateProtocol (&mPmicPwrOnProtocolGuid, NULL, (VOID **)&PwrOn);
  if (EFI_ERROR (Status) || (PwrOn == NULL)) {
    Print (L"DARWINBOOT: PMIC PwrOn unavailable (%r); panic will not preset Fastboot\n", Status);
    return FALSE;
  }
  //
  // Picasso ships revision 1000A; newer firmware uses 1000B. Both published
  // protocol tables keep the members through SetSpareReg at the same offsets.
  //
  if ((PwrOn->Revision != DARWINBOOT_PMIC_PWRON_REVISION_1000A) &&
      (PwrOn->Revision != DARWINBOOT_PMIC_PWRON_REVISION_1000B)) {
    Print (L"DARWINBOOT: PMIC PwrOn revision %lx is unsupported; panic will not preset Fastboot\n",
           PwrOn->Revision);
    return FALSE;
  }

  //
  // ABL reads SPARE[7:1] through PmicPwrOn after the reset and treats 2 as
  // FASTBOOT_MODE. This only arms the reason; XNU still owns the reset.
  //
  Status = PwrOn->SetSpareReg (
                    DARWINBOOT_PRIMARY_PMIC,
                    DarwinBootPonSoftSpare,
                    DARWINBOOT_FASTBOOT_MODE << DARWINBOOT_PON_REASON_SHIFT,
                    DARWINBOOT_PON_REASON_MASK
                    );
  if (EFI_ERROR (Status)) {
    Print (L"DARWINBOOT: PMIC Fastboot reset reason failed (%r)\n", Status);
    return FALSE;
  }

  Status = PwrOn->GetSpareReg (DARWINBOOT_PRIMARY_PMIC, DarwinBootPonSoftSpare, &Spare);
  if (EFI_ERROR (Status)) {
    Print (L"DARWINBOOT: PMIC Fastboot reason readback failed (%r)\n", Status);
    return FALSE;
  } else if ((Spare & DARWINBOOT_PON_REASON_MASK) !=
             (DARWINBOOT_FASTBOOT_MODE << DARWINBOOT_PON_REASON_SHIFT)) {
    Print (L"DARWINBOOT: PMIC Fastboot reason did not stick (spare %02x)\n", Spare);
    return FALSE;
  } else {
    Print (L"DARWINBOOT: panic reset armed for Fastboot (rev %lx, spare %02x)\n",
           PwrOn->Revision, Spare);
  }

  return TRUE;
}

