# Picasso display recovery matrix, 2026-10-31

| ID | Hypothesis | Single variable | Result | Next action |
| --- | --- | --- | --- | --- |
| H1 | Packed Data fails because the seed is too large | 64 MiB, 2 GiB and 8 GiB packed seeds | Rejected: all stop before CDC, including a zero-extent 64 MiB seed | Treat packed handoff independently of size |
| H2 | Packed Data itself reaches launchd slowly | 300 s CDC-ready grace | Rejected: no CDC appeared; device returned to Fastboot | Continue with raw 64 MiB Data for display work |
| H3 | `offline-mobile-graphics-load=1` is safe before CDC | Raw Data with graphics load | Rejected: source-relinked kernels stop before CDC | Keep graphics startup behind the CDC command sequence |
| H4 | `register-display` fails only because graphics was not loaded | `start-graphics` followed by `register-display` | Confirmed: meta is valid, size 6416, super `IOMobileFramebufferLegacy` | Continue to provider publication |
| H5 | `register-display` causes the slot 189 panic directly | Immediate and delayed registration | Rejected tactically, confirmed root cause: no panic in the call, but later duplicate sysctl registration | Remove `postModLoad` from the prelinked-class path |
| H6 | The owned provider can attach and start | `publish-display-start` after registration | Confirmed: GOP `1080x2400`, stride 4320, nub/display attach+start, result 0 | Preserve this path while fixing static initialization |
| H7 | `postModLoad` can stay if registration is delayed | Ten `status` heartbeats before register | Rejected: same slot 189 panic | Use direct C++ static initialization |
| H8 | A byte-patched `m16p` kernel can validate the direct initializer | Two-instruction patch in `native_register_display_and_report` | Rejected: modified prebuilt kernel stops before CDC | Do not validate by binary-patching the prelinked kernel |
| H9 | A current source relink with direct initialization boots to CDC | Direct initializer, no early graphics load | Rejected: current source relink still stops before CDC | Keep the known-good m16p kernel while repairing source-build hygiene |
| H10 | Slot 189 is caused by a manually created `IOSurfaceRoot` | Preserve m16p except for the manual-root creation branch | Confirmed: m20f skips the panic, m20g finds the normal root and publishes the provider | Never create `IOSurfaceRoot`; defer display commands until IOBSD publishes `IOCoreSurfaceRoot` |
| H11 | The remaining panic is a display regression | Same m20g kernel and display provider | Rejected: OwnerData reaches ENOSPC with 282 free blocks; `watchdogd` then exits and panics | Move to persistent/large Data storage |
| H12 | Packed 8 GiB Data is the packed-handoff failure | Exact m16p ADT with only the map key changed to `PackedRAMDisk` | Rejected: m20k reaches CDC and userland with the 64 MiB packed seed | Preserve the original ADT random seed; do not combine packed and ADT-generation changes |
| H13 | Persistent 8 GiB OwnerData removes the storage failure | Same m20f kernel/display fix, exact ADT, 8 GiB seed, `sheng-data-persist=2` | Confirmed: disk1s1 is 8 GiB with 2,090,155 free blocks; display client enters; no ENOSPC or slot 189 | Continue with the upstream native watchdog/AES work |
| H14 | `watchdogd` exit is caused by Data exhaustion | Same m20m 8 GiB boot | Rejected: `watchdogd[97]` still exits with namespace 2 subcode 5 and no ENOSPC | Port the qualified native watchdog service and boot-stage policy |

## Known-good baseline

```text
image   out/picasso/boot/m16p-final-control-plane.img
sha256  e62d1e02aef84b66e774517b6eabd7924d64b470c74a44703c3f9b3650e7770e
data    owner-data-64.cdr
sha256  1c7438fb4cf5acc4869bd44d3e16b07ed57b7ab4e5fab710180c08e6b78477a2
```

The baseline reaches launchd and repeatedly starts SpringBoard. SpringBoard
exits with status 5 because no framebuffer provider exists. The three CDC
display commands successfully publish the GOP-backed Legacy provider, but the
old `postModLoad` path later panics on sysctl slot 189.

## Guard rails

- Only `fastboot boot` is used.
- Every boot has a host heartbeat and the established watchdog/Fastboot return.
- No partition is flashed, erased or formatted.
- A source relink is not considered bootable merely because it links; CDC must
  enumerate and `status` must return before another experiment is stacked.
