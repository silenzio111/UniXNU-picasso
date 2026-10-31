// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef DARWINBOOT_PMIC_PWRON_H_
#define DARWINBOOT_PMIC_PWRON_H_

//
// The subset of the Qualcomm PMIC PwrOn protocol used by DarwinBoot. Keep the
// members before SetSpareReg in place: its UEFI protocol offset is +0x28.
//
#define DARWINBOOT_PMIC_PWRON_PROTOCOL_GUID \
  { 0x9ba45b66, 0xefa4, 0x441c, { 0xa3, 0xe4, 0xed, 0x22, 0x24, 0x78, 0x6b, 0xe2 } }

#define DARWINBOOT_PMIC_PWRON_REVISION_1000A  0x000000000001000AULL
#define DARWINBOOT_PMIC_PWRON_REVISION_1000B  0x000000000001000BULL
#define DARWINBOOT_PRIMARY_PMIC         0
#define DARWINBOOT_FASTBOOT_MODE        2
#define DARWINBOOT_PON_REASON_SHIFT     1
#define DARWINBOOT_PON_REASON_MASK      0xFE

typedef enum {
  DarwinBootPonPerphSpare,
  DarwinBootPonDvddSpare,
  DarwinBootPonXvddSpare,
  DarwinBootPonSoftSpare,
  DarwinBootPonSpareInvalid
} DARWINBOOT_PON_SPARE_REG_TYPE;

typedef
EFI_STATUS
(EFIAPI *DARWINBOOT_PON_GET_SPARE_REG)(
  IN UINT32                         PmicDeviceIndex,
  IN DARWINBOOT_PON_SPARE_REG_TYPE  SpareReg,
  OUT UINT8                         *Data
  );

typedef
EFI_STATUS
(EFIAPI *DARWINBOOT_PON_SET_SPARE_REG)(
  IN UINT32                         PmicDeviceIndex,
  IN DARWINBOOT_PON_SPARE_REG_TYPE  SpareReg,
  IN UINT8                          SetValue,
  IN UINT8                          Mask
  );

typedef struct {
  UINT64                          Revision;
  VOID                            *GetPonPblStatus;
  VOID                            *SetPonTrigger;
  VOID                            *GetPonRtStatus;
  DARWINBOOT_PON_GET_SPARE_REG    GetSpareReg;
  DARWINBOOT_PON_SET_SPARE_REG    SetSpareReg;
} DARWINBOOT_PMIC_PWRON_PROTOCOL;

#endif
