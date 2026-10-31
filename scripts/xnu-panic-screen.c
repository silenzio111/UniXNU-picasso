/* Draw a kernel panic message, with its arguments, on the frame buffer, then stop.
 *
 * Built freestanding by patch-xnu-entry-probe.py and placed in unused padding
 * of a COPY of the kernel; panic() and its common panic funnel are redirected
 * here. It runs on the kernel's
 * own mapping of the frame buffer. With the board observer it uses the retained
 * GOP mapping prefix; otherwise it uses vinfo.v_baseaddr.
 *
 * panic(fmt, ...) is variadic and the Darwin arm64 ABI passes every variadic
 * argument in an 8-byte stack slot, starting at the stack pointer on entry.
 * A small printf walks the format string over those slots (%s %x %p, with
 * 0-flag, width, precision and l/ll/z). Kernel strings are checked with an
 * active hardware translation query before reading each byte;
 * unmapped/device/user pointers print as pointers. VINFO names that mapping prefix.
 */
typedef unsigned long u64;
typedef unsigned int u32;
typedef unsigned char u8;

struct vc_info {
  u32 v_height, v_width, v_depth, v_rowbytes;
  u64 v_baseaddr;
};

#define SCALE   3

extern const u64 gFont[];

/* All state lives on the stack: this code sits in the kernel's read-only text,
 * so a writable global would fault on its first store. */
struct ctx {
  u8  *base;
  u32 rb, w, h, x, y;
  u8  *log;   /* kernel buffer that also receives the formatted message (PANIC_TEXT), 0 = none */
  u32 logn;
};

#ifndef PANIC_TEXT
#define PANIC_TEXT 0UL
#endif
#define PANIC_TEXT_BYTES 256

/* Resolved from the kernel's symbol table by the patcher; 0 disables the part. */
#ifndef PE_STATE
#define PE_STATE 0UL
#endif
#ifndef GPRAM
#define GPRAM 0UL
#endif
/* CDC mirror: both panic entries branch here directly, bypassing the C panic
 * hooks, so the renderer hands its text to the USB console itself before it
 * parks. Addresses resolved from the kernel symbol table by the patcher;
 * 0 disables the part. */
#ifndef USB_PANIC_BEGIN
#define USB_PANIC_BEGIN 0UL
#endif
#ifndef USB_PANIC_WRITE
#define USB_PANIC_WRITE 0UL
#endif
#ifndef USB_PANIC_RESET
#define USB_PANIC_RESET 0UL
#endif

__attribute__((noreturn)) static void park (void)
{
  for (;;) {
    __asm__ volatile ("wfe");
  }
}

static void put (struct ctx *g, u8 c)
{
  const u32 cell = 6 * SCALE, line = 14 * SCALE;
  u32 row, col, dy, dx;

  if (g->log && g->logn < PANIC_TEXT_BYTES - 1 && c >= 9 && c <= 126) {
    g->log[g->logn++] = (c < 32) ? ' ' : c;
    g->log[g->logn] = 0;
  }

  if (c == '\n') {
    g->x = 0;
    g->y += line;
    return;
  }
  if (c == '\t') {
    g->x += 4 * cell;
    return;
  }
  if (c < 32 || c > 126) {
    return;
  }
  if (g->x + cell > g->w) {
    g->x = 0;
    g->y += line;
  }
  if (g->y + line > g->h) {
    return;
  }
  u64 glyph = gFont[c - 32];
  u32 top = (u32)(glyph >> 32), bottom = (u32)glyph;
  for (row = 0; row < 12; row++) {
    u32 data = (row < 6) ? (top >> (row * 5)) : (bottom >> ((row - 6) * 5));
    for (col = 0; col < 5; col++) {
      if ((data >> col) & 1) {
        for (dy = 0; dy < SCALE; dy++) {
          for (dx = 0; dx < SCALE; dx++) {
            *(volatile u32 *)(g->base + (u64)(g->y + row * SCALE + dy) * g->rb + (u64)(g->x + col * SCALE + dx) * 4) = 0xFFFFFFFFu;
          }
        }
      }
    }
  }
  g->x += cell;
}

static void number (struct ctx *g, u64 value, u32 base, u32 width, int zero)
{
  u8  digits[24];
  u32 n = 0, i;

  do {
    u32 d = (u32)(value % base);
    digits[n++] = (u8)(d < 10 ? '0' + d : 'a' + d - 10);
    value /= base;
  } while (value && n < 24);
  for (i = n; i < width; i++) {
    put (g, zero ? '0' : ' ');
  }
  while (n) {
    put (g, digits[--n]);
  }
}

/* Probe each string byte before dereferencing it. Heap strings (process name
 * and exit reason) live outside the linked image. AT performs a hardware
 * translation query only: no table edits, mappings, or exception bypass.
 * Refuse user pointers, translation faults and device-memory attributes. */
static int readable (const char *s, const u64 *args)
{
  u64 a = (u64)s, par;
  (void)args;
  if (a < 0xffff000000000000UL) return 0;
  __asm__ volatile ("at s1e1r, %1; isb; mrs %0, par_el1" : "=r" (par) : "r" (a) : "memory");
  return !(par & 1) && ((par >> 60) != 0);
}

static void format (struct ctx *g, const char *fmt, const u64 *args)
{
  u32 i, used = 0;

  for (i = 0; i < 2000 && fmt[i]; i++) {
    u8 c = (u8)fmt[i];
    if (c != '%') {
      put (g, c);
      continue;
    }
    u32 width = 0;
    int zero = 0, longs = 0;
    c = (u8)fmt[++i];
    if (c == '0') {
      zero = 1;
      c = (u8)fmt[++i];
    }
    while (c >= '0' && c <= '9') {
      width = width * 10 + (c - '0');
      c = (u8)fmt[++i];
    }
    u32 precision = 4000;
    if (c == '.') {
      precision = 0;
      c = (u8)fmt[++i];
      if (c == '*') {
        precision = (u32)args[used++];
        c = (u8)fmt[++i];
      } else {
        while (c >= '0' && c <= '9') {
          if (precision < 4000) precision = precision * 10 + (c - '0');
          c = (u8)fmt[++i];
        }
      }
      if (precision > 4000) precision = 4000;
    }
    if (width > 256) width = 256;
    while (c == 'l' || c == 'z' || c == 'h') {
      longs += (c != 'h');
      c = (u8)fmt[++i];
    }
    if (c == '%') {
      put (g, '%');
      continue;
    }
    if (c == 0) {
      break;
    }
    u64 v = args[used++];
    if (c == 's') {
      const char *s = (const char *)v;
      u32 k;
      for (k = 0; k < precision; k++) {
        if (!readable (s + k, args)) {
          put (g, '<');
          number (g, (u64)(s + k), 16, 0, 0);
          put (g, '>');
          break;
        }
        u8 ch = (u8)s[k];
        if (!ch) break;
        put (g, ch);
      }
    } else if (c == 'x' || c == 'X') {
      number (g, longs ? v : (v & 0xFFFFFFFFu), 16, width, zero);
    } else if (c == 'u' || c == 'd') {
      number (g, longs ? v : (v & 0xFFFFFFFFu), 10, width, zero);
    } else if (c == 'p') {
      put (g, '0');
      put (g, 'x');
      number (g, v, 16, width, zero);
    }
  }
}

static void text (struct ctx *g, const char *s)
{
  while (*s) {
    put (g, (u8)*s++);
  }
}

static void hex (struct ctx *g, const char *label, u64 v)
{
  text (g, label);
  put (g, '0');
  put (g, 'x');
  number (g, v, 16, 16, 1);
}

/* x0 = fmt as for panic(); x1 is set by the stub to the entry stack pointer;
 * x2 carries the stub's own return address (x30 at the panic entry), i.e. the
 * call site that panicked -- the single most useful word when the caller graph
 * is in question. */
