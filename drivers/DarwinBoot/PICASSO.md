# Picasso reset-reason override

Base: DarwinBoot `d099354d0eab34df5088ca1ba89c9c88bc3260bb`.
This device-local copy preserves the original trust-cache, ramdisk, video and
kernel handoff code. Its only loader change calls the PMIC reset-reason helper
immediately before the final GetMemoryMap/ExitBootServices loop.

`PmicPwrOn.h` and the helper come from the existing local Picasso firmware
bring-up changes in `UniXNU/drivers/DarwinBoot`. They accept protocol revisions
1000A/1000B, write primary PMIC SOFT_SPARE bits 7:1 to Fastboot mode 2, and
check the readback. They do not request a reset or access any storage partition.
Failure is explicitly printed and is not proof of a recoverable boot.

The override is intentionally not gated by the optional generic PCD: the
actually tested r88/m10/m12 FD contains a DarwinBoot with neither this protocol
GUID nor any SetSpareReg path, despite a separate Mu-Silicium DSC enabling the
PCD. Source configuration alone must never qualify the packaged FD.

Required static gate: extract the DarwinBoot PE from the actual boot image,
verify its hash against the just-built module, and verify the PwrOn GUID and
success/failure messages. This is necessary but not proof of a successful
PMIC transaction or reset-to-Fastboot on hardware.
