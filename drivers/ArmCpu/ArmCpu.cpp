// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IOCPU.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSSymbol.h>
#include <machine/machine_routines.h>
#include <vm/memory_types.h>
#include <machine/memory_types.h>
#include <pexpert/pexpert.h>
#include "ArmCpuTimer.h"

#include <IOKit/IOLib.h>
#include <mach/mach_time.h>
#include <mach/clock_types.h>
#include <kern/sched_prim.h>
#include <kern/clock.h>
#include <sys/param.h>
#include <sys/time.h>
#include <sys/errno.h>

// Exact private entry from Darwin 23 arm/rtclock.h. Its full data header is
// not exported to IOKit; the IRQ path calls the real kernel implementation.
extern "C" void rtclock_intr (unsigned int);
extern "C" void picasso_usb_timer_tick (void) __attribute__((weak));
// MACH_KERNEL_PRIVATE hides this declaration from IOKit compilation units.
extern "C" vm_offset_t ml_io_map_unmappable (vm_offset_t, vm_size_t, uint32_t);
// BSD sleep used for the r41 acceptance probe. Declared directly to avoid
// pulling the full proc header into this driver.
extern "C" int msleep (void *chan, void *mtx, int pri, const char *wmesg,
    struct timespec *ts);
// Kernel-internal entries used for secondary CPU startup (Darwin 23 osfmk).
extern "C" vm_offset_t ml_static_vtop (vm_offset_t);
extern "C" char LowResetVectorBase[], LowResetVectorEnd[], ResetHandlerData[], const_boot_args[];
extern "C" kern_return_t processor_start (processor_t);
extern "C" kern_return_t kernel_thread_start (void (*) (void *, wait_result_t), void *, thread_t *);
extern "C" void thread_deallocate (thread_t);
extern "C" thread_t current_thread (void);
extern "C" kern_return_t thread_terminate (thread_t);

extern "C" {
volatile uint32_t armcpu_registered = 0;
volatile uint32_t armcpu_gic_ready = 0;
volatile uint64_t armcpu_timer_interrupts = 0;
volatile uint64_t armcpu_ipi_interrupts = 0;
volatile uint64_t armcpu_ipi_sent = 0;       // SGIs the kernel asked ArmCpu::signalCPU to send
volatile uint64_t armcpu_ipi_selftest_sent = 0;
// Boot core's own GIC registers read back once after enabling (own frame, the same one just written): 0 IGROUPR0, 1 ISENABLER0, 2 GICD_CTLR,
// 3 GICD_TYPER, 4 IGRPMODR0, 5 ICC_PMR_EL1, 6 ICC_IGRPEN1_EL1, 7 ICC_SRE_EL1.
volatile uint32_t armcpu_gic_boot[8];
volatile uint32_t armcpu_core_timer[8], armcpu_core_sgi[8];   // per logical core: architectural timer interrupts and SGI 0 taken
static bool gSpiIpi = false;                // boot-arg armcpu-spi-ipi=1: IPIs are software-pended SPIs 1008+core routed to that core (ICC_SGI1R_EL1 generates nothing on this board)
static bool gSelfIpi = false;               // boot-arg armcpu-self-ipi=1: every core sends itself SGI 0 from initCPU
volatile uint32_t armcpu_irq_phase = 0,armcpu_irq_id = 1023;
// M1 timer probe evidence (read-only until the selftest stage).
volatile uint32_t armcpu_timer_probe[40];
volatile uint32_t armcpu_timer_probe_intid = 19;
volatile uint32_t armcpu_timer_probe_enabled_mask = 0;
volatile uint32_t armcpu_quiesce_calls = 0,armcpu_halt_calls = 0;
// Secondary-CPU bring-up evidence (sheng-smp=1). Step values: 1 nub started, 2 registered, 3 start thread running,
// 4 PSCI CPU_ON issued, 5 entered initCPU on the new core, 6 GIC redistributor ready, 7 running.
volatile uint32_t armcpu_smp_requested = 0, armcpu_smp_running = 0, armcpu_smp_conduit = 0;
volatile uint32_t armcpu_smp_step[8], armcpu_smp_mpidr[8], armcpu_smp_seen_mpidr[8], armcpu_smp_fail[8], armcpu_boot_mpidr = 0;
// System registers seen on each secondary core at initCPU entry: CurrentEL, SCTLR_EL1, TCR_EL1, MAIR_EL1 (low words), MIDR_EL1.
volatile uint32_t armcpu_smp_sys[8][5], armcpu_boot_sys[5];
volatile int32_t armcpu_smp_psci_rc[8];
}

#define ARMCPU_SMP_SLOTS 8
#define ARMCPU_TIMER_DEFAULT_INTID 19

class ArmCpu : public IOCPU {
  OSDeclareDefaultStructors (ArmCpu)
  OSSharedPtr<IOMemoryMap> distributor;
  OSSharedPtr<IOMemoryMap> redistributors;
  IOCPUInterruptController *controller = nullptr;
  const OSSymbol *cpuName = nullptr;
  uintptr_t dist = 0;
  uintptr_t redist = 0;
  uint64_t affinity = 0;
  uintptr_t redistBase = 0;
  uint64_t redistSize = 0;
  bool skipGic = false;
  bool diagnosePower = false;
  bool secondary = false;
  uint32_t logicalId = 0;
  bool mapGic (void);
  bool initDistributor (void);
  bool wakeRedistributor (bool local);
  bool finishSecondary (void);
  static void drainPending (void);
  bool prepareGic (void);
  void startSecondary (void);
  static void startThread (void *argument, wait_result_t);
  void enableGic (void);
  void interrupt (void);
  static void dispatch (void *target, void *, void *, int);
public:
  IOService *probe (IOService *provider, SInt32 *score) override;
  bool start (IOService *provider) override;
  void initCPU (bool boot) override;
  void quiesceCPU (void) override;
  void haltCPU (void) override;
  kern_return_t startCPU (vm_offset_t, vm_offset_t) override;
  void signalCPU (IOCPU *target) override;
  uint64_t signalTarget (void) const { return affinity; }
  void enableCPUTimeBase (bool enabled) override;
  const OSSymbol *getCPUName (void) override;
  void timerProbe (void);
  void timerStageB (void);
  void timerStageB2Arm (void);
  void timerStageB2Sample (unsigned ms);
  void timerSelftestBegin (void);
  void timerSelftestSample (unsigned ms);
  void timerSelftestGo (void);
  void timerSelftestEnd (void);
  void timerSleepTest (void);
  void finishBootStart (void);
  void framebufferProbe (void);
};

OSDefineMetaClassAndStructors (ArmCpu, IOCPU)

// Selected by the boot CPU before GIC writes; shared with secondary nubs.
// Picasso's DTB advertises the virtual timer as PPI 19. Keep the empty ADT
// property as an explicit override, but use the board's real line by default.
static uint32_t gVirtualTimerIntid = ARMCPU_TIMER_DEFAULT_INTID;

static const uint32_t kIpiSpiBase = 1008;   // INTIDs 1008..1015 stand for 'IPI to logical core 0..7'


// r43: never combine isb and mrs in one asm template. The driver toolchain
// silently drops the mrs half (disassembly shows a lone isb; the destination
// keeps stale register garbage). All counter reads go through these split
// helpers. Verified in ArmCpu.o disassembly.
static inline uint64_t armcpu_cntvct (void)
{
  uint64_t v;
  __asm__ volatile ("isb" ::: "memory");
  __asm__ volatile ("mrs %0, cntvct_el0" : "=r" (v) :: "memory");
  return v;
}
static inline uint64_t armcpu_cntpct (void)
{
  uint64_t v;
  __asm__ volatile ("isb" ::: "memory");
  __asm__ volatile ("mrs %0, cntpct_el0" : "=r" (v) :: "memory");
  return v;
}

// r53 (single variable vs r52): keep-path IRQ sampler. r52 proved End-kept
// (daif=0x200) is still not enough: DEFER post stays 0. This thread uses
// only CNTVCT reads + yield spins (no IOSleep/deadline), so it runs even if
// the timer deadline path is broken, and reports whether armcpu_timer_
// interrupts keeps growing during the 60 s control window.
static void armcpu_keep_sampler(void *, wait_result_t)
{
  uint64_t freq = 0;
  __asm__ volatile ("mrs %0, cntfrq_el0" : "=r" (freq));
  if (freq == 0) { freq = 19200000ULL; }
  for (unsigned i = 0; i < 6; i++) {
    const uint64_t start = armcpu_cntvct();
    const uint64_t waitTicks = freq * 10ULL;
    while (armcpu_cntvct() - start < waitTicks) {
      for (unsigned k = 0; k < 1000; k++) {
        __asm__ volatile ("yield" ::: "memory");
      }
    }
    uint64_t daif = 0, ctl = 0, tval = 0;
    __asm__ volatile ("mrs %0, daif" : "=r" (daif));
    __asm__ volatile ("mrs %0, cntv_ctl_el0" : "=r" (ctl));
    __asm__ volatile ("mrs %0, cntv_tval_el0" : "=r" (tval));
    printf("PICASSO TIMER: keep sample %u irq=%llu daif=%llx ctl=%llx tval=%llx vct=%llu\n",
        i, armcpu_timer_interrupts, daif, ctl, tval, armcpu_cntvct());
  }
  printf("PICASSO TIMER: keep sampler done\n");
}

static uint32_t read32 (uintptr_t base, unsigned offset)
{
  return *reinterpret_cast<volatile uint32_t *>(base + offset);
}

static void write32 (uintptr_t base, unsigned offset, uint32_t value)
{
  *reinterpret_cast<volatile uint32_t *>(base + offset) = value;
}

// Distributor side of the software IPI of logical core `core`: group 1, edge-triggered, priority 0x80, routed to affinity `aff`, enabled.
static void spiIpiConfigure (uintptr_t dist, uint32_t core, uint64_t aff)
{
  const uint32_t id = kIpiSpiBase + core;
  write32 (dist, 0x80 + (id / 32) * 4, read32 (dist, 0x80 + (id / 32) * 4) | (1U << (id % 32)));
  write32 (dist, 0xc00 + (id / 16) * 4, read32 (dist, 0xc00 + (id / 16) * 4) | (2U << ((id % 16) * 2)));
  write32 (dist, 0x400 + (id / 4) * 4, 0x80808080);
  *reinterpret_cast<volatile uint64_t *>(dist + 0x6000 + 8ULL * id) = aff & 0xff00ffffffULL;
  __asm__ volatile ("dsb sy" ::: "memory");
  write32 (dist, 0x100 + (id / 32) * 4, 1U << (id % 32));
  __asm__ volatile ("dsb sy" ::: "memory");
}

static void spiIpiSend (uintptr_t dist, uint32_t core)
{
  const uint32_t id = kIpiSpiBase + core;
  __asm__ volatile ("dsb ishst" ::: "memory");
  write32 (dist, 0x200 + (id / 32) * 4, 1U << (id % 32));
  __asm__ volatile ("dsb sy" ::: "memory");
}

static uint64_t counter (void)
{
  uint64_t value;
  __asm__ volatile ("isb; mrs %0, cntvct_el0" : "=r" (value));
  return value;
}

static bool waitClear (uintptr_t base, unsigned offset, uint32_t mask)
{
  uint64_t frequency;
  __asm__ volatile ("mrs %0, cntfrq_el0" : "=r" (frequency));
  uint64_t started = counter ();
  while ((read32 (base, offset) & mask) != 0) {
    if (counter () - started > frequency / 10) {
      return false;
    }
  }
  return true;
}

// GIC state shared by all CPU nubs. The boot CPU maps the distributor and redistributor frames; a secondary core
// only needs the virtual addresses and its own redistributor frame.
static OSSharedPtr<IOMemoryMap> gDistMap, gRedistMap;
static uintptr_t gDist = 0, gFrames = 0;
static uint64_t gFramesSize = 0;
static IOCPUInterruptController *gController = nullptr;
static volatile uint32_t gGicShared = 0;
static uint64_t gRedistStride = 0; // DT redistributor-stride; 0 = infer from GICR_TYPER.VLPIS
volatile uint32_t armcpu_gic_stride = 0, armcpu_gic_typer_probe[2];
static bool gSmpWanted = false;
// Secondary nubs never wait for the boot CPU (IOSleep needs the timer that only the boot nub enables): they queue here and
// the boot nub, or a late secondary once the boot CPU is registered, finishes them.
static class ArmCpu *volatile gPending[ARMCPU_SMP_SLOTS];
static volatile uint32_t gBootRegistered = 0;
static class ArmCpu *volatile gBootCpu = nullptr;
// r39: set when start()'s tail (direct install + initCPU) has been finished
// exactly once, regardless of which thread runs it.
static volatile uint32_t gBootFinished = 0;
static bool gRemoteGic = false; // sheng-smp-gic=1: the boot CPU prepares a new core's redistributor before CPU_ON

// Shared peripheral interrupts (GICv3 SPIs, INTID 32..1019). A driver attaches a handler for one INTID; the line is set to
// Group 1 non-secure, priority 0xa0, the given trigger and routed to the boot CPU (GICD_IROUTER, affinity routing is on),
// then enabled. The handler runs in the primary interrupt context, before the EOI, and must clear a level source.
// An SPI without a handler is counted and disabled instead of being acknowledged forever.
#define ARMCPU_SPI_SLOTS 16
struct ArmCpuSpi {uint32_t id; void (*handler) (void *); void *context; volatile uint64_t count;};
static ArmCpuSpi gSpi[ARMCPU_SPI_SLOTS];
static volatile uint32_t gSpiUsed = 0;
static uint64_t gBootAffinity = 0;
extern "C" {
volatile uint64_t armcpu_spi_unhandled = 0;
volatile uint32_t armcpu_spi_unhandled_id = 0;
}

IOService *ArmCpu::probe (IOService *provider, SInt32 *score)
{
  OSData *type = OSDynamicCast (OSData, provider->getProperty ("device_type"));
  OSData *reg = OSDynamicCast (OSData, provider->getProperty ("reg"));
  uint32_t id;
  if ((type == nullptr) || (type->getLength () != 4) ||
      (memcmp (type->getBytesNoCopy (), "cpu", 4) != 0) ||
      (reg == nullptr) || (reg->getLength () < sizeof (id))) {
    return nullptr;
  }
  memcpy (&id, reg->getBytesNoCopy (), sizeof (id));
  if (ml_get_cpu_number (id) != ml_get_boot_cpu_number ()) {
    unsigned smp = 0;
    PE_parse_boot_argn ("sheng-smp", &smp, sizeof (smp));
    if (smp == 0 || ml_get_cpu_number (id) < 0 || ml_get_cpu_number (id) >= ARMCPU_SMP_SLOTS) {
      return nullptr; // Secondary startup is opt-in (sheng-smp=1).
    }
  }
  return IOCPU::probe (provider, score);
}

bool ArmCpu::mapGic (void)
{
  IORegistryEntry *soc = IORegistryEntry::fromPath ("/arm-io", gIODTPlane);
  IORegistryEntry *gic = IORegistryEntry::fromPath ("/arm-io/intc", gIODTPlane);
  if ((soc == nullptr) || (gic == nullptr)) {
    if (soc != nullptr) { soc->release (); }
    if (gic != nullptr) { gic->release (); }
    return false;
  }
  OSData *ranges = OSDynamicCast (OSData, soc->getProperty ("ranges"));
  OSData *reg = OSDynamicCast (OSData, gic->getProperty ("reg"));
  OSData *compatible = OSDynamicCast (OSData, gic->getProperty ("compatible"));
  uint64_t bus[3];
  uint64_t regions[4];
  bool valid = (ranges != nullptr) && (ranges->getLength () == sizeof (bus)) &&
               (reg != nullptr) && (reg->getLength () == sizeof (regions)) &&
               (compatible != nullptr) && (compatible->getLength () == 11) &&
               (memcmp (compatible->getBytesNoCopy (), "arm,gic-v3", 11) == 0);
  if (valid) {
    memcpy (bus, ranges->getBytesNoCopy (), sizeof (bus));
    memcpy (regions, reg->getBytesNoCopy (), sizeof (regions));
  }
  OSObject *timerObject = gic->getProperty ("virtual-timer-intid");
  OSData *timerProperty = OSDynamicCast (OSData, timerObject);
  uint32_t timerIntid;
  if ((timerObject != nullptr && timerProperty == nullptr) ||
      !ArmCpuTimerIntid (timerProperty ? timerProperty->getBytesNoCopy () : nullptr,
                        timerProperty ? timerProperty->getLength () : 0, &timerIntid)) {
    soc->release ();
    gic->release ();
    IOLog ("ArmCpu: virtual-timer-intid must be a little-endian u32 PPI (16..31)\n");
    return false;
  }
  OSData *strideProperty = OSDynamicCast (OSData, gic->getProperty ("redistributor-stride"));
  if (strideProperty != nullptr && strideProperty->getLength () == sizeof (uint64_t)) {
    memcpy (&gRedistStride, strideProperty->getBytesNoCopy (), sizeof (uint64_t));
    if (gRedistStride != 0x20000 && gRedistStride != 0x30000 && gRedistStride != 0x40000) { gRedistStride = 0; }
  }
  armcpu_gic_stride = static_cast<uint32_t> (gRedistStride);
  soc->release ();
  gic->release ();
  if (!valid || (regions[1] < 0x10000) || (regions[3] < 0x20000)) {
    return false;
  }
  gVirtualTimerIntid = timerIntid;
  for (unsigned index = 0; index < 4; index += 2) {
    if ((regions[index] < bus[0]) || (regions[index] - bus[0] > bus[2]) ||
        (regions[index + 1] > bus[2] - (regions[index] - bus[0])) ||
        (bus[1] + regions[index] - bus[0] < bus[1])) {
      return false;
    }
  }
  OSSharedPtr<IOMemoryDescriptor> distMemory (IOMemoryDescriptor::withPhysicalAddress (
      bus[1] + regions[0] - bus[0], regions[1], kIODirectionInOut), OSNoRetain);
  OSSharedPtr<IOMemoryDescriptor> rdMemory (IOMemoryDescriptor::withPhysicalAddress (
      bus[1] + regions[2] - bus[0], regions[3], kIODirectionInOut), OSNoRetain);
  if (!distMemory || !rdMemory) {
    return false;
  }
  distributor = OSSharedPtr<IOMemoryMap> (distMemory->map (kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
  redistributors = OSSharedPtr<IOMemoryMap> (rdMemory->map (kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
  if (!distributor || !redistributors) {
    return false;
  }
  dist = static_cast<uintptr_t> (distributor->getVirtualAddress ());
  if (((read32 (dist, 0xffe8) >> 4) & 15) != 3) {
    return false;
  }
  redistBase = static_cast<uintptr_t> (redistributors->getVirtualAddress ());
  redistSize = regions[3];
  gDistMap = distributor;
  gRedistMap = redistributors;
  gDist = dist;
  gFrames = redistBase;
  gFramesSize = redistSize;
  return true;
}

bool ArmCpu::initDistributor (void)
{
  uint32_t ctl = read32 (dist, 0);
  write32 (dist, 0, ctl & ~3U);
  if (!waitClear (dist, 0, 1U << 31)) { return false; }
  write32 (dist, 0, (ctl & ~3U) | 16U);
  if (!waitClear (dist, 0, 1U << 31)) { return false; }
  unsigned interrupts = ((read32 (dist, 4) & 31) + 1) * 32;
  for (unsigned id = 32; id < interrupts && id < 1020; id += 32) {
    write32 (dist, 0x180 + id / 8, 0xffffffff);
  }
  return true;
}

// Runs on the core whose redistributor is configured: the frame is selected by that core's own MPIDR affinity.
bool ArmCpu::wakeRedistributor (bool local)
{
  uintptr_t frames = redistBase;
  // local: this core finds its own frame from MPIDR. Remote: the boot CPU prepares a not-yet-started core's frame
  // from the DT affinity, so the new core never needs GIC MMIO of its own.
  if (local) { __asm__ volatile ("mrs %0, mpidr_el1" : "=r" (affinity)); }
  uint32_t target = static_cast<uint32_t> ((affinity & 0xffffff) | ((affinity >> 8) & 0xff000000));
  redist = 0;
  for (uint64_t offset = 0; offset + 0x20000 <= redistSize;) {
    armcpu_gic_typer_probe[0] = static_cast<uint32_t> (offset);
    uint64_t typer = *reinterpret_cast<volatile uint64_t *> (frames + offset + 8);
    armcpu_gic_typer_probe[1] = static_cast<uint32_t> (typer);
    if (static_cast<uint32_t> (typer >> 32) == target) {
      redist = frames + offset;
      break;
    }
    if ((typer & 16) != 0) { break; }
    offset += gRedistStride != 0 ? gRedistStride : ((typer & 2) != 0 ? 0x40000 : 0x20000);
  }
  if (redist == 0) { return false; }
  write32 (redist, 0x14, read32 (redist, 0x14) & ~2U);
  if (!waitClear (redist, 0x14, 4)) { return false; }
  uintptr_t sgi = redist + 0x10000;
  write32 (sgi, 0x180, 0xffffffff);
  write32 (sgi, 0x80, 0xffffffff);
  write32 (sgi, 0xd00, 0);
  write32 (sgi, 0x280, 0xffffffff);
  for (unsigned offset = 0x400; offset < 0x420; offset += 4) {
    write32 (sgi, offset, 0x80808080);
  }
  if (!local) { write32 (sgi, 0x100, ArmCpuSgiPpiEnable (gVirtualTimerIntid)); }
  __asm__ volatile ("dsb sy" ::: "memory");
  return waitClear (redist, 0, 8);
}

bool ArmCpu::prepareGic (void)
{
  if (!mapGic () || !initDistributor () || !wakeRedistributor (true)) { return false; }
  gGicShared = 1;
  return true;
}

// M2 stage A: prove that the GOP framebuffer retained in PE_Video is still
// reachable after the MMU is live. This is deliberately a boot-arg-gated,
// one-shot diagnostic: caller supplies picasso-fb-probe=1, the USB console is
// already active, and only a temporary fastboot boot is expected.
void ArmCpu::framebufferProbe (void)
{
  static bool attempted = false;
  unsigned enabled = 0;
  if (attempted ||
      !PE_parse_boot_argn ("picasso-fb-probe", &enabled, sizeof (enabled)) ||
      enabled != 1) {
    return;
  }
  attempted = true;

  PE_Video video = {};
  const int consoleError = PE_current_console (&video);
  printf ("PICASSO FB: begin console=%d base=0x%lx bytes/row=%lu width=%lu "
      "height=%lu depth=%lu rotate=%u scale=%u offset=%lu length=%lu\n",
      consoleError, video.v_baseAddr, video.v_rowBytes, video.v_width,
      video.v_height, video.v_depth, video.v_rotate, video.v_scale,
      video.v_offset, video.v_length);
  if (consoleError != 0 || video.v_baseAddr == 0 ||
      (video.v_baseAddr & 3UL) != 0 || video.v_width == 0 ||
      video.v_height == 0 || video.v_width > 16384 ||
      video.v_height > 16384 || video.v_depth != 32 ||
      video.v_rotate != 0 || video.v_offset != 0 ||
      video.v_rowBytes < video.v_width * 4UL ||
      video.v_rowBytes > UINT64_MAX / video.v_height) {
    printf ("PICASSO FB: rejected invalid GOP console metadata\n");
    return;
  }

  const uint64_t rowBytes = video.v_rowBytes;
  const uint64_t frameBytes = rowBytes * video.v_height;
  if (frameBytes == 0 || frameBytes > (256ULL << 20) ||
      (video.v_length != 0 && video.v_length < frameBytes)) {
    printf ("PICASSO FB: rejected frame bytes=%llu length=%lu\n",
        frameBytes, video.v_length);
    return;
  }

  const vm_size_t mapBytes = static_cast<vm_size_t> (
      round_page (static_cast<vm_offset_t> (frameBytes)));
  printf ("PICASSO FB: map begin phys=0x%lx bytes=%llu format='%s'\n",
      video.v_baseAddr, frameBytes, video.v_pixelFormat);
  vm_offset_t mapping = ml_io_map_unmappable (video.v_baseAddr, mapBytes,
      VM_WIMG_WCOMB);
  if (mapping == 0) {
    printf ("PICASSO FB: ml_io_map_unmappable failed\n");
    return;
  }
  printf ("PICASSO FB: mapped va=0x%lx map_bytes=%llu\n", mapping,
      static_cast<uint64_t> (mapBytes));

  volatile uint32_t *framebuffer =
      reinterpret_cast<volatile uint32_t *> (mapping);
  volatile uint32_t *last =
      reinterpret_cast<volatile uint32_t *> (mapping + frameBytes - 4);
  const uint32_t beforeFirst = framebuffer[0];
  const uint32_t beforeLast = *last;
  printf ("PICASSO FB: pixels before first=%08x last=%08x vct=%llu\n",
      beforeFirst, beforeLast, armcpu_cntvct ());

  // Eight vertical SMPTE-like bars. XRGB8888 values are intentionally pure
  // and easy to identify on a photographed screen.
  static const uint32_t bars[8] = {
    0xffffffffU, 0xffffff00U, 0xff00ffffU, 0xff00ff00U,
    0xffff00ffU, 0xffff0000U, 0xff0000ffU, 0xff101010U
  };
  const uint32_t stridePixels = static_cast<uint32_t> (rowBytes / 4);
  const uint32_t width = static_cast<uint32_t> (video.v_width);
  const uint32_t height = static_cast<uint32_t> (video.v_height);
  for (uint32_t y = 0; y < height; y++) {
    volatile uint32_t *row = framebuffer + static_cast<uint64_t> (y) *
        stridePixels;
    for (uint32_t x = 0; x < width; x++) {
      row[x] = bars[(static_cast<uint64_t> (x) * 8) / width];
    }
  }

  const uint32_t border = (width < height ? width : height) / 80 + 2;
  for (uint32_t y = 0; y < height; y++) {
    volatile uint32_t *row = framebuffer + static_cast<uint64_t> (y) *
        stridePixels;
    if (y < border || y >= height - border) {
      for (uint32_t x = 0; x < width; x++) {
        row[x] = 0xff000000U;
      }
    } else {
      for (uint32_t x = 0; x < border; x++) {
        row[x] = 0xff000000U;
        row[width - 1 - x] = 0xff000000U;
      }
    }
  }
  __asm__ volatile ("dsb sy" ::: "memory");

  const uint32_t afterFirst = framebuffer[0];
  const uint32_t afterLast = *last;
  printf ("PICASSO FB: probe complete pixels first=%08x last=%08x "
      "bars=8 border=%u vct=%llu\n", afterFirst, afterLast, border,
      armcpu_cntvct ());
}

// M1 stage A: read-only snapshot of the GIC/timer path. The USB control layer
// calls this only after CDC is live, never from the early CPU startup path.
void ArmCpu::timerProbe (void)
{
  if (redist == 0 || dist == 0) { printf ("PICASSO TIMER: no GIC mapping\n"); return; }
  uint64_t mpidr;
  __asm__ volatile ("mrs %0, mpidr_el1" : "=r" (mpidr));
  printf ("PICASSO TIMER: snapshot registered=%u gic_ready=%u boot_registered=%u "
      "cpu=%u mpidr=%llx redist=%p dist=%p irq=%llu\n",
      armcpu_registered, armcpu_gic_ready, gBootRegistered, logicalId, mpidr,
      reinterpret_cast<void *> (redist), reinterpret_cast<void *> (dist),
      armcpu_timer_interrupts);
  const uintptr_t sgi = redist + 0x10000;
  uint64_t daif, cntfrq, cntv_ctl, cntv_tval, cntvct;
  uint64_t sre, ctlr, pmr, igrp;
  __asm__ volatile ("mrs %0, daif" : "=r" (daif));
  __asm__ volatile ("mrs %0, cntfrq_el0" : "=r" (cntfrq));
  __asm__ volatile ("mrs %0, cntv_ctl_el0" : "=r" (cntv_ctl));
  __asm__ volatile ("mrs %0, cntv_tval_el0" : "=r" (cntv_tval));
  cntvct = armcpu_cntvct ();
  __asm__ volatile ("mrs %0, icc_sre_el1" : "=r" (sre));
  __asm__ volatile ("mrs %0, icc_ctlr_el1" : "=r" (ctlr));
  __asm__ volatile ("mrs %0, icc_pmr_el1" : "=r" (pmr));
  __asm__ volatile ("mrs %0, icc_igrpen1_el1" : "=r" (igrp));

  const uint32_t igroupr0 = read32 (sgi, 0x80);
  const uint32_t isenabler0 = read32 (sgi, 0x100);
  const uint32_t gicd_ctl = read32 (dist, 0);
  const uint32_t gicd_typer = read32 (dist, 4);
  const uint32_t icfgr0 = read32 (sgi, 0xc00);
  const uint32_t icfgr1 = read32 (sgi, 0xc04);
  const uint32_t waker = read32 (redist, 0x14);
  const uint32_t ispender0 = read32 (sgi, 0x200);
  const uint32_t iactive0 = read32 (sgi, 0x300);
  const uint32_t prio19 = *reinterpret_cast<volatile uint8_t *> (sgi + 0x400 + 19);
  const uint32_t prio27 = *reinterpret_cast<volatile uint8_t *> (sgi + 0x400 + 27);
  const bool en19 = (isenabler0 >> 19 & 1) != 0;
  const bool en27 = (isenabler0 >> 27 & 1) != 0;
  const bool grp19 = (igroupr0 >> 19 & 1) != 0;
  const bool grp27 = (igroupr0 >> 27 & 1) != 0;

  armcpu_timer_probe[0] = static_cast<uint32_t> (gVirtualTimerIntid);
  armcpu_timer_probe[1] = igroupr0;
  armcpu_timer_probe[2] = isenabler0;
  armcpu_timer_probe[3] = icfgr0;
  armcpu_timer_probe[4] = icfgr1;
  armcpu_timer_probe[5] = waker;
  armcpu_timer_probe[6] = gicd_ctl;
  armcpu_timer_probe[7] = gicd_typer;
  armcpu_timer_probe[8] = static_cast<uint32_t> (daif);
  armcpu_timer_probe[9] = static_cast<uint32_t> (cntfrq);
  armcpu_timer_probe[10] = static_cast<uint32_t> (cntv_ctl);
  armcpu_timer_probe[11] = static_cast<uint32_t> (cntv_tval);
  armcpu_timer_probe[12] = static_cast<uint32_t> (sre);
  armcpu_timer_probe[13] = static_cast<uint32_t> (ctlr);
  armcpu_timer_probe[14] = static_cast<uint32_t> (pmr);
  armcpu_timer_probe[15] = static_cast<uint32_t> (igrp);
  armcpu_timer_probe[16] = (prio19 << 16) | (prio27 << 24);
  armcpu_timer_probe[17] = ispender0;
  armcpu_timer_probe[18] = iactive0;
  armcpu_timer_probe_intid = gVirtualTimerIntid;
  armcpu_timer_probe_enabled_mask = (en19 ? 1U : 0U) | (en27 ? 2U : 0U) |
      (grp19 ? 4U : 0U) | (grp27 ? 8U : 0U);

  printf ("PICASSO TIMER: boot-cache group=%08x enable=%08x ctlr=%08x igrp=%08x\n",
      armcpu_gic_boot[0], armcpu_gic_boot[1], armcpu_gic_boot[2],
      armcpu_gic_boot[6]);
  printf ("PICASSO TIMER: intid=%u enable19=%u enable27=%u group19=%u group27=%u prio19=%u prio27=%u\n",
      gVirtualTimerIntid, en19 ? 1U : 0U, en27 ? 1U : 0U, grp19 ? 1U : 0U,
      grp27 ? 1U : 0U, prio19, prio27);
  printf ("PICASSO TIMER: GICR IGROUPR0=%08x ISENABLER0=%08x ISPENDR0=%08x ICFGR0=%08x ICFGR1=%08x WAKER=%08x\n",
      igroupr0, isenabler0, ispender0, icfgr0, icfgr1, waker);
  printf ("PICASSO TIMER: GICD CTLR=%08x TYPER=%08x ICC SRE=%llx CTLR=%llx PMR=%llx IGRPEN1=%llx DAIF=%llx\n",
      gicd_ctl, gicd_typer, sre, ctlr, pmr, igrp, daif);
  printf ("PICASSO TIMER: CNTFRQ=%llu CNTV_CTL=%llx CNTV_TVAL=%llx CNTVCT=%llu\n",
      cntfrq, cntv_ctl, cntv_tval, cntvct);

  printf ("PICASSO TIMER: snapshot complete (read-only)\n");
}

// M1 stage B: pending-bit routing. No GIC writes, no DAIF change, no IRQ
// enable. Re-arms the architected virtual timer for 1 s, spins ~1.2 s on a
// yield loop bounded by a CNTVCT delta plus an iteration cap (no IOSleep or
// thread_block this early), then reports which PPI pending bit follows the
// timer. Runs only under native-timer-stageb=1, after the stage-A snapshot.
void ArmCpu::timerStageB(void)
{
  if (redist == 0 || dist == 0) { printf ("PICASSO TIMER: stageB no GIC mapping\n"); return; }
  const uintptr_t sgi = redist + 0x10000;
  uint64_t cntfrq = 0, ctl0 = 0, tval0 = 0, c0v = 0, c0p = 0;
  __asm__ volatile ("mrs %0, cntfrq_el0" : "=r" (cntfrq));
  __asm__ volatile ("mrs %0, cntv_ctl_el0" : "=r" (ctl0));
  __asm__ volatile ("mrs %0, cntv_tval_el0" : "=r" (tval0));
  __asm__ volatile ("isb; mrs %0, cntvct_el0" : "=r" (c0v));
  __asm__ volatile ("isb; mrs %0, cntpct_el0" : "=r" (c0p));
  const uint32_t isp0 = read32 (sgi, 0x200);
  printf ("PICASSO TIMER: stageB begin tval=19200000 cntfrq=%llu\n", cntfrq);
  printf ("PICASSO TIMER: stageB base ctl=%llx tval=%llx ispendr=%08x bit19=%u bit27=%u cntvct=%llu cntpct=%llu\n",
      ctl0, tval0, isp0, (isp0 >> 19) & 1U, (isp0 >> 27) & 1U, c0v, c0p);
  // Keep the signal path open without touching the GIC: ENABLE=1, IMASK=0.
  // CPU-local architected timer state only.
  if ((ctl0 & 0x3ULL) != 0x1ULL) {
    const uint64_t fixed = (ctl0 & ~0x3ULL) | 0x1ULL;
    __asm__ volatile ("msr cntv_ctl_el0, %0; isb" :: "r" (fixed) : "memory");
    printf ("PICASSO TIMER: stageB ctl fixed %llx -> %llx\n", ctl0, fixed);
  }
  __asm__ volatile ("msr cntv_tval_el0, %0; isb" :: "r" (19200000ULL) : "memory");
  uint64_t ctl1 = 0, tval1 = 0;
  __asm__ volatile ("mrs %0, cntv_ctl_el0" : "=r" (ctl1));
  __asm__ volatile ("mrs %0, cntv_tval_el0" : "=r" (tval1));
  printf ("PICASSO TIMER: stageB armed ctl=%llx tval=%llx\n", ctl1, tval1);
  // Spin ~1.2 s: CNTVCT delta target with a bounded yield cap so a stopped
  // counter still terminates. Worst case is a few seconds against the 15 s
  // default watchdog (last pet was at the handoff tail, milliseconds ago).
  const uint64_t ticks = cntfrq != 0 ? cntfrq + cntfrq / 5 : 23040000ULL;
  uint64_t c1v = c0v;
  uint64_t iters = 0;
  const uint64_t cap = 200000000ULL;
  do {
    __asm__ volatile ("yield" ::: "memory");
    __asm__ volatile ("isb; mrs %0, cntvct_el0" : "=r" (c1v));
  } while (c1v - c0v < ticks && ++iters < cap);
  uint64_t c1p = 0, ctl2 = 0;
  __asm__ volatile ("isb; mrs %0, cntpct_el0" : "=r" (c1p));
  __asm__ volatile ("mrs %0, cntv_ctl_el0" : "=r" (ctl2));
  const uint32_t isp1 = read32 (sgi, 0x200);
  const unsigned b19 = (isp1 >> 19) & 1U, b27 = (isp1 >> 27) & 1U;
  printf ("PICASSO TIMER: stageB after ctl=%llx ispendr=%08x bit19=%u bit27=%u dcntvct=%llu dcntpct=%llu iters=%llu\n",
      ctl2, isp1, b19, b27, c1v - c0v, c1p - c0p, iters);
  const char *verdict = (!b19 && !b27) ? "D-neither" : (b19 && b27) ? "C-both" : b19 ? "A-intid19" : "B-intid27";
  printf ("PICASSO TIMER: stageB verdict=%s\n", verdict);
  armcpu_timer_probe[20] = isp0;
  armcpu_timer_probe[21] = static_cast<uint32_t> (ctl0);
  armcpu_timer_probe[22] = isp1;
  armcpu_timer_probe[23] = static_cast<uint32_t> (ctl2);
  armcpu_timer_probe[24] = static_cast<uint32_t> (c1v - c0v);
  armcpu_timer_probe[25] = static_cast<uint32_t> ((c1v - c0v) >> 32);
  armcpu_timer_probe[26] = static_cast<uint32_t> (c1p - c0p);
  armcpu_timer_probe[27] = static_cast<uint32_t> ((c1p - c0p) >> 32);
  armcpu_timer_probe[28] = static_cast<uint32_t> (iters);
  armcpu_timer_probe[29] = static_cast<uint32_t> (iters >> 32);
  printf ("PICASSO TIMER: stageB complete\n");
}

// M1 stage B2: counter-free wait support. The patch-side control loop (which
// pets the watchdog every millisecond) arms the virtual timer once via
// armcpu_timer_stageb2_arm, then calls armcpu_timer_stageb2_sample every 100
// iterations. Timing comes from loop iterations (host-calibrated), never from
// CNTVCT/CNTPCT deltas, which r23 proved untrustworthy on this board.
void ArmCpu::timerStageB2Arm(void)
{
  if (redist == 0 || dist == 0) { printf ("PICASSO TIMER: stageb2 no GIC mapping\n"); return; }
  uint64_t ctl0 = 0;
  __asm__ volatile ("mrs %0, cntv_ctl_el0" : "=r" (ctl0));
  if ((ctl0 & 0x3ULL) != 0x1ULL) {
    const uint64_t fixed = (ctl0 & ~0x3ULL) | 0x1ULL;
    __asm__ volatile ("msr cntv_ctl_el0, %0; isb" :: "r" (fixed) : "memory");
    printf ("PICASSO TIMER: stageb2 ctl fixed %llx -> %llx\n", ctl0, fixed);
  }
  __asm__ volatile ("msr cntv_tval_el0, %0; isb" :: "r" (19200000ULL) : "memory");
  uint64_t ctl1 = 0, tval1 = 0;
  __asm__ volatile ("mrs %0, cntv_ctl_el0" : "=r" (ctl1));
  __asm__ volatile ("mrs %0, cntv_tval_el0" : "=r" (tval1));
  printf ("PICASSO TIMER: stageb2 armed ctl=%llx tval=%llx\n", ctl1, tval1);
}

void ArmCpu::timerStageB2Sample(unsigned ms)
{
  if (redist == 0 || dist == 0) { printf ("PICASSO TIMER: stageb2 t=%u no mapping\n", ms); return; }
  const uintptr_t sgi = redist + 0x10000;
  uint64_t ctl = 0, tval = 0, ctv = 0, ctp = 0;
  __asm__ volatile ("mrs %0, cntv_ctl_el0" : "=r" (ctl));
  __asm__ volatile ("mrs %0, cntv_tval_el0" : "=r" (tval));
  __asm__ volatile ("isb; mrs %0, cntvct_el0" : "=r" (ctv));
  __asm__ volatile ("isb; mrs %0, cntpct_el0" : "=r" (ctp));
  const uint32_t isp = read32 (sgi, 0x200);
  printf ("PICASSO TIMER: stageb2 poll t=%u ctl=%llx tval=%llx ispendr=%08x bit19=%u bit27=%u cntvct=%llu cntpct=%llu\n",
      ms, ctl, tval, isp, (isp >> 19) & 1U, (isp >> 27) & 1U, ctv, ctp);
}

extern "C" void
armcpu_timer_stageb2_arm(void)
{
  ArmCpu *cpu = __atomic_load_n (&gBootCpu, __ATOMIC_ACQUIRE);
  if (cpu != nullptr) {
    cpu->timerStageB2Arm ();
  } else {
    printf ("PICASSO TIMER: stageb2 no boot CPU instance\n");
  }
}

extern "C" void
armcpu_timer_stageb2_sample(unsigned ms)
{
  ArmCpu *cpu = __atomic_load_n (&gBootCpu, __ATOMIC_ACQUIRE);
  if (cpu != nullptr) {
    cpu->timerStageB2Sample (ms);
  }
}

// M1 stage C: minimal closed loop on INTID 19. Begin (DAIF.I set): move the
// line to Group 1, run the existing enableGic (ISENABLER/GICD/ICC), re-arm the
// virtual timer. The patch loop then samples pending with interrupts still
// masked; Go clears DAIF.I so the CPU takes the IRQ; End re-masks, disables
// the source and reports the count. Gated on native-timer-selftest=1.
void ArmCpu::timerSelftestBegin(void)
{
  if (redist == 0 || dist == 0) { printf ("PICASSO TIMER: selftest no GIC mapping\n"); return; }
  const uintptr_t sgi = redist + 0x10000;
  __asm__ volatile ("msr daifset, #2; isb" ::: "memory");
  write32 (sgi, 0x80, read32 (sgi, 0x80) | (1U << 19));
  enableGic ();
  uint64_t ctl0 = 0;
  __asm__ volatile ("mrs %0, cntv_ctl_el0" : "=r" (ctl0));
  if ((ctl0 & 0x3ULL) != 0x1ULL) {
    const uint64_t fixed = (ctl0 & ~0x3ULL) | 0x1ULL;
    __asm__ volatile ("msr cntv_ctl_el0, %0; isb" :: "r" (fixed) : "memory");
    printf ("PICASSO TIMER: selftest ctl fixed %llx -> %llx\n", ctl0, fixed);
  }
  __asm__ volatile ("msr cntv_tval_el0, %0; isb" :: "r" (19200000ULL) : "memory");
  uint64_t ctl1 = 0, tval1 = 0, daif = 0;
  __asm__ volatile ("mrs %0, cntv_ctl_el0" : "=r" (ctl1));
  __asm__ volatile ("mrs %0, cntv_tval_el0" : "=r" (tval1));
  __asm__ volatile ("mrs %0, daif" : "=r" (daif));
  // r26 read-only intent probe (single variable vs r25): HPPIR tells whether
  // the GIC intends IRQ-19 as Group-1, Group-0/FIQ, or nothing on unmask.
  uint64_t hppir1 = 1023, vbar = 0, isr = 0, curEL = 0;
  __asm__ volatile ("mrs %0, icc_hppir1_el1" : "=r" (hppir1));
  // r33: read-only exception state (single variable vs r32). No interrupt
  // has ever been taken on picasso; if VBAR is 0 the unmask vectors into
  // nowhere, which explains the hang better than any GIC theory.
  __asm__ volatile ("mrs %0, vbar_el1" : "=r" (vbar));
  __asm__ volatile ("mrs %0, isr_el1" : "=r" (isr));
  __asm__ volatile ("mrs %0, currentel" : "=r" (curEL));
  const uint32_t grp = read32 (sgi, 0x80);
  const uint32_t ena = read32 (sgi, 0x100);
  printf ("PICASSO TIMER: selftest begin ctl=%llx tval=%llx daif=%llx igroup=%08x isenabler=%08x ready=%u hppir1=%llx vbar=%llx isr=%llx curel=%llx\n",
      ctl1, tval1, daif, grp, ena, armcpu_gic_ready, hppir1, vbar, isr, curEL);
  // r34: dump the EL1h IRQ vector words (single variable vs r33). If the
  // VBAR mapping is not up yet, these loads fault here (snapshot-only hang);
  // if they print, the mapping exists and a later Go-hang is in the take/
  // handler path, not in unmapped vectors.
  if (vbar != 0) {
    const volatile uint32_t *vec =
        reinterpret_cast<const volatile uint32_t *> (vbar + 0x80);
    uint32_t w0 = vec[0], w1 = vec[1], w2 = vec[2], w3 = vec[3];
    printf ("PICASSO TIMER: selftest vectors el1h_irq %08x %08x %08x %08x\n",
        w0, w1, w2, w3);
  }
}

void ArmCpu::timerSelftestSample(unsigned ms)
{
  if (redist == 0 || dist == 0) { printf ("PICASSO TIMER: selftest t=%u no mapping\n", ms); return; }
  const uintptr_t sgi = redist + 0x10000;
  uint64_t ctl = 0, tval = 0, daif = 0;
  __asm__ volatile ("mrs %0, cntv_ctl_el0" : "=r" (ctl));
  __asm__ volatile ("mrs %0, cntv_tval_el0" : "=r" (tval));
  __asm__ volatile ("mrs %0, daif" : "=r" (daif));
  uint64_t hppir1 = 1023, vbar = 0, isr = 0, curEL = 0;
  __asm__ volatile ("mrs %0, icc_hppir1_el1" : "=r" (hppir1));
  // r33: read-only exception state (single variable vs r32). No interrupt
  // has ever been taken on picasso; if VBAR is 0 the unmask vectors into
  // nowhere, which explains the hang better than any GIC theory.
  __asm__ volatile ("mrs %0, vbar_el1" : "=r" (vbar));
  __asm__ volatile ("mrs %0, isr_el1" : "=r" (isr));
  __asm__ volatile ("mrs %0, currentel" : "=r" (curEL));
  const uint32_t isp = read32 (sgi, 0x200);
  printf ("PICASSO TIMER: selftest poll t=%u irq=%llu phase=%u id=%u ctl=%llx tval=%llx ispendr=%08x bit19=%u daif=%llx hppir1=%llx vbar=%llx isr=%llx curel=%llx\n",
      ms, armcpu_timer_interrupts, armcpu_irq_phase, armcpu_irq_id, ctl, tval,
      isp, (isp >> 19) & 1U, daif, hppir1, vbar, isr, curEL);
}

void ArmCpu::timerSelftestGo(void)
{
  uint64_t before = 0;
  __asm__ volatile ("mrs %0, daif" : "=r" (before));
  // r31: live timer at unmask (r30 disarm reverted); the handler bypass
  // (native-timer-no-rtclock=1) is what keeps this alive.
  __asm__ volatile ("msr daifclr, #2; isb" ::: "memory");
  printf ("PICASSO TIMER: selftest go daif=%llx -> unmasked (live timer, no-rtclock bypass)\n", before);
}

void ArmCpu::timerSelftestEnd(void)
{
  const uintptr_t sgi = redist + 0x10000;
  // r52 (single variable vs r51): keep the live timer for M1 Stage D.
  // r51 proved DEFER pre daif=0x200 (IRQ enabled at entry) but post=0:
  // the 5 s blocking IOSleep never wakes because this End masks DAIF.I
  // and disables INTID 19 right after the sleep test (which runs before
  // End, so it still passes). With native-timer-keep=1 skip the teardown,
  // leave the source enabled and IRQs unmasked so the deferred UFS wait
  // can fire via rtclock.
  int keep = 0;
  if (PE_parse_boot_argn("native-timer-keep", &keep, sizeof(keep)) && keep) {
    __asm__ volatile ("msr daifclr, #2; isb" ::: "memory");
    uint64_t daif = 0;
    __asm__ volatile ("mrs %0, daif" : "=r" (daif));
    printf("PICASSO TIMER: selftest end kept irq=%llu phase=%u id=%u daif=%llx\n",
        armcpu_timer_interrupts, armcpu_irq_phase, armcpu_irq_id, daif);
    // r54 (single variable vs r53): synchronous re-arm probe. r53 proved a
    // keep-path thread is never scheduled once the 60 s control-window busy
    // loop starts (zero keep samples), so sample here in the boot thread
    // before returning to the patch. Re-arm TVAL=1s, spin 1.2s on CNTVCT,
    // then report ISPENDR bit19 + IRQ delta. Pending=1 means the GIC+timer
    // hardware still works after keep and the fault is in rtclock re-arm;
    // pending=0 means the timer itself stopped.
    {
      const uint64_t irq0 = armcpu_timer_interrupts;
      uint64_t ctl0 = 0, tval0 = 0;
      __asm__ volatile ("mrs %0, cntv_ctl_el0" : "=r" (ctl0));
      __asm__ volatile ("mrs %0, cntv_tval_el0" : "=r" (tval0));
      printf("PICASSO TIMER: keep rearm begin ctl=%llx tval=%llx irq=%llu\n",
          ctl0, tval0, irq0);
      __asm__ volatile ("msr cntv_tval_el0, %0; isb" :: "r" (19200000ULL) : "memory");
      uint64_t freq = 0;
      __asm__ volatile ("mrs %0, cntfrq_el0" : "=r" (freq));
      if (freq == 0) { freq = 19200000ULL; }
      const uint64_t c0 = armcpu_cntvct();
      while (armcpu_cntvct() - c0 < freq * 12ULL / 10ULL) {
        for (unsigned k = 0; k < 1000; k++) {
          __asm__ volatile ("yield" ::: "memory");
        }
      }
      uint64_t ctl1 = 0, tval1 = 0, daif1 = 0;
      __asm__ volatile ("mrs %0, cntv_ctl_el0" : "=r" (ctl1));
      __asm__ volatile ("mrs %0, cntv_tval_el0" : "=r" (tval1));
      __asm__ volatile ("mrs %0, daif" : "=r" (daif1));
      const uint32_t isp = (redist != 0) ? read32(sgi, 0x200) : 0u;
      printf("PICASSO TIMER: keep rearm end ctl=%llx tval=%llx ispendr=%08x bit19=%u daif=%llx irq=%llu->%llu\n",
          ctl1, tval1, isp, (isp >> 19) & 1U, daif1,
          irq0, armcpu_timer_interrupts);
    }
    return;
  }
  __asm__ volatile ("msr daifset, #2; isb" ::: "memory");
  if (redist != 0 && dist != 0) {
    write32 (sgi, 0x180, 1U << 19);
  }
  uint64_t daif = 0;
  __asm__ volatile ("mrs %0, daif" : "=r" (daif));
  printf ("PICASSO TIMER: selftest end irq=%llu phase=%u id=%u daif=%llx\n",
      armcpu_timer_interrupts, armcpu_irq_phase, armcpu_irq_id, daif);
}

extern "C" void
armcpu_timer_selftest_begin(void)
{
  ArmCpu *cpu = __atomic_load_n (&gBootCpu, __ATOMIC_ACQUIRE);
  if (cpu != nullptr) {
    cpu->timerSelftestBegin ();
  } else {
    printf ("PICASSO TIMER: selftest no boot CPU instance\n");
  }
}

extern "C" void
armcpu_timer_selftest_sample(unsigned ms)
{
  ArmCpu *cpu = __atomic_load_n (&gBootCpu, __ATOMIC_ACQUIRE);
  if (cpu != nullptr) {
    cpu->timerSelftestSample (ms);
  }
}

extern "C" void
armcpu_timer_selftest_go(void)
{
  ArmCpu *cpu = __atomic_load_n (&gBootCpu, __ATOMIC_ACQUIRE);
  if (cpu != nullptr) {
    cpu->timerSelftestGo ();
  }
}

extern "C" void
armcpu_timer_selftest_end(void)
{
  ArmCpu *cpu = __atomic_load_n (&gBootCpu, __ATOMIC_ACQUIRE);
  if (cpu != nullptr) {
    cpu->timerSelftestEnd ();
  }
}

// r51: print the deferred-wait capture (stored with MRS+store only, no calls in
// the interrupt-disabled deferred context after r50's zalloc panic). This
// sleep test has proven-safe IOLog.
extern uint64_t gPicassoDeferPreDaif, gPicassoDeferPreVct;
extern uint64_t gPicassoDeferPostDaif, gPicassoDeferPostVct;
extern unsigned gPicassoDeferPostOk;
// r41 (single variable vs r40: measurement only). Timed IOSleep(100) plus a
// timed 100 ms msleep timeout, both measured against CNTVCT/CNTFRQ so the
// verdict does not depend on the clock under test. Runs in the synchronous
// control window after the selftest; the ~200 ms total block is far below
// the watchdog interval.
void ArmCpu::timerSleepTest (void)
{
  // r44 (single variable vs r43: ten iterations). M1 acceptance item 2:
  // IOSleep(100) 10/10 in 90-110 ms plus assert_wait_deadline 100 ms
  // timeouts 10/10, each timed against CNTVCT/CNTFRQ with IRQ deltas.
  // Runs with the live timer (called before selftest_end's disable).
  uint64_t freq = 0;
  __asm__ volatile ("mrs %0, cntfrq_el0" : "=r" (freq));
  uint64_t ctl = 0;
  __asm__ volatile ("mrs %0, cntv_ctl_el0" : "=r" (ctl));
  const uint64_t irq0 = armcpu_timer_interrupts;
  const uint64_t vinit = armcpu_cntvct ();
  const uint64_t pinit = armcpu_cntpct ();
  printf ("PICASSO TIMER: sleep raw freq=%llu vct0=%llu pct0=%llu ctl=%llx irq0=%llu\n",
      freq, vinit, pinit, ctl, irq0);
  unsigned io_pass = 0, aw_pass = 0;
  for (unsigned i = 0; i < 10; i++) {
    const uint64_t v0 = armcpu_cntvct ();
    const uint64_t p0 = armcpu_cntpct ();
    const uint64_t ia = armcpu_timer_interrupts;
    IOSleep (100);
    const uint64_t v1 = armcpu_cntvct ();
    const uint64_t p1 = armcpu_cntpct ();
    const uint64_t ib = armcpu_timer_interrupts;
    const uint64_t io_ms = (freq != 0) ? (v1 - v0) * 1000ULL / freq : 0;
    const bool io_ok = (io_ms >= 90 && io_ms <= 110);
    if (io_ok) { io_pass++; }
    static int aw_tok = 0;
    uint64_t deadline = 0;
    clock_interval_to_deadline (100, NSEC_PER_MSEC, &deadline);
    const uint64_t mat0 = mach_absolute_time ();
    const uint64_t w0 = armcpu_cntvct ();
    wait_result_t wa = assert_wait_deadline (&aw_tok, THREAD_UNINT, deadline);
    wait_result_t w = wa;
    if (w == THREAD_WAITING) { w = thread_block (THREAD_CONTINUE_NULL); }
    const uint64_t w1 = armcpu_cntvct ();
    const uint64_t mat1 = mach_absolute_time ();
    const uint64_t aw_ms = (freq != 0) ? (w1 - w0) * 1000ULL / freq : 0;
    const bool aw_ok = (w == THREAD_TIMED_OUT && aw_ms >= 90 && aw_ms <= 150);
    if (aw_ok) { aw_pass++; }
    printf ("PICASSO TIMER: sleep iter=%u io_ms=%llu%s aw_a=%d aw_rc=%d aw_ms=%llu%s dl=%llu mat=%llu->%llu w=%llu->%llu irq=%llu->%llu\n",
        i, io_ms, io_ok ? " PASS" : " FAIL", wa, w, aw_ms, aw_ok ? " PASS" : " FAIL",
        deadline, mat0, mat1, w0, w1, ia, ib);
  }
  printf ("PICASSO TIMER: defer pre daif=%llx vct=%llu\n",
      gPicassoDeferPreDaif, gPicassoDeferPreVct);
  printf ("PICASSO TIMER: sleep summary io=%u/10 aw=%u/10 %s\n",
      io_pass, aw_pass, (io_pass == 10 && aw_pass == 10) ? "PASS" : "FAIL");
  printf ("PICASSO TIMER: defer post daif=%llx vct=%llu ok=%u\n",
      gPicassoDeferPostDaif, gPicassoDeferPostVct, gPicassoDeferPostOk);
}

extern "C" void
armcpu_timer_sleep_test(void)
{
  ArmCpu *cpu = __atomic_load_n (&gBootCpu, __ATOMIC_ACQUIRE);
  if (cpu != nullptr) {
    cpu->timerSleepTest ();
  } else {
    printf ("PICASSO TIMER: sleep test with no boot CPU instance\n");
  }
}

// r39: run start()'s stalled tail (direct dispatch install + initCPU) on
// whichever thread gets here first. Minimal on purpose: the shared interrupt
// controller is useless for cpus=1, and registerService can wait for the
// matching thread (it owns IOKit publication). Once the timer IRQ works,
// preemption returns and every frozen thread resumes on its own.
void ArmCpu::finishBootStart (void)
{
  ml_install_interrupt_handler (this, static_cast<int> (logicalId), this, reinterpret_cast<IOInterruptHandler> (&ArmCpu::dispatch), nullptr);
  printf ("PICASSO TIMER: boot dispatch installed (boot-finish path)\n");
  initCPU (true);
}

extern "C" void
armcpu_boot_finish(void)
{
  if (__atomic_exchange_n (&gBootFinished, 1, __ATOMIC_ACQ_REL) != 0) { return; }
  ArmCpu *cpu = __atomic_load_n (&gBootCpu, __ATOMIC_ACQUIRE);
  if (cpu == nullptr) {
    printf ("PICASSO TIMER: boot finish with no boot CPU instance\n");
    return;
  }
  cpu->finishBootStart ();
}

extern "C" void
armcpu_timer_probe_now(void)
{
  // r39: finish start()'s tail here. The matching thread freezes mid-start
  // r39: finish start()'s tail here (see armcpu_boot_finish below). The
  // matching thread freezes mid-start (r38: no M1 marker; cpus=1, no timer
  // preemption, spinner never blocks), so initCPU(true) never runs and no
  // IRQ handler is installed. Guarded: if the matching thread resumes
  // later, start()'s own bypass re-applies the direct install.
  armcpu_boot_finish();
  ArmCpu *cpu = __atomic_load_n (&gBootCpu, __ATOMIC_ACQUIRE);
  if (cpu != nullptr) {
    cpu->framebufferProbe ();
    cpu->timerProbe ();
    int stageb = 0;
    if (PE_parse_boot_argn ("native-timer-stageb", &stageb, sizeof (stageb)) && stageb) {
      cpu->timerStageB ();
    }
  } else {
    printf ("PICASSO TIMER: no boot CPU instance registered=%u gic_ready=%u\n",
        armcpu_registered, armcpu_gic_ready);
  }
}

void ArmCpu::enableGic (void)
{
  uint64_t value;
  __asm__ volatile ("mrs %0, icc_sre_el1" : "=r" (value));
  value |= 1;
  __asm__ volatile ("msr icc_sre_el1, %0; isb" :: "r" (value) : "memory");
  __asm__ volatile ("mrs %0, icc_sre_el1" : "=r" (value));
  if ((value & 1) == 0) { panic ("ArmCpu: GICv3 system register interface unavailable"); }
  __asm__ volatile ("mrs %0, icc_ctlr_el1" : "=r" (value));
  value &= ~2ULL; // EOImode 0: EOIR also deactivates the interrupt.
  __asm__ volatile ("msr icc_ctlr_el1, %0" :: "r" (value) : "memory");
  value = 0xff;
  __asm__ volatile ("msr icc_pmr_el1, %0" :: "r" (value) : "memory");
  value = 0;
  __asm__ volatile ("msr icc_bpr1_el1, %0" :: "r" (value) : "memory");
  if (!secondary) { gBootAffinity = affinity & 0xff00ffffffULL; }
  if (!secondary || !gRemoteGic) {
    write32 (redist + 0x10000, 0x100, ArmCpuSgiPpiEnable (gVirtualTimerIntid));
    // ARE_NS, EnableGrp1NS and EnableGrp1A. The working GICv3 reference
    // reaches 0x13 here; 0x12 leaves Group 1 NS sources disabled.
    write32 (dist, 0, read32 (dist, 0) | 0x13U);
  }
  value = 1;
  __asm__ volatile ("msr icc_igrpen1_el1, %0; dsb sy; isb" :: "r" (value) : "memory");
  if (!secondary) {
    uint64_t pmr, ig, sre;
    __asm__ volatile ("mrs %0, icc_pmr_el1" : "=r" (pmr));
    __asm__ volatile ("mrs %0, icc_igrpen1_el1" : "=r" (ig));
    __asm__ volatile ("mrs %0, icc_sre_el1" : "=r" (sre));
    armcpu_gic_boot[0] = read32 (redist + 0x10000, 0x80);
    armcpu_gic_boot[1] = read32 (redist + 0x10000, 0x100);
    armcpu_gic_boot[2] = read32 (dist, 0);
    armcpu_gic_boot[3] = read32 (dist, 4);
    armcpu_gic_boot[4] = read32 (redist + 0x10000, 0xd00);
    armcpu_gic_boot[5] = static_cast<uint32_t> (pmr);
    armcpu_gic_boot[6] = static_cast<uint32_t> (ig);
    armcpu_gic_boot[7] = static_cast<uint32_t> (sre);
  }
  armcpu_gic_ready = 1;
}


bool ArmCpu::start (IOService *provider)
{
  unsigned skip = 0;
  PE_parse_boot_argn ("armcpu-skip-gic", &skip, sizeof (skip));
  skipGic = skip != 0;
  unsigned selfIpi = 0;
  PE_parse_boot_argn ("armcpu-self-ipi", &selfIpi, sizeof (selfIpi));
  gSelfIpi = selfIpi == 1;
  unsigned spiIpi = 0;
  PE_parse_boot_argn ("armcpu-spi-ipi", &spiIpi, sizeof (spiIpi));
  gSpiIpi = spiIpi == 1;
  unsigned diagnostic = 0;
  PE_parse_boot_argn("armcpu-diagnostics", &diagnostic, sizeof(diagnostic));
  diagnosePower=diagnostic==1;
  uint32_t physical;
  OSData *reg = OSDynamicCast (OSData, provider->getProperty ("reg"));
  memcpy (&physical, reg->getBytesNoCopy (), sizeof (physical));
  logicalId = static_cast<uint32_t> (ml_get_cpu_number (physical));
  secondary = logicalId != static_cast<uint32_t> (ml_get_boot_cpu_number ());
  if (secondary) {
    if (!IOCPU::start (provider)) { return false; }
    armcpu_smp_step[logicalId] = 1;
    affinity = physical;
    armcpu_smp_mpidr[logicalId] = physical;
    cpuName = OSSymbol::withCStringNoCopy ("ARM architectural secondary CPU");
    if (cpuName == nullptr) { return false; }
    retain ();
    __atomic_store_n (&gPending[logicalId], this, __ATOMIC_RELEASE);
    if (gBootRegistered != 0) { drainPending (); }
    return true;
  } else {
    uint64_t bootMpidr;
    __asm__ volatile ("mrs %0, mpidr_el1" : "=r" (bootMpidr));
    armcpu_boot_mpidr = static_cast<uint32_t> (bootMpidr);
    if ((!skipGic && !prepareGic ()) || !IOCPU::start (provider)) { return false; }
    __atomic_store_n (&gBootCpu, this, __ATOMIC_RELEASE);
  }
  cpuName = OSSymbol::withCStringNoCopy ("ARM architectural boot CPU");
  if (cpuName == nullptr) { return false; }
  ml_processor_info_t info = {};
  info.cpu_id = this;
  info.phys_id = physical;
  info.log_id = logicalId;
  info.cluster_id = static_cast<uint32_t> (ml_get_cluster_number (physical));
  info.cluster_type = ml_get_boot_cluster_type ();
  perfmon_interrupt_handler_func pmi = nullptr;
  kern_return_t status = ml_processor_register (&info, &machProcessor, &ipi_handler, &pmi);
  if (status != KERN_SUCCESS) { panic ("ArmCpu: CPU %u registration failed: %d", logicalId, status); }
  setCPUNumber (info.log_id);
  armcpu_registered = 1;
  unsigned smp = 0;
  PE_parse_boot_argn ("sheng-smp", &smp, sizeof (smp));
  gSmpWanted = smp != 0;
  unsigned remote = 0;
  PE_parse_boot_argn ("sheng-smp-gic", &remote, sizeof (remote));
  gRemoteGic = remote != 0;
  armcpu_smp_requested = gSmpWanted ? 1 : 0;
  unsigned cpuCount = static_cast<unsigned> (ml_get_max_cpu_number ()) + 1;
  controller = new IOCPUInterruptController;
  if ((controller == nullptr) || (controller->initCPUInterruptController (cpuCount, 1) != kIOReturnSuccess)) {
    panic ("ArmCpu: CPU interrupt controller allocation failed");
  }
  // r38: bisect where start() parks (r37: proof print never appeared).
  printf ("PICASSO TIMER: start marker controller-init-ok\n");
  controller->registerCPUInterruptController ();
  printf ("PICASSO TIMER: start marker cpu-controller-registered\n");
  controller->enableCPUInterrupt (this);
  printf ("PICASSO TIMER: start marker cpu-interrupt-enabled\n");
  // r37: registerInterrupt parks in assert_wait+thread_block until every ADT
  // CPU enables (enabledCPUs != numCPUs with cpus=1), so start() never
  // reached initCPU(true) (r36: gic_ready=0 boot_registered=0). Install the
  // dispatch directly like the secondary path instead.
  ml_install_interrupt_handler (this, static_cast<int> (logicalId), this, reinterpret_cast<IOInterruptHandler> (&ArmCpu::dispatch), nullptr);
  printf ("PICASSO TIMER: boot dispatch installed (bypassed shared registerInterrupt)\n");
  gController = controller;
  initCPU (true);
  registerService ();
  __atomic_store_n (&gBootRegistered, 1U, __ATOMIC_RELEASE);
  drainPending ();
  return true;
}

void ArmCpu::drainPending (void)
{
  for (unsigned id = 1; id < ARMCPU_SMP_SLOTS; id++) {
    ArmCpu *cpu = __atomic_exchange_n (&gPending[id], static_cast<ArmCpu *> (nullptr), __ATOMIC_ACQ_REL);
    if (cpu != nullptr) { cpu->finishSecondary (); }
  }
}

bool ArmCpu::finishSecondary (void)
{
  dist = gDist;
  redistBase = gFrames;
  redistSize = gFramesSize;
  if (gRemoteGic && !wakeRedistributor (false)) { armcpu_smp_fail[logicalId] = 3; return false; }
  ml_processor_info_t info = {};
  info.cpu_id = this;
  info.phys_id = static_cast<uint32_t> (affinity);
  info.log_id = logicalId;
  info.cluster_id = static_cast<uint32_t> (ml_get_cluster_number (static_cast<uint32_t> (affinity)));
  info.cluster_type = ml_get_boot_cluster_type ();
  perfmon_interrupt_handler_func pmi = nullptr;
  kern_return_t status = ml_processor_register (&info, &machProcessor, &ipi_handler, &pmi);
  if (status != KERN_SUCCESS) { armcpu_smp_fail[logicalId] = 4; return false; }
  setCPUNumber (info.log_id);
  armcpu_smp_step[logicalId] = 2;
  setCPUState (kIOCPUStateUninitalized);
  registerService ();
  startSecondary ();
  return true;
}

// PSCI conduit comes from the /psci node of the device tree ("smc" or "hvc"); no default is assumed.
static bool psciConduit (bool *hvc)
{
  IORegistryEntry *psci = IORegistryEntry::fromPath ("/psci", gIODTPlane);
  if (psci == nullptr) { return false; }
  OSData *method = OSDynamicCast (OSData, psci->getProperty ("method"));
  bool ok = false;
  if (method != nullptr && method->getLength () == 4 && memcmp (method->getBytesNoCopy (), "smc", 4) == 0) {
    *hvc = false; ok = true;
  } else if (method != nullptr && method->getLength () == 4 && memcmp (method->getBytesNoCopy (), "hvc", 4) == 0) {
    *hvc = true; ok = true;
  }
  psci->release ();
  return ok;
}

static int64_t psciCall (bool hvc, uint64_t function, uint64_t a, uint64_t b, uint64_t c)
{
  register uint64_t x0 __asm__ ("x0") = function;
  register uint64_t x1 __asm__ ("x1") = a;
  register uint64_t x2 __asm__ ("x2") = b;
  register uint64_t x3 __asm__ ("x3") = c;
  if (hvc) {
    __asm__ volatile ("hvc #0" : "+r" (x0) : "r" (x1), "r" (x2), "r" (x3) : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
  } else {
    __asm__ volatile ("smc #0" : "+r" (x0) : "r" (x1), "r" (x2), "r" (x3) : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
  }
  return static_cast<int64_t> (x0);
}

void ArmCpu::startThread (void *argument, wait_result_t)
{
  ArmCpu *self = static_cast<ArmCpu *> (argument);
  armcpu_smp_step[self->logicalId] = 3;
  // Blocks until the new core reaches the scheduler; a core that never arrives only stalls this helper thread.
  processor_start (self->machProcessor);
  thread_terminate (current_thread ());
}

void ArmCpu::startSecondary (void)
{
  thread_t thread;
  if (kernel_thread_start (&ArmCpu::startThread, this, &thread) != KERN_SUCCESS) {
    armcpu_smp_psci_rc[logicalId] = -1000;
    return;
  }
  thread_deallocate (thread);
}

void ArmCpu::initCPU (bool boot)
{
  if (secondary) {
    if (!boot) { panic ("ArmCpu: secondary resume is unsupported"); }
    armcpu_smp_step[logicalId] = 5;
    uint64_t mpidr;
    __asm__ volatile ("mrs %0, mpidr_el1" : "=r" (mpidr));
    armcpu_smp_seen_mpidr[logicalId] = static_cast<uint32_t> (mpidr);
    // A failure is recorded and the core parked (rather than a panic that hides the other cores' state).
    if ((mpidr & 0xffff) != (affinity & 0xffff)) { armcpu_smp_fail[logicalId] = 1; for (;;) { __asm__ volatile ("wfe"); } }
    if (!gRemoteGic && !wakeRedistributor (true)) { armcpu_smp_fail[logicalId] = 2; for (;;) { __asm__ volatile ("wfe"); } }
    armcpu_smp_step[logicalId] = 6;
    enableGic ();
    if (gSpiIpi) { spiIpiConfigure (gDist, logicalId, affinity); }
    // The shared IOCPUInterruptController blocks registrations until every core is enabled, so the secondary cores
    // install their handler directly. ml_install_interrupt_handler() records it in the calling core's cpu_data.
    ml_install_interrupt_handler (this, static_cast<int> (logicalId), this, reinterpret_cast<IOInterruptHandler> (&ArmCpu::dispatch), nullptr);
    enableCPUTimeBase (true);
    setCPUState (kIOCPUStateRunning);
    armcpu_smp_step[logicalId] = 7;
    armcpu_smp_running = armcpu_smp_running + 1;
    // XNU clears SIGPdisabled for a core the first time that core takes an IPI, and cpu_signal() silently skips a core until then; Apple's
    // CPU driver provokes it with a self-IPI from initCPU (osfmk/kern/machine.c, processor_up).
    if (gSelfIpi) { signalCPU (this); }
    return;
  }
  if (!boot) { panic ("ArmCpu: secondary/resume CPU startup is unsupported"); }
  if (skipGic) {
    // Explicit A/B hardware diagnostic: do not touch GIC MMIO or enable an
    // interrupt source. CPU registration still happens; the timer is left off.
    setCPUState (kIOCPUStateRunning);
    return;
  }
  enableGic ();
  if (gSpiIpi) { spiIpiConfigure (gDist, logicalId, affinity); }
  // r35: install the boot-CPU IRQ dispatch (mirrors the secondary path).
  // Without this cpu_data->interrupt_handler stays 0 and the first IRQ
  // vectors to PC=0 (r33 panic photo: fetch abort at 0x0).
  ml_install_interrupt_handler (this, static_cast<int> (logicalId), this, reinterpret_cast<IOInterruptHandler> (&ArmCpu::dispatch), nullptr);
  enableCPUTimeBase (true);
  setCPUState (kIOCPUStateRunning);
  if (gSelfIpi) { signalCPU (this); }
}

void ArmCpu::dispatch (void *target, void *, void *, int)
{
  static_cast<ArmCpu *> (target)->interrupt ();
}

void ArmCpu::interrupt (void)
{
  unsigned drained=0;
  for (;;) {
    armcpu_irq_phase=1;
    uint64_t id;
    __asm__ volatile ("mrs %0, icc_iar1_el1" : "=r" (id));
    armcpu_irq_id=static_cast<uint32_t>(id & 0xffffff);
    if ((id & 0xffffff) >= 1020) { armcpu_irq_phase=0;break; }
    if(diagnosePower && ++drained>128)panic("ArmCpu: IRQ drain exceeds 128 id=%llx timer=%llu IPI=%llu",id,armcpu_timer_interrupts,armcpu_ipi_interrupts);
    if ((id & 0xffffff) == gVirtualTimerIntid) {
      armcpu_timer_interrupts = armcpu_timer_interrupts + 1;
      armcpu_core_timer[logicalId & 7] = armcpu_core_timer[logicalId & 7] + 1;
      armcpu_irq_phase=2;
      // r40: restore the production clock handler (take/dispatch proven by
      // r39 5x). If this hangs, rtclock itself is the blocker, not the
      // take path. rtclock re-arms the timer via ml_set_decrementer.
      rtclock_intr (0);
      if (picasso_usb_timer_tick != nullptr) {
        picasso_usb_timer_tick ();
      }
      armcpu_irq_phase=3;
    } else if ((id & 0xffffff) == 0) {
      armcpu_ipi_interrupts = armcpu_ipi_interrupts + 1;
      armcpu_core_sgi[logicalId & 7] = armcpu_core_sgi[logicalId & 7] + 1;
      armcpu_irq_phase=4;
      if (ipi_handler != nullptr) { ipi_handler (); }
      armcpu_irq_phase=5;
    } else if (gSpiIpi && (id & 0xffffff) >= kIpiSpiBase && (id & 0xffffff) < kIpiSpiBase + 8) {
      armcpu_ipi_interrupts = armcpu_ipi_interrupts + 1;
      armcpu_core_sgi[logicalId & 7] = armcpu_core_sgi[logicalId & 7] + 1;
      armcpu_irq_phase = 4;
      if (ipi_handler != nullptr) { ipi_handler (); }
      armcpu_irq_phase = 5;
    } else {
      const uint32_t intid = static_cast<uint32_t> (id & 0xffffff);
      ArmCpuSpi *spi = nullptr;
      const uint32_t used = __atomic_load_n (&gSpiUsed, __ATOMIC_ACQUIRE);
      for (uint32_t slot = 0; intid >= 32 && slot < used && spi == nullptr; slot++) {
        if (gSpi[slot].id == intid) { spi = &gSpi[slot]; }
      }
      if (spi != nullptr) {
        armcpu_irq_phase = 7;
        spi->count = spi->count + 1;
        spi->handler (spi->context);
      } else if (intid >= 32 && gDist != 0) {
        armcpu_spi_unhandled = armcpu_spi_unhandled + 1;
        armcpu_spi_unhandled_id = intid;
        write32 (gDist, 0x180 + (intid / 32) * 4, 1U << (intid % 32));
      } else {
        panic ("ArmCpu: unexpected architectural IRQ %llu", id);
      }
    }
    armcpu_irq_phase=6;
    __asm__ volatile ("msr icc_eoir1_el1, %0; isb" :: "r" (id) : "memory");
  }
}

void ArmCpu::signalCPU (IOCPU *target)
{
  ArmCpu *peer = OSDynamicCast (ArmCpu, target);
  if (peer == nullptr || (peer->signalTarget () & 0xff) >= 16) {
    panic ("ArmCpu: unsupported CPU signal target");
  }
  uint64_t aff = peer->signalTarget ();
  uint64_t sgi = ((aff & 0xff00000000ULL) << 16) | ((aff & 0xff0000) << 16) |
                 ((aff & 0xff00) << 8) | (1ULL << (aff & 0xff));
  armcpu_ipi_sent = armcpu_ipi_sent + 1;
  if (gSpiIpi) { spiIpiSend (gDist, peer->logicalId & 7); return; }
  __asm__ volatile ("dsb ishst; msr icc_sgi1r_el1, %0; isb" :: "r" (sgi) : "memory");
}

// Diagnostic: sends SGI 0 to the core whose MPIDR affinity is aff (the DT "reg" value, e.g. 0x100 for core 1) exactly as signalCPU
// does. The handler on the target runs the kernel's IPI callback with nothing pending, which is harmless. Returns nothing: the caller
// compares armcpu_ipi_interrupts before and after.
// Read-only snapshot of the GIC state that decides whether SGI 0 reaches the core with MPIDR affinity aff: its redistributor's SGI frame and
// the distributor control register. out: 0 IGROUPR0, 1 ISENABLER0, 2 ISPENDR0, 3 IGRPMODR0, 4 GICR_WAKER, 5 GICD_CTLR, 6 IPRIORITYR0, 7 found(1).
// armcpu_gic_diag_stage names the access in progress, so a read that never returns is identified from another thread:
// 0x100+frame walk, 2 IGROUPR0, 3 ISENABLER0, 4 ISPENDR0, 5 IGRPMODR0, 6 WAKER, 7 GICD_CTLR, 8 IPRIORITYR0, 9 done, 0xe not found.
volatile uint32_t armcpu_gic_diag_stage = 0;
extern "C" void armcpu_gic_diag (uint64_t aff, uint32_t *out)
{
  for (unsigned i = 0; i < 8; i++) { out[i] = 0xdeadbeef; }
  if (gDist == 0 || gFrames == 0) { armcpu_gic_diag_stage = 0xe; return; }
  const uint32_t target = static_cast<uint32_t> ((aff & 0xffffff) | ((aff >> 8) & 0xff000000));
  for (uint64_t offset = 0; offset + 0x20000 <= gFramesSize;) {
    armcpu_gic_diag_stage = 0x100 + static_cast<uint32_t> (offset >> 16);
    const uint64_t typer = *reinterpret_cast<volatile uint64_t *> (gFrames + offset + 8);
    if (static_cast<uint32_t> (typer >> 32) == target) {
      const uintptr_t redist = gFrames + offset, sgi = redist + 0x10000;
      armcpu_gic_diag_stage = 2; out[0] = read32 (sgi, 0x80);
      armcpu_gic_diag_stage = 3; out[1] = read32 (sgi, 0x100);
      armcpu_gic_diag_stage = 4; out[2] = read32 (sgi, 0x200);
      armcpu_gic_diag_stage = 5; out[3] = read32 (sgi, 0xd00);
      armcpu_gic_diag_stage = 6; out[4] = read32 (redist, 0x14);
      armcpu_gic_diag_stage = 7; out[5] = read32 (gDist, 0);
      armcpu_gic_diag_stage = 8; out[6] = read32 (sgi, 0x400);
      out[7] = 1;
      armcpu_gic_diag_stage = 9;
      return;
    }
    if ((typer & 16) != 0) { break; }
    offset += gRedistStride != 0 ? gRedistStride : ((typer & 2) != 0 ? 0x40000 : 0x20000);
  }
  armcpu_gic_diag_stage = 0xe;
}

// Does an SGI sent to the calling core itself become pending at its own CPU interface, and which INTIDs? Only system registers are used (no GIC
// MMIO). With IRQs masked, SGIs 0..15 are sent one at a time to this core's own MPIDR affinity through ICC_SGI1R_EL1 (Group 1) and, when that
// shows nothing, through ICC_SGI0R_EL1 (Group 0); ICC_HPPIR1_EL1 / ICC_HPPIR0_EL1 is polled and a pending SGI is acknowledged and ended here so the
// kernel never sees it. out[8], out[9]: CNTVCT ticks of one ICC_SGI1R_EL1 write and of one ICC_PMR_EL1 read (a trapped write costs far more). out: 0 MPIDR, 1 INTIDs pending as Group 1, 2 INTIDs pending as Group 0, 3 ICC_PMR, 4 ICC_IGRPEN1, 5 ICC_SRE, 6 ICC_CTLR, 7 ICC_RPR.
extern "C" void armcpu_sgi_selfprobe (uint64_t *out)
{
  uint64_t mpidr, daif, v;
  __asm__ volatile ("mrs %0, mpidr_el1" : "=r" (mpidr));
  __asm__ volatile ("mrs %0, daif; msr daifset, #2; isb" : "=r" (daif) :: "memory");
  for (unsigned i = 0; i < 10; i++) { out[i] = 0; }
  out[0] = mpidr;
  __asm__ volatile ("mrs %0, icc_pmr_el1" : "=r" (v)); out[3] = v;
  __asm__ volatile ("mrs %0, icc_igrpen1_el1" : "=r" (v)); out[4] = v;
  __asm__ volatile ("mrs %0, icc_sre_el1" : "=r" (v)); out[5] = v;
  __asm__ volatile ("mrs %0, icc_ctlr_el1" : "=r" (v)); out[6] = v;
  __asm__ volatile ("mrs %0, icc_rpr_el1" : "=r" (v)); out[7] = v;
  if ((mpidr & 0xff) < 16) {
    const uint64_t base = ((mpidr & 0xff00000000ULL) << 16) | ((mpidr & 0xff0000) << 16) | ((mpidr & 0xff00) << 8) | (1ULL << (mpidr & 0xff));
    for (uint64_t intid = 0; intid < 16; intid++) {
      const uint64_t sgi = base | (intid << 24);
      uint64_t h = 1023, ta, tb;
      __asm__ volatile ("dsb ishst; isb; mrs %0, cntvct_el0; isb" : "=r" (ta) :: "memory");
      __asm__ volatile ("msr icc_sgi1r_el1, %0; isb; mrs %1, cntvct_el0" : "=r" (tb) : "r" (sgi) : "memory");
      if (intid == 0) { out[8] = tb - ta; }
      for (unsigned polls = 0; polls < 400; polls++) {
        __asm__ volatile ("mrs %0, icc_hppir1_el1" : "=r" (h));
        if ((h & 0xffffff) == intid) { break; }
        __asm__ volatile ("isb" ::: "memory");
      }
      if ((h & 0xffffff) == intid) {
        out[1] |= 1ULL << intid;
        __asm__ volatile ("mrs %0, icc_iar1_el1" : "=r" (v));
        __asm__ volatile ("msr icc_eoir1_el1, %0; isb" :: "r" (v) : "memory");
        continue;
      }
      __asm__ volatile ("dsb ishst; msr icc_sgi0r_el1, %0; isb" :: "r" (sgi) : "memory");
      for (unsigned polls = 0; polls < 400; polls++) {
        __asm__ volatile ("mrs %0, icc_hppir0_el1" : "=r" (h));
        if ((h & 0xffffff) == intid) { break; }
        __asm__ volatile ("isb" ::: "memory");
      }
      if ((h & 0xffffff) == intid) {
        out[2] |= 1ULL << intid;
        __asm__ volatile ("mrs %0, icc_iar0_el1" : "=r" (v));
        __asm__ volatile ("msr icc_eoir0_el1, %0; isb" :: "r" (v) : "memory");
      }
    }
  }
  {
    uint64_t ta, tb;
    __asm__ volatile ("isb; mrs %0, cntvct_el0; isb; mrs %2, icc_pmr_el1; isb; mrs %1, cntvct_el0" : "=r" (ta), "=r" (tb), "=r" (v) :: "memory");
    out[9] = tb - ta;
  }
  __asm__ volatile ("msr daif, %0; isb" :: "r" (daif) : "memory");
}

extern "C" void armcpu_ipi_selftest (uint64_t aff)
{
  uint64_t sgi = ((aff & 0xff00000000ULL) << 16) | ((aff & 0xff0000) << 16) | ((aff & 0xff00) << 8) | (1ULL << (aff & 0xff));
  armcpu_ipi_selftest_sent = armcpu_ipi_selftest_sent + 1;
  if (gSpiIpi) { spiIpiSend (gDist, static_cast<uint32_t> ((aff >> 8) & 7)); return; }
  __asm__ volatile ("dsb ishst; msr icc_sgi1r_el1, %0; isb" :: "r" (sgi) : "memory");
}

void ArmCpu::enableCPUTimeBase (bool enabled)
{
  uint64_t value = enabled ? 1 : 2;
  __asm__ volatile ("msr cntv_ctl_el0, %0; isb" :: "r" (value) : "memory");
}

void ArmCpu::quiesceCPU (void)
{
  armcpu_quiesce_calls=armcpu_quiesce_calls+1;
  if(diagnosePower)panic("ArmCpu: unsupported quiesce requested before timer/GIC disable caller=%p halt=%u timer=%llu phase=%u",__builtin_return_address(0),armcpu_halt_calls,armcpu_timer_interrupts,armcpu_irq_phase);
  enableCPUTimeBase (false);
  uint64_t value = 0;
  __asm__ volatile ("msr icc_igrpen1_el1, %0; dsb sy; isb" :: "r" (value) : "memory");
}

void ArmCpu::haltCPU (void)
{
  armcpu_halt_calls=armcpu_halt_calls+1;
  quiesceCPU ();
  setCPUState (kIOCPUStateStopped);
  __asm__ volatile ("msr daifset, #15" ::: "memory");
  for (;;) { __asm__ volatile ("wfi"); }
}

// The new core runs with its MMU and caches off until start_cpu enables them, so it reads the reset vector, ResetHandlerData
// and the boot arguments from memory. Write those lines back to the point of coherency first: on this SoC the boot CPU's
// dirty cache lines are not guaranteed to be visible to a core with caches off (XNU only flushes the per-CPU data).
static void cleanToMemory (const void *start, size_t length)
{
  uint64_t ctr;
  __asm__ volatile ("mrs %0, ctr_el0" : "=r" (ctr));
  uintptr_t line = static_cast<uintptr_t> (4) << ((ctr >> 16) & 15);
  uintptr_t address = reinterpret_cast<uintptr_t> (start) & ~(line - 1), end = reinterpret_cast<uintptr_t> (start) + length;
  for (; address < end; address += line) { __asm__ volatile ("dc civac, %0" :: "r" (address) : "memory"); }
  __asm__ volatile ("dsb sy; isb" ::: "memory");
}

kern_return_t ArmCpu::startCPU (vm_offset_t, vm_offset_t)
{
  if (!secondary) { return KERN_NOT_SUPPORTED; }
  bool hvc = false;
  if (!psciConduit (&hvc)) { armcpu_smp_psci_rc[logicalId] = -1001; return KERN_NOT_SUPPORTED; }
  armcpu_smp_conduit = hvc ? 2 : 1;
  // PSCI 0.2 CPU_ON (SMC64 function id); the entry point is the kernel's physical reset vector page, whose reset
  // code selects this core's cpu_data entry by MPIDR affinity. The context id is unused by that code.
  uint64_t entry = static_cast<uint64_t> (ml_static_vtop (reinterpret_cast<vm_offset_t> (LowResetVectorBase)));
  cleanToMemory (LowResetVectorBase, 0x10000);   // reset vector, start_cpu and the code up to the MMU enable
  cleanToMemory (ResetHandlerData, 0x1000);
  cleanToMemory (const_boot_args, 0x1000);
  armcpu_smp_step[logicalId] = 4;
  int64_t result = psciCall (hvc, 0xc4000003ULL, affinity & 0xff00ffffffULL, entry, 0);
  armcpu_smp_psci_rc[logicalId] = static_cast<int32_t> (result);
  return result == 0 ? KERN_SUCCESS : KERN_FAILURE;
}

const OSSymbol *ArmCpu::getCPUName (void)
{
  return cpuName;
}

// Readback after configuration: bit 0 group 1, bit 1 enabled, bit 2 edge, bits 8..15 priority; IROUTER separately.
extern "C" IOReturn armcpu_spi_attach (uint32_t intid, bool edge, void (*handler) (void *), void *context,
                                       uint32_t *readback, uint64_t *router)
{
  if (gDist == 0 || armcpu_gic_ready == 0 || handler == nullptr) { return kIOReturnNotReady; }
  const unsigned interrupts = ((read32 (gDist, 4) & 31) + 1) * 32;
  if (intid < 32 || intid >= 1020 || intid >= interrupts) { return kIOReturnBadArgument; }
  const uint32_t used = __atomic_load_n (&gSpiUsed, __ATOMIC_ACQUIRE);
  for (uint32_t slot = 0; slot < used; slot++) {
    if (gSpi[slot].id == intid) { return kIOReturnExclusiveAccess; }
  }
  if (used >= ARMCPU_SPI_SLOTS) { return kIOReturnNoResources; }
  const uint32_t word = (intid / 32) * 4, bit = 1U << (intid % 32);
  write32 (gDist, 0x180 + word, bit); // ICENABLER while it is reconfigured
  if (!waitClear (gDist, 0, 1U << 31)) { return kIOReturnTimeout; }
  write32 (gDist, 0x80 + word, read32 (gDist, 0x80 + word) | bit); // IGROUPR: group 1 (RAZ/WI if secure owns it)
  *reinterpret_cast<volatile uint8_t *> (gDist + 0x400 + intid) = 0xa0;
  const unsigned cfg = 0xc00 + (intid / 16) * 4, shift = (intid % 16) * 2 + 1;
  write32 (gDist, cfg, (read32 (gDist, cfg) & ~(1U << shift)) | ((edge ? 1U : 0U) << shift));
  *reinterpret_cast<volatile uint64_t *> (gDist + 0x6000 + 8 * intid) = gBootAffinity;
  gSpi[used] = {intid, handler, context, 0};
  __atomic_store_n (&gSpiUsed, used + 1, __ATOMIC_RELEASE);
  __asm__ volatile ("dsb sy" ::: "memory");
  write32 (gDist, 0x100 + word, bit); // ISENABLER
  if (readback != nullptr) {
    *readback = ((read32 (gDist, 0x80 + word) & bit) ? 1U : 0U) | ((read32 (gDist, 0x100 + word) & bit) ? 2U : 0U) |
                (((read32 (gDist, cfg) >> shift) & 1U) << 2) |
                (static_cast<uint32_t> (*reinterpret_cast<volatile uint8_t *> (gDist + 0x400 + intid)) << 8);
  }
  if (router != nullptr) { *router = *reinterpret_cast<volatile uint64_t *> (gDist + 0x6000 + 8 * intid); }
  return kIOReturnSuccess;
}

extern "C" uint64_t armcpu_spi_count (uint32_t intid)
{
  const uint32_t used = __atomic_load_n (&gSpiUsed, __ATOMIC_ACQUIRE);
  for (uint32_t slot = 0; slot < used; slot++) {
    if (gSpi[slot].id == intid) { return gSpi[slot].count; }
  }
  return 0;
}
