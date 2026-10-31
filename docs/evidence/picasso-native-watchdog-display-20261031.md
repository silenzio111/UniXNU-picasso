# Native watchdog and CPU/GOP display integration

## Result

This branch combines the timer/watchdog/AES work from
`pr/picasso-boot-timer-dispatch` with the owned CPU/GOP display path and an
8 GiB packed persistent Data seed. The image was booted with `fastboot boot`
only.

Verified on device `52cc791d`:

- `PicassoWatchdogDriver` publishes the original `IOWatchdog` policy and the
  SM7250 backend claims the APSS watchdog.
- `watchdogd` matches the service (`found=1`) and its original
  `newUserClient(type=1)` returns success.
- launchd reaches `Early boot complete. Continuing system boot.`
- `backboardd` opens the `NativeLegacyDisplay` user client successfully.
- `SpringBoard` starts and opens `AppleKeyStore` and `IOPMrootDomain`.
- The 8 GiB packed OwnerData volume mounts and the original display provider
  reports a valid `1080x2400`, 32-bit GOP mapping at `0xa0000000`.

## Images

| Item | SHA-256 |
| --- | --- |
| boot image (`watchdog-m20n.img`) | `807e418ec6405a2b6bc6035f59266f7555c3e03f6a0ef30db7ee9fdaf80a7a30` |
| kernel (`kernel.macho`) | `938cd03a590216bebaaa12454a7e4c2bbe6d74b6252a67efdee279ba264ac4bd` |
| exact payload | `12de8a38a2568c8ffacef87797e65885d519a37d3df8d47b2c11c3de56d3a55e` |
| deterministic ADT (`m20n-kernel.adt`) | `e5e6c7fb9ba5a11ea40fe76315fcfd8ac36a5b3bf394994c25f2b6ea98b74143` |
| Data seed | `73535e75603a28a003adf47bf897ec8536932587923e038f5985ccc2203b01e2` |
| trust cache | `7416ca4f8377146626bb284e2133c76bee6cb4e3ee0af5d648b186235a7323a7` |

Kernel UUID: `04201352-E42C-3488-8CE9-06769D924827`.

## Critical launch finding

The old packed-Data experiment reused `/tmp/m20k-kernel.adt`, which did not
contain `/arm-io/intc/virtual-timer-intid`. The native timer therefore selected
its fallback INTID 27 instead of Picasso's INTID 19. The USB watchdog could be
armed, but once the native watchdog policy claimed hardware ownership, no live
timer pet occurred and the board reset after roughly 15-30 seconds.

`make-picasso-adt.py` now accepts an exact `--random-seed HEX`. The verified ADT
reuses the original m16p random seed and adds the qualified virtual timer INTID
19 property. This keeps the packed-handoff seed stable while repairing timer
delivery.

## Relevant evidence

The complete capture is ignored by Git and retained at:

`picasso-diagnostics/20261031-stage7-watchdog-integration/hardware-cdc-singlefd.log`

Key lines:

```text
ArmCpu: virtual timer INTID=19 (ADT selection, IRQ delivery not yet verified)
picasso-watchdog: original IOWatchdog policy published, size=248; SM7250 owner active, hardware=15s policy=600/90s poll=1s
com.apple.xpc.launchd ... Early boot complete. Continuing system boot.
PICASSO_WATCHDOG match sample=1 pid=53 ... kr=0 found=1
picasso-watchdog: original newUserClient type=1 result=00000000 accepted=1
PICASSO_IOKIT ... pid=58 name=backboardd ... detail=NativeLegacyDisplay
PICASSO_DISPLAY event=4 sample=1 a=0 b=1 c=0 failure=0
PICASSO_IOKIT ... pid=27 name=SpringBoard ...
```

## Current blocker

There is no slot-189 IOSurface panic, no watchdogd SIGTRAP, and no ENOSPC in
this run. The next boundary is the userspace graphics stack:

- `backboardd` repeatedly finds no `AGXAccelerator` or `IOAcceleratorES` service.
- It also cannot find `AppleM2ScalerCSCDriver`.
- No `PICASSO_DISPLAY event=6/7` swap-validation or CPU-copy event has appeared,
  so the display client has opened but has not submitted a frame.
- `SpringBoard` remains running but is querying AppleKeyStore and power-domain
  services.
- `audiomxd`, `CommCenter`, and `mediaserverd` are killed by the original
  userspace watchdog health checks; this is independent of the display class.

The next experiment should trace IOMobileFramebuffer selectors 4/5/72 and decide
whether the missing frame is caused by an absent accelerators/front-board
dependency or by a request-format mismatch. Adding a fake GPU success is not a
valid next step.

## Microtime repair

The 19.2 MHz timebase exposed a second independent defect: XNU divided the
subsecond remainder by the truncated integer divisor 19. This moved microtime
backwards at each one-second boundary and produced negative APFS transaction
durations.

The follow-up image adds only `patches/xnu/picasso-timebase-microtime.patch`,
which scales the remainder by `USEC_PER_SEC` and divides by the real second
divisor. Timer INTID, watchdog deadlines, clock frequency and calendar
adjustment are unchanged.

| Item | SHA-256 |
| --- | --- |
| microtime boot image (`watchdog-microtime.img`) | `ff3144f6098bb05a5fdfb5c3f7f7a8da4ba11ecd6c16b8a23484cf8afcb16290` |
| microtime payload | `3be26410fd3efcdb88c439770de93a7bf8547e612b70a3ea2c9a363ad8aefdf3` |
| microtime kernel | `8ec6f3c6c84d3760163fda24eebc58eccf220291e07f12fb9232656772292e0a` |

Kernel UUID: `47F3E939-9B1F-305F-8C08-B7F852C012AA`.

The device again reaches launchd early boot, native IOWatchdog matching,
backboardd display-client open, and SpringBoard. The capture no longer contains
negative `tx took` values or `attrlistbulk_iterator` time underflow. The
Framebuffer method trace is still not enabled, and no swap-validation or
CPU-copy event has appeared; CPU-pixel presentation therefore remains
unqualified on this combined image.
