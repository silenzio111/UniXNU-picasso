// SPDX-License-Identifier: BSD-2-Clause-Patent
// Only this synthetic QEMU payload uses PL011 MMIO after ExitBootServices.
#ifndef FIXTURE_WINDOW_SIZE
#define FIXTURE_WINDOW_SIZE 0x10000000
#endif
#ifndef FIXTURE_ADT_EXTRA
#define FIXTURE_ADT_EXTRA 0
#endif
.section .text, "ax"
.global _start
_start:
  mrs x1, sctlr_el1
  mov x2, #0x1005
  tst x1, x2
  b.ne failed
  ldr w1, [x0]
  mov w2, #0x20002
  cmp w1, w2
  b.ne failed
  ldr x3, [x0, #8]
  ldr x4, =0xfffffff007004000
  cmp x3, x4
  b.ne failed
  ldr x5, [x0, #16]
  eor x6, x3, x5
  tst x6, #0x1fffff
  b.ne failed
  adr x6, _start
  sub x6, x6, x5
  cmp x6, #0x1000
  b.ne failed
  ldr x6, [x0, #24]
  mov x7, #FIXTURE_WINDOW_SIZE
  cmp x6, x7
  b.ne failed
  ldr x6, [x0, #0x478]
  cmp x6, x7
  b.ne failed
  add x7, x5, x7
  ldr x6, [x0, #32]
  cmp x6, x7
  b.hi failed
  cmp x6, x0
  b.ls failed
#if FIXTURE_GOP
  ldp x8, x9, [x0, #0x28]
  cbz x8, failed
  cmp x9, #1
  b.ne failed
  ldp x8, x9, [x0, #0x38]
  cbz x9, failed
  cmp x8, x9, lsl #2
  b.lo failed
  ldp x8, x9, [x0, #0x48]
  cbz x8, failed
  cmp x9, #32
  b.ne failed
#else
  ldp x8, x9, [x0, #0x28]
  orr x8, x8, x9
  cbnz x8, failed
  ldp x8, x9, [x0, #0x38]
  orr x8, x8, x9
  cbnz x8, failed
  ldp x8, x9, [x0, #0x48]
  orr x8, x8, x9
  cbnz x8, failed
#endif
  ldr x6, [x0, #0x60]
  sub x6, x6, x3
  add x6, x6, x5
  ldr w7, [x6]
  cmp w7, #1
  b.ne failed
  ldr w7, [x6, #4]
  cmp w7, #1
  b.ne failed
  ldr w7, [x6, #40]
  cmp w7, #12
  b.ne failed
  // Actual owned RAMDisk address/rounded size patched into the nested ADT.
  add x14, x6, #200 + FIXTURE_ADT_EXTRA
  ldp w9, w15, [x14]
  orr x9, x9, x15, lsl #32
  ldp w10, w15, [x14, #8]
  orr x10, x10, x15, lsl #32
  cmp x10, #0x2000
  b.ne failed
  tst x9, #0xfff
  b.ne failed
  cmp x9, x5
  b.ls failed
  add x10, x9, x10
  ldr x11, [x0, #32]
  cmp x10, x11
  b.hi failed
  ldr x10, [x9]
  ldr x11, =0x4b5349444d415244
  cmp x10, x11
  b.ne failed
  add x9, x9, #0x1000
  ldrb w10, [x9]
  cmp w10, #0xa5
  b.ne failed
  ldrb w10, [x9, #1]
  cbnz w10, failed
  ldrb w10, [x9, #4095]
  cbnz w10, failed
  // Independent owned TrustCache binding: exact 56 bytes and zero padding.
  add x14, x6, #252 + FIXTURE_ADT_EXTRA
  ldp w12, w15, [x14]
  orr x12, x12, x15, lsl #32
  ldp w13, w15, [x14, #8]
  orr x13, x13, x15, lsl #32
  cmp x13, #56
  b.ne failed
  tst x12, #0xfff
  b.ne failed
  add x14, x9, #0x1000
  cmp x12, x14
  b.lo failed
  add x14, x12, #0x1000
  ldr x15, [x0, #32]
  cmp x14, x15
  b.hi failed
  ldp w13, w14, [x12]
  cmp w13, #1
  b.ne failed
  cmp w14, #8
  b.ne failed
  ldr w13, [x12, #8]
  cmp w13, #2
  b.ne failed
  ldr w13, [x12, #28]
  cmp w13, #1
  b.ne failed
  ldrb w13, [x12, #32]
  cmp w13, #0x11
  b.ne failed
  ldrb w13, [x12, #52]
  cmp w13, #2
  b.ne failed
  ldrb w13, [x12, #56]
  cbnz w13, failed
  ldrb w13, [x12, #4095]
  cbnz w13, failed
  add x6, x5, #0x4000
  mov x7, #0x200
1:
  ldr x8, [x6], #8
  cbnz x8, failed
  subs x7, x7, #1
  b.ne 1b
  adr x6, command_line
  add x7, x0, #0x6c
2:
  ldrb w8, [x6], #1
  ldrb w9, [x7], #1
  cmp w8, w9
  b.ne failed
  cbnz w8, 2b
  adr x0, passed
  b puts
failed:
  adr x0, failure
puts:
  mov x1, #0x09000000
3:
  ldrb w2, [x0], #1
  cbz w2, done
4:
  ldr w3, [x1, #0x18]
  tbnz w3, #5, 4b
  str w2, [x1]
  b 3b
done:
  wfe
  b done
command_line:
  .asciz "darwinboot.fixture=1"
passed:
#if FIXTURE_GOP
  .asciz "DARWINBOOT-FIXTURE: actual GOP boot video after ExitBootServices PASS\n"
#else
  .asciz "DARWINBOOT-FIXTURE: boot_args, RAMDisk, TrustCache, zero-fill and MMU-off entry PASS\n"
#endif
failure:
  .asciz "DARWINBOOT-FIXTURE: FAIL\n"
