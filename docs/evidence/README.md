# CDC console captures

Raw logs captured from the Picasso CDC-ACM console (`/dev/cu.usbmodemPICASSO011`)
while the diagnostic payload booted the XNU kernel. They are the first evidence
that the gadget carries the kernel log past early boot; no partition was
written, erased or formatted for them (`fastboot boot` only).

| File | Captured | Size | SHA-256 |
| --- | --- | --- | --- |
| `picasso-usb-console-20261025-214241.log` | 2026-10-25 21:44 CST | 23 KiB | `dd9d29c91fb8ca4891c8f61602ef6a3b59f84a5c026730002a4ed9318045eeb7` |
| `picasso-usb-console-20261025-221842.log` | 2026-10-25 22:19 CST | 25 KiB | `4e8597828f7d619053d8e737401aaf1774b5354746e5c1be7eb44d16fa73b6a9` |

Both start with the `Darwin Kernel Version` banner and carry the kernel's
`printf` output through `bsd_init`. The second capture reaches the APFS/VFS
stage and ends waiting for the `Apple_HFS` root device, which is the stage-6
blocker (NativeUfs root mount), not a console failure.

The first capture is dominated by `PICASSO USB: event 0x0000c046` once the
event ring is polled ahead of the kernel thread; that noise is why the thread
hand-off (`b3233c0`) exists. Raw terminal captures were taken with:

```sh
cat /dev/cu.usbmodemPICASSO011 > picasso-usb-console-$(date +%s).log
```

Reproduce with the supported script instead, which configures termios and
survives re-enumeration:

```sh
python3 scripts/capture-picasso-usb-console.py --reconnect \
  --output docs/evidence/picasso-usb-console-$(date +%Y%m%d-%H%M%S).log
```