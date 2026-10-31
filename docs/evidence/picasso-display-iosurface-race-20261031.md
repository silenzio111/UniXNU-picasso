# Picasso display: IOSurface ownership and deferred publication

## Result

The slot 189 panic was caused by two `IOSurfaceRoot` instances. The first was
created manually because `native_legacy_surface_ready()` ran during the early
CDC control window, before IOBSD existed. The normal `IOResources` match later
created the second instance and registered the `iosurface` sysctl again.

The fix is:

1. Never create `IOSurfaceRoot` from the display driver.
2. Preserve the complete `preModLoad` / static initializer / `postModLoad`
   sequence for `NativeLegacyDisplay`.
3. Start graphics early, but defer `register-display` and `publish-display-start`
   until IOBSD has published the normal `IOCoreSurfaceRoot`.

## Hardware evidence

| Image | Single variable | Observation | Result |
| --- | --- | --- | --- |
| m20e | m16p payload with only the 8-byte display postModLoad bypass | CDC opened at 72.6 s and status/timer checks passed | CDC control restored; direct metaclass bypass not used for the final design |
| m20f | Skip only manual `IOSurfaceRoot` creation; preserve pre/postModLoad | Publish blocked in `IOMobileFramebufferLegacy::start()` because root was not yet available | No slot 189; proves the root dependency |
| m20g | Same kernel; issue display commands after IOBSD | `event=0 ... c=1` (normal root found), provider attach/start/register all 1, `publish-display result=0` | Slot 189 eliminated; userland display client entered successfully |

The m20g log is
`picasso-diagnostics/20261031-stage7-m20g-deferred-surface/hardware-cdc.log`.

## Remaining blocker

After display publication, the 64 MiB volatile OwnerData volume filled:

```text
disk1s1 gettin' low on space ... free_blocks 282
fs_tx_enter_internal ... ENOSPC ...
watchdogd[96] exited ... subcode 0x5
panic: watchdogd exited
```

The display provider itself remains registered and the userland display client
path reports success. The new blocking issue is Data capacity, not display class
registration or IOSurface ownership.

The follow-up 8 GiB packed seed with `sheng-data-persist=2` mounts as
`disk1s1 size=0x200000000` with 2,090,155 of 2,097,152 blocks free. It removes
the ENOSPC cascade. A later `watchdogd[97]` exit with namespace 2 / subcode 5
still panics, so that service is the next independent integration target.

## Artifacts

- m20f kernel:
  `out/picasso/kernel/kernel-m16p-no-manual-surface.macho`
  SHA-256 `05ab3829d5d4e5d8ad351d0b9089a17caac1ec58ddf478606c4ab519d5972541`
- m20f boot image:
  `out/picasso/boot/m20f-no-manual-surface.img`
  SHA-256 `10f6f0e85b3cdae344d24fa86a314d897cca76eddc2435f7320ec7bed7ae6c82`

Only `fastboot boot` was used.
