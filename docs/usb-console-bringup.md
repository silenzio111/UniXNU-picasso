# Picasso USB console bring-up

The `usb` branch contains a CDC-ACM kernel console backed by the Picasso DWC3
controller. It is verified on hardware: the host receives the buffered boot log
and subsequent kernel output over USB, without a screen photo.

## Host capture

Connect the device to the Mac and check for a CDC serial port:

```sh
python3 scripts/capture-picasso-usb-console.py --list
```

Start capture before booting the test image. It can wait for the port to appear:

```sh
python3 scripts/capture-picasso-usb-console.py --wait 120 --duration 60
```

The wait and capture durations are separate. A wait timeout means no port was
opened and no kernel log was collected. The script writes a binary-safe log under `out/picasso/logs/` and
also mirrors received bytes to the terminal. Choose `--port` when more than
one matching device is connected; use `--output PATH` to select a log path.

## Verification boundary

Local verification covers patch propagation, XNU compilation, payload and
boot-image round-trip verification, and host-side capture. Hardware testing on
2026-10-25 confirmed DWC3 device mode, USB enumeration, CDC driver binding, and
kernel-log delivery. The diagnostic payload retains the placeholder platform
seed `41a551c0dec0de01`; it is not a qualified complete OS image. All phone tests
use temporary boot only.

## Hardware attempt (2026-10-25)

Two temporary `fastboot boot` attempts were made with the generated `boot.img`;
no partition was flashed, erased, or formatted. On the second attempt, the user
observed the phone reach the screen output with many USB-related messages, then
go black, reboot, show the Redmi logo, and return to Fastboot. The host did not
observe a `/dev/cu.usbmodem*` device and captured no CDC log. Fastboot again
reported `product: picasso` and `is-userspace: no` after the reboot.

A read-only `fastboot fetch minidump` attempt failed because this bootloader
does not support Fastboot `fetch`. Do not infer that the minidump is empty; it
was not read. The undocumented `fastboot oem dump` and `fastboot oem ramdump`
commands were intentionally not issued.

This confirms that the test image reaches USB-related kernel output, but does
not establish whether the reset is caused by the DWC3 path or later boot code.
The exact last visible USB log line is still needed to narrow the failure. Keep
the phone in Fastboot and do not repeat the boot until the next image has a
specific diagnostic change.

## Hold diagnostic (2026-10-25, 11:53 CST)

The complete standalone USB patch now produces a 645-line source file and matches
the nested upstream patch. A fresh output directory was used, avoiding the earlier
truncated 636-line patch and manually edited generated source. The build passed.
Literal `[NL]` strings were replaced by newlines, and the console-ring reader now
advances its position after copying bytes. Neither fix establishes the reset cause.

Image: `/tmp/picasso-usb-hold2-out/picasso/boot/boot.img`

SHA-256: `3357959b226d204bfd3882a0a6d0f319f918c1f23b6a44f39881b113abfd0688`

Arguments:

```text
-v debug=0x14e cpus=1 native-ufs-probe=12 native-ufs-diagnostics=1 io=0x232 picasso_usb=2 picasso_usb_hold=1
```

`picasso_usb_hold=1` sleeps in `bsd_init` after gadget initialization returns. It
does not stop IOKit threads already running, and it also holds if gadget setup
returns early. Thus the hold alone cannot prove successful USB setup or exclude
all UFS activity.

The first download succeeded but Bootloader returned a load/authentication error.
After `fastboot reboot bootloader`, the same image was accepted (`Booting OKAY`,
11:53:23 CST). Host USB monitoring recorded Fastboot disconnect at 11:53:26.
Through the observation ending at 11:54:59, there was no recorded CDC device or
Fastboot return. Serial waiting timed out; no kernel log was obtained. Host USB
absence cannot distinguish a held kernel, hang, black screen, or reboot without
USB. At 11:56:20 a separate `fastboot devices` check again found `52cc791d`.
The exact return time and whether it was automatic or manual were not observed.
The phone's physical screen/reset state still needs user confirmation.

No partition was flashed, erased, or formatted. Do not repeat the same image to
infer a new result. First establish the current screen state; the next diagnostic
should isolate a USB initialization stage or compare a probe-only hold.

## CDC enumeration and kernel-log capture (2026-10-25, 21:42 CST)

The USB console now works on hardware. A temporary `fastboot boot` of the hold
image produced a real CDC-ACM device on the Mac:

```text
/dev/cu.usbmodemPICASSO011
picasso XNU USB console
VID:PID 1209:0001
480 Mb/s
```

The host captured 23,191 bytes (545 lines), beginning with the Darwin kernel
banner. The log contains the SMMU guard and bind, DWC3 reset and command
completion, UTMI setup, device connection, endpoint configuration, and
`holding at bsd_init; SMMU guard PASS`. This removes the screen-photo requirement
for the kernel phase.

The last blocking defect was event consumption, not DWC3 DMA. `ud_thread` did not
run while `bsd_init` was executing synchronously, so the controller accumulated
24 bytes of events while the counters showed `events=0/24`. Moving the same
event loop into an inline `ud_poll()` and calling it from the early sample loop
consumed the connect and control events immediately. The configured bulk-IN
path then drained the complete 256 KiB console mirror.

For the next phase, the early sample loop remains synchronous. If boot is not
held, a kernel thread is started afterward to continue polling while
`bsd_init` proceeds. The hold path does not start that thread, preventing two
consumers from racing on one event queue.

Proven hold image:

```text
/Users/silenzio/Desktop/development/ios/picasso-diagnostics/20261025-usb-console/boot/boot-hold.img
SHA-256 7f4840558755ea6e883ba93961ec92f0871adaf0e964682f523ebd3b1f004f08
```

The first live-poll image was not sufficient. It handed control to a kernel
thread immediately after CDC configuration, before the host had opened the tty;
the host enumerated a CDC device but captured no log bytes. This was an event
handoff failure, not a DWC3 or SMMU failure.

The corrected live image tracks `SET_CONTROL_LINE_STATE`, keeps the synchronous
poller active until the host opens the port, drains for another 500 ms, and uses
an atomic lock so console-driven and kernel-thread polling never consume the
DWC3 event ring concurrently. Console output now drives `ud_poll()` even when
the early kernel thread is not yet schedulable.

Verified live image (2026-10-25, 22:18 CST):

```text
/tmp/picasso-usb-console-driven-20261025-2215/picasso/boot/boot.img
SHA-256 3d2d44853adb2c72db950b02cb1fc28d23423027fac3d0b033f094d0f58d1415
```

The temporary `fastboot boot` captured 25,313 bytes (603 lines), beginning with
the Darwin banner. It includes both SMMU guard passes, CDC enumeration,
`live handoff configured=1 host_open=1 after 258 ms`, and continued output after
`background poller started`. The last line was:

```text
Waiting on <dict ID="0"><key>IOProviderClass</key><string ID="1">IOMedia</string><key>Content</key><string ID="2">Apple_HFS</string></dict>
```

The repeated `NativeUfs::probe fails` and missing `Apple_HFS` provider are the
next boot blocker. USB logging is no longer on that critical path: it works with
the kernel running past `bsd_init` and does not require a screen photo or holding
the boot.

The board defaults enable the console without holding boot. For diagnostic runs
that need to park at `bsd_init`, append `picasso_usb_hold=1`.

No partition was flashed, erased, or formatted. Use `fastboot boot` only.
