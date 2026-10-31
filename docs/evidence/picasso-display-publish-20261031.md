# Picasso display publication, 2026-10-31

## Result

The existing CDC diagnostic commands can bring up the owned Legacy display
after the kernel console is available:

1. `start-graphics` loads `com.apple.iokit.IOMobileGraphicsFamily`.
2. `register-display` runs the owned `NativeLegacyDisplay` static initializer.
3. `publish-display-start` allocates, initializes, attaches and starts the
   provider.

The raw 64 MiB Data image (`m16p`) boots through launchd and SpringBoard. The
three commands all returned success on hardware. The last published scanout
parameters were:

```text
base=a0000000 width=1080 height=2400 stride=4320 depth=32
nub attach=1 start=1 dt=1 display attach=1 start=1
publish-display result=0
```

Evidence is in
[`picasso-display-publish-sysctl-panic-20261031.log`](picasso-display-publish-sysctl-panic-20261031.log).

## Remaining failure

After publication, `AppleFSCompressionTypeZlib` start panicked because the
display registration path had called `OSMetaClass::postModLoad` on the
prelinked `__kernel__` module. That repeated an existing sysctl registration:

```text
panic: attempting to register a sysctl at previously registered slot : 189
@kern_newsysctl.c:216
```

`ShengPlatformPower.cpp` now runs only
`native_legacy_display_static_init()`, which constructs
`NativeLegacyDisplay::gMetaClass`, and deliberately skips the module
`preModLoad`/`postModLoad` pair. This source change still needs a clean kernel
build and hardware boot; the current successful provider experiment used the
prebuilt `m16p` kernel.

## Reproduction baseline

- Boot image: `out/picasso/boot/m16p-final-control-plane.img`
- SHA-256: `e62d1e02aef84b66e774517b6eabd7924d64b470c74a44703c3f9b3650e7770e`
- Data seed: `UniXNU/build/owner-data-fixture/owner-data-64.cdr`
- Commands are sent as one CDC sequence, with periodic `status` commands as
  host heartbeats:

```sh
python3 scripts/picasso-cdc-command.py \
  --command start-graphics,register-display,publish-display-start,status \
  --repeat-interval 5 --duration 120 \
  --fastboot-timeout 120 --fastboot-serial 52cc791d
```

Only `fastboot boot` was used.
