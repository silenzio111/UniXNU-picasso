# Picasso control-plane recovery evidence (2026-10-31)

Temporary `fastboot boot` only. No flash, erase, or format was used.

## Recovery chain

- DarwinBoot writes and verifies the PMIC Fastboot reason, then arms the
  Picasso APSS watchdog at `0x17c10000` for its 32-second maximum immediately
  before the UEFI-to-XNU handoff.
- XNU re-arms the same watchdog and services it from the live timer interrupt.
- CDC gets a 120-second boot grace. After the host attaches, a parsed command
  refreshes a 60-second deadline. A silent host requests a forced watchdog
  bite, which returns the device to Fastboot.
- The host-deadman path does not run from EP0, DWC3 initialization, or
  `SET_CONTROL_LINE_STATE`.

## Root cause

`picasso_usb_counter()` originally combined `isb` and `mrs cntvct_el0` in one
inline-asm template. This toolchain silently dropped the `mrs`, leaving the
counter at zero, so every host deadline was computed from zero and never
expired. The counter now uses the same split-asm pattern already documented in
ArmCpu.

## Hardware observations

| Image | Single variable | Observation | Result |
| --- | --- | --- | --- |
| m16f | m16c kernel + DarwinBoot APSS watchdog | CDC status worked; normal boot did not reset early | pass |
| m16g | `picasso_usb=0` | no CDC; real Fastboot at 56 s | pass |
| m16n | timer-tick host deadman + counter fix | status reported `now=1872045982`, `deadline=3024045797`; Fastboot 57 s after silence | pass |
| m16p | final normal control plane | 60-second silence returned to real Fastboot in 56 s | pass |
| m16q | final kernel with `picasso_usb=1` | no CDC; 120-second boot grace returned to Fastboot at 144 s | pass |

## Final artifacts

- Normal: `out/picasso/boot/m16p-final-control-plane.img`
  - SHA-256 `e62d1e02aef84b66e774517b6eabd7924d64b470c74a44703c3f9b3650e7770e`
- No-CDC boot grace: `out/picasso/boot/m16q-boot-grace-no-cdc.img`
  - SHA-256 `84ad30ac4c14a67d86b5d3e2d1040f6d2323572c471ad6473cdd2b37462c7819`
- Kernel:
  - SHA-256 `5ebebfe5b73e71adeabb3c442a71dcaeeaf17b176be19f1aceeef4839a86fc7b`
- DarwinBoot:
  - SHA-256 `218c6fc5d45fd73989bdc78b3fa2d33d470d522f7b729c7cad0039e08e2389fe`

## Host rule

Send a CDC command at least once per 60 seconds while the console is open.
`ping` is sufficient. The device deliberately returns to Fastboot after host
silence instead of becoming uncontrollable.