__attribute__((noreturn, used, section(".text.b_main"))) void panic_screen_main (const char *fmt, const u64 *args, u64 lr)
{
  volatile struct vc_info *v = (volatile struct vc_info *)VINFO;
  struct ctx g;
  u32 row, col;
  u8 keep[PANIC_TEXT_BYTES];

  __asm__ volatile ("msr daifset, #15");
  g.base = (u8 *)v->v_baseaddr;
  g.rb   = v->v_rowbytes;
  g.w    = v->v_width;
  g.h    = v->v_height;
  if (g.base == 0 || v->v_depth != 32 || fmt == 0) {
    park ();
  }

  /* First panic wins: a marker in the last row's first two pixels means a panic already drew
   * the screen; a later panic (another thread or exception) must not overwrite it. */
  volatile u32 *mark = (volatile u32 *)(g.base + (u64)(g.h - 1) * g.rb);
  if (mark[0] == 0xC0FFEE11u && mark[1] == 0x5CEEDA7Au) {
    park ();
  }
  /* Arm the CDC panic path first: the C hooks never run (both panic entries
   * branch here directly), so the renderer owns the USB handoff. */
  if (USB_PANIC_BEGIN) {
    ((void (*)(void))USB_PANIC_BEGIN)();
  }
  g.x = 0;
  g.y = g.h / 8;
  g.log = 0;
  g.logn = 0;
  for (row = g.y; row < g.h; row++) {
    for (col = 0; col < g.w; col++) {
      *(volatile u32 *)(g.base + (u64)row * g.rb + (u64)col * 4) = 0;
    }
  }
  g.y += 8;
  /* The board's live banner keeps showing this text after the next swap paints over the screen (BootProgress.c,
   * boot_progress_panic_text). The first panic's text is kept: a buffer that already holds one is left alone. */
  if (PANIC_TEXT && *(volatile u8 *)PANIC_TEXT == 0) {
    g.log = (u8 *)PANIC_TEXT;
  }
  if (!g.log) {
    /* No banner buffer on this board: keep the text on the stack so it can be
     * mirrored into the persistent panic region below. */
    keep[0] = 0;
    g.log = keep;
  }
  format (&g, fmt, args);
  /* The return address into whoever branched to the hooked panic entry: identifies
   * the panic call site even when the static call graph cannot (orphaned code,
   * wild indirect jumps). Mapped back to a symbol offline with llvm-nm. */
  hex (&g, "\nlr=", lr);
  /* Whether PE_state really reads as initialized at panic time; 0 here with a
   * message blaming PE_state means the global was wiped after PE_init_platform. */
  if (PE_STATE) {
    hex (&g, "\nPEini=", *(volatile const u32 *)PE_STATE);
  }
  /* The framebuffer mapping the renderer itself is using. */
  hex (&g, "\nfb=", (u64)v->v_baseaddr);
  /* Mirror the message into the persistent panic region (gPanicBase, mapped by
   * check_for_panic_log over the ADT /pram range) so it survives the reset and
   * can be read back with scripts/dump-picasso-panic.py --paddr <pram>. */
  if (GPRAM) {
    u64 pb = *(volatile u64 *)GPRAM;
    if (pb && g.log == keep) {
      volatile u32 *h = (volatile u32 *)pb;
      h[0] = 0x46554E4Bu;   /* eph_magic 'FUNK' */
      h[1] = 0;             /* eph_crc */
      h[2] = 4;             /* eph_version */
      h[3] = 0;             /* eph_panic_flags (u64) */
      h[4] = 0;
      h[5] = 0x100;         /* eph_panic_log_offset */
      h[6] = g.logn;        /* eph_panic_log_len */
      h[7] = 0;
      h[8] = 0;
      h[9] = 0;
      for (col = 0; col < g.logn; col++) {
        *(volatile u8 *)(pb + 0x100 + col) = keep[col];
      }
      *(volatile u8 *)(pb + 0x100 + g.logn) = 0;
    }
  }
  /* Mirror the captured text to CDC, drain it, then take the configured reset
   * path so the board returns to Fastboot on its own instead of waiting for a
   * watchdog bite. Each part compiles out when its symbol is missing. */
  /* Zero-length writes no-op inside picasso_usb_panic_write, so no logn test. */
  if (USB_PANIC_WRITE) {
    ((void (*)(const char *, int))USB_PANIC_WRITE)((const char *)g.log, (int)g.logn);
  }
  /* No standalone flush: picasso_usb_panic_reset() drains the ring itself
   * before it arms the Fastboot reason and PSHOLD-resets. */
  if (USB_PANIC_RESET) {
    ((void (*)(void))USB_PANIC_RESET)();
  }
  mark[0] = 0xC0FFEE11u;
  mark[1] = 0x5CEEDA7Au;
  __asm__ volatile ("dsb sy");
  park ();
}

/* Entry from panic(): keep the stack pointer, which addresses the variadic
 * arguments; x30 here is the return address of the panic() call itself. */
__attribute__((naked, noreturn, section(".text.a_entry"))) void panic_screen (const char *fmt)
{
  __asm__ volatile ("mov x1, sp\n\tmov x2, x30\n\tb panic_screen_main");
}

/* The real panic_trap_to_debugger signature takes (fmt, va_list *, ...).
 * Darwin ARM64 va_list is a stack pointer; its address is passed in x1.
 * The wrapper has already established its frame, so SP is NOT the argument
 * list here. Follow exactly the one pointer supplied by the real wrapper.
 */
__attribute__((naked, noreturn, section(".text.a_common"))) void panic_screen_common (void)
{
  __asm__ volatile ("ldr x1, [x1]\n\tmov x2, x30\n\tb panic_screen_main");
}

#include "panic-screen-font.h"
__attribute__((section(".text.z_font"))) const u64 gFont[] = GLYPH_FONT;
