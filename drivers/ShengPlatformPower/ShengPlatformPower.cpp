// SPDX-License-Identifier: MIT
// Board-side original PMGR ABI qualification. Default off. Modes: 1 releases
// unpublished objects, 2 publishes the original nub, 3 tests power refusal.
// No clocks, sleep, reset or secondary-CPU success is advertised.
#include "OriginalPMGRABI.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSKext.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSData.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>
#include "picasso-display-gop.h"

extern "C" {
extern kmod_info_t g_kernel_kmod_info;
kern_return_t OSKextLoadKextWithIdentifier(const char *);
void sheng_pmgr_static_init(void) __asm__("__GLOBAL__sub_I_ShengPlatformPower.cpp");
IOService *sheng_original_pmgr_nub_create(IORegistryEntry *, ApplePMGR *);
uint32_t sheng_original_pmgr_nub_read(IOService *);
void sheng_original_pmgr_nub_write(IOService *, uint64_t);
// stage,status,parent-size,own-size,provider,nub,bound-provider,read-before,
// read-after,own-before,own-after,nub-before,nub-after,attached,registered,mode.
volatile uint64_t sheng_pmgr_observation[16];
__attribute__((noinline)) void sheng_pmgr_checkpoint(void) { __asm__ volatile("" ::: "memory"); }
void native_legacy_display_probe(void) __attribute__((weak));
bool native_legacy_display_probe_ready(void);
void picasso_keystore_probe(void) __attribute__((weak));
bool picasso_aks_ready(void) __attribute__((weak));
static IOService *publishedDisplay;
static IOService *publishedNub;
OSReturn oskext_start_direct(OSKext *kext, bool start_dependencies)
    __asm__("__ZN6OSKext5startEb");
}

__attribute__((weak)) void native_legacy_display_probe(void)
{
  printf("NATIVE LEGACY: unavailable; built without legacy display\n");
}

extern "C" void native_legacy_display_static_init(void)
    __asm__("__GLOBAL__sub_I_NativeLegacyDisplay.cpp");
struct NativeLegacyMetaStorage;
extern NativeLegacyMetaStorage NativeLegacyDisplay_gMetaClass
    __asm("__ZN19NativeLegacyDisplay10gMetaClassE");

// Step 2b: run the owned display class's stripped static initializer inside
// XNU's own preModLoad/postModLoad window, exactly like the runtime probe.
// Until this runs, gMetaClass storage is zero (the archive ships it without
// __mod_init_func), so allocate() yields an object no metaCast accepts.
extern "C" kern_return_t
native_register_display_and_report(void)
{
  if (picasso_keystore_probe) picasso_keystore_probe();
  if (picasso_aks_ready) {
    printf("PICASSO_AKS ready=%u at CDC register\n", picasso_aks_ready() ? 1u : 0u);
  }
  static bool registered = false;
  const OSMetaClass *meta = reinterpret_cast<const OSMetaClass *>(
      &NativeLegacyDisplay_gMetaClass);
  if (registered) {
    const OSMetaClass *super = meta->getSuperClass();
    printf("NATIVE DISPLAY REG: already registered meta=%p super=%s size=%u\n",
        meta, super ? super->getClassName() : "missing",
        meta->getClassSize());
    return kOSReturnSuccess;
  }
  if (native_legacy_display_probe_ready()) {
    const OSMetaClass *super = meta->getSuperClass();
    printf("NATIVE DISPLAY REG: early probe already initialized meta=%p super=%s size=%u\n",
        meta, super ? super->getClassName() : "missing",
        meta->getClassSize());
    registered = true;
    return kOSReturnSuccess;
  }
  /*
   * The owned display is part of the prelinked kernel, not an independently
   * started kext. The module pre/post hooks re-register the kernel's sysctl
   * table (and panic on the already-used slot), so run only the C++ static
   * initializer that constructs NativeLegacyDisplay::gMetaClass.
   */
  printf("NATIVE DISPLAY REG: direct static init\n");
  native_legacy_display_static_init();
  const OSMetaClass *super = meta->getSuperClass();
  printf("NATIVE DISPLAY REG: static init meta=%p super=%s size=%u\n",
      meta, super ? super->getClassName() : "missing",
      meta->getClassSize());
  if (super) {
    registered = true;
    return kOSReturnSuccess;
  }
  return kOSReturnError;
}

extern "C" kern_return_t
native_start_kext_and_report(const char *identifier, const char *class_name)
{
  auto kext = OSKext::lookupKextWithIdentifier(identifier);
  if (!kext) {
    printf("NATIVE KEXT START: lookup failed id=%s\n", identifier);
    return kOSKextReturnNotFound;
  }
  printf("NATIVE KEXT START: begin id=%s loaded=%u started=%u\n",
      identifier, kext->isLoaded() ? 1u : 0u, kext->isStarted() ? 1u : 0u);
  // Prelinked kexts are present but not loaded; start() alone refuses them
  // with kOSKextReturnInvalidArgument. Go through load, which loads and then
  // starts (default startOpt) via the same OSKext::start path.
  kern_return_t result = kext->isLoaded() ? oskext_start_direct(kext, true) :
      OSKext::loadKextWithIdentifier(identifier);
  const OSSymbol *symbol = OSSymbol::withCString(class_name);
  const OSMetaClass *meta = symbol ?
      OSMetaClass::getMetaClassWithName(symbol) : nullptr;
  OSSafeReleaseNULL(symbol);
  const OSMetaClass *super = meta ? meta->getSuperClass() : nullptr;
  printf("NATIVE KEXT START: result=%d meta=%p class=%s size=%u super=%s\n",
      result, meta, meta ? meta->getClassName() : "missing",
      meta ? meta->getClassSize() : 0,
      super ? super->getClassName() : "missing");
  return result;
}

extern "C" IOService *native_legacy_display_allocate(void);
extern "C" bool native_legacy_surface_ready(void);

// Step 3, staged: level 0 only allocates/initializes the owned Legacy display
// and reports its type identity; level 1 additionally attaches it to the
// platform, starts it and registers the service. Each stage prints before the
// next one runs, so a backend-guard panic still leaves the last good stage in
// the CDC log.
extern "C" kern_return_t
native_publish_display_and_report(int level)
{
  IOService *display = native_legacy_display_allocate();
  printf("NATIVE DISPLAY: allocate object=%p\n", display);
  if (!display) {
    return kOSReturnError;
  }
  const OSMetaClass *object_meta = display->getMetaClass();
  printf("NATIVE DISPLAY: meta=%p super=%p size=%u\n", object_meta,
      object_meta ? object_meta->getSuperClass() : nullptr,
      object_meta ? object_meta->getClassSize() : 0);
  bool legacy = display->metaCast("IOMobileFramebufferLegacy") != nullptr;
  bool framebuffer = display->metaCast("IOMobileFramebuffer") != nullptr;
  bool service = display->metaCast("IOService") != nullptr;
  printf("NATIVE DISPLAY: cast legacy=%u framebuffer=%u service=%u\n",
      legacy, framebuffer, service);
  if (!legacy || !framebuffer || !service) {
    display->release();
    return kOSKextReturnValidation;
  }
  bool initialized = display->init();
  printf("NATIVE DISPLAY: init=%u\n", initialized ? 1u : 0u);
  if (!initialized) {
    display->release();
    return kOSReturnError;
  }
  if (level < 1) {
    display->release();
    printf("NATIVE DISPLAY: level=0 complete, released\n");
    return kOSReturnSuccess;
  }
  if (!native_legacy_surface_ready()) {
    printf("NATIVE DISPLAY: IOSurface/IOCoreSurfaceRoot unavailable; continuing to provider probe\n");
  }
  IOService *parent = IOService::getResourceService();
  IORegistryEntry *treeParent = IORegistryEntry::fromPath("/arm-io", gIODTPlane);
  IOService *nub = nullptr;
  OSObject *armio_object = OSMetaClass::allocClassWithName("AppleARMIODevice");
  if (armio_object) {
    nub = OSDynamicCast(IOService, armio_object);
    if (nub) {
      printf("NATIVE DISPLAY: AppleARMIODevice alloc=%p class=%s\n",
          nub, nub->getMetaClass()->getClassName());
    } else {
      OSSafeReleaseNULL(armio_object);
    }
  }
  if (!nub) {
    nub = new IOPlatformDevice;
    printf("NATIVE DISPLAY: AppleARMIODevice unavailable; fallback IOPlatformDevice=%p\n",
        nub);
  }
  PE_Video video = {};
  bool videoValid = !PE_current_console(&video) && picasso_gop_video(video);
  IODeviceMemory *memory = videoValid ?
      IODeviceMemory::withRange(video.v_baseAddr,
          (IOByteCount)video.v_rowBytes * video.v_height) : nullptr;
  OSArray *ranges = OSArray::withCapacity(1);
  const char type[] = "display-subsystem";
  OSData *typeData = OSData::withBytes(type, sizeof(type));
  printf("NATIVE DISPLAY: parent=%p armio-tree=%p video=%u base=%lx width=%lu height=%lu stride=%lu depth=%lu\n",
      parent, treeParent, videoValid ? 1u : 0u, video.v_baseAddr,
      video.v_width, video.v_height, video.v_rowBytes, video.v_depth);
  bool nubAttached = false, nubStarted = false, dtAttached = false;
  bool attached = false, started = false;
  if (nub && memory && ranges && treeParent && typeData && parent &&
      nub->init() && ranges->setObject(memory)) {
    nub->setDeviceMemory(ranges);
    nub->setName("NativeGOPFramebuffer");
    nubAttached = nub->attach(parent);
    if (nubAttached) nubStarted = nub->start(parent);
    if (nubStarted && nub->setProperty("device_type", typeData)) {
      nub->setName("gop-display", gIODTPlane);
      dtAttached = nub->attachToParent(treeParent, gIODTPlane);
      SInt32 score = 0;
      if (dtAttached && display->probe(nub, &score) == display)
        attached = display->attach(nub);
      if (attached) started = display->start(nub);
    }
  }
  printf("NATIVE DISPLAY: nub attach=%u start=%u dt=%u display attach=%u start=%u\n",
      nubAttached ? 1u : 0u, nubStarted ? 1u : 0u, dtAttached ? 1u : 0u,
      attached ? 1u : 0u, started ? 1u : 0u);
  if (started) {
    display->registerService(kIOServiceSynchronous);
    printf("NATIVE DISPLAY: registered provider=%p state=0x%x\n",
        display->getProvider(), display->getState());
    publishedDisplay = display;
    publishedNub = nub;
  } else {
    if (attached) display->detach(nub);
    if (dtAttached) nub->detachFromParent(treeParent, gIODTPlane);
    if (nubStarted) nub->stop(parent);
    if (nubAttached) nub->detach(parent);
    display->release();
    OSSafeReleaseNULL(nub);
  }
  OSSafeReleaseNULL(typeData);
  OSSafeReleaseNULL(ranges);
  OSSafeReleaseNULL(memory);
  OSSafeReleaseNULL(treeParent);
  return started ? kOSReturnSuccess : kOSReturnError;
}

__attribute__((noreturn)) static void unavailable(unsigned slot)
{
  sheng_pmgr_observation[0] = 0x100 + slot;
  sheng_pmgr_observation[1] = static_cast<uint32_t>(kIOReturnUnsupported);
  sheng_pmgr_checkpoint();
  panic("ShengPlatformPower: unimplemented operation %u", slot);
}

class ShengPlatformPower : public ApplePMGR {
  OSDeclareDefaultStructors(ShengPlatformPower)
public:
  void enableCPUCore(unsigned, uint64_t) override { unavailable(168); }
  void enableCPUCore(unsigned) override { unavailable(169); }
  void disableCPUCore(unsigned) override { unavailable(170); }
  void enableCPUCluster(unsigned) override { unavailable(171); }
  void disableCPUCluster(unsigned) override { unavailable(172); }
  void initCPUIdle(ml_processor_info_t *) override { unavailable(173); }
  void enterCPUIdle(UInt64 *) override { unavailable(174); }
  void exitCPUIdle(UInt64 *) override { unavailable(175); }
  void updateCPUIdle(UInt64 *) override { unavailable(176); }
#include "GuardedPMGRSlots.inc"
};
OSDefineMetaClassWithInit(ShengPlatformPower, ApplePMGR, )
OSMetaClassConstructorInit(ShengPlatformPower, ApplePMGR, )
OSDefineBasicStructors(ShengPlatformPower, ApplePMGR)
// The original object's storage exceeds kalloc_type's 16 KiB size limit.
// Use XNU's existing zeroed, size-aware OSObject heap allocation pair.
void *ShengPlatformPower::operator new(size_t size) { return OSObject::operator new(size); }
void ShengPlatformPower::operator delete(void *memory, size_t size)
{
  OSObject::operator delete(memory, size);
}
static_assert(sizeof(ShengPlatformPower) == 0x4b8a8, "Exact original provider storage");

static const OSMetaClass *lookup(const char *text)
{
  const OSSymbol *name = OSSymbol::withCString(text);
  const OSMetaClass *meta = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
  OSSafeReleaseNULL(name);
  return meta;
}

extern "C" void sheng_pmgr_run(void)
{
  {
    const boot_args *args = static_cast<const boot_args *>(PE_state.bootArgs);
    if (args == nullptr) {
      printf("PICASSO_MEMORY boot args=missing\n");
    } else {
      printf("PICASSO_MEMORY boot phys_base=%llx bytes=%llx actual=%llx top=%llx\n",
          (unsigned long long)args->physBase,
          (unsigned long long)args->memSize,
          (unsigned long long)args->memSizeActual,
          (unsigned long long)args->topOfKernelData);
    }
  }
  /*
   * Load the original family before the synchronous USB window. The owned
   * subclass lifecycle remains exclusively CDC-triggered: probing it here
   * would initialize its metaclass before the later register-display command
   * and make a second postModLoad assertion-prone.
   */
  unsigned legacy = 0;
  if (PE_parse_boot_argn("offline-legacy-register", &legacy, sizeof(legacy)) && legacy) {
    extern kern_return_t OSKextLoadKextWithIdentifier(const char *);
    kern_return_t graphics = OSKextLoadKextWithIdentifier("com.apple.iokit.IOMobileGraphicsFamily");
    printf("NATIVE LEGACY: graphics load status=%d\n", graphics);
  }
  unsigned mode = 0;
  if (!PE_parse_boot_argn("sheng-pmgr", &mode, sizeof(mode)) || !mode) return;
  sheng_pmgr_observation[15] = mode;
  sheng_pmgr_observation[1] = static_cast<uint32_t>(kIOReturnError);
  if (mode > 3 || sheng_pmgr_observation[0]) { sheng_pmgr_checkpoint(); return; }
  sheng_pmgr_observation[0] = 1;
  kern_return_t result = OSKextLoadKextWithIdentifier("com.apple.driver.ApplePMGR");
  if (result) { sheng_pmgr_observation[1] = static_cast<uint32_t>(result); sheng_pmgr_checkpoint(); return; }
  const OSMetaClass *parent = lookup("ApplePMGR"), *nubMeta = lookup("ApplePMGRNub");
  if (!parent || parent != &ApplePMGR::gMetaClass || parent->getClassSize() != 0x4b8a8 ||
      !nubMeta || nubMeta->getClassSize() != 0x740) { sheng_pmgr_checkpoint(); return; }
  sheng_pmgr_observation[2] = parent->getClassSize();
  void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
  if (!handle) { sheng_pmgr_checkpoint(); return; }
  sheng_pmgr_static_init();
  result = OSMetaClass::postModLoad(handle);
  if (result) { sheng_pmgr_observation[1] = static_cast<uint32_t>(result); sheng_pmgr_checkpoint(); return; }
  sheng_pmgr_observation[9] = ShengPlatformPower::gMetaClass.getInstanceCount();
  sheng_pmgr_observation[11] = nubMeta->getInstanceCount();
  ShengPlatformPower *provider = new ShengPlatformPower;
  IORegistryEntry *description = new IORegistryEntry;
  IOService *nub = nullptr;
  do {
    if (!provider || !description || !provider->init() || !description->init()) break;
    sheng_pmgr_observation[0] = 2;
    sheng_pmgr_observation[3] = provider->getMetaClass()->getClassSize();
    sheng_pmgr_observation[4] = reinterpret_cast<uintptr_t>(provider);
    // Empty resource description: no Apple register map or device capabilities.
    description->setName("sheng-platform-power");
    nub = sheng_original_pmgr_nub_create(description, provider);
    sheng_pmgr_observation[5] = reinterpret_cast<uintptr_t>(nub);
    if (!nub || nub->getMetaClass() != nubMeta || !provider->metaCast("ApplePMGR")) break;
    uintptr_t bound = 0;
    memcpy(&bound, reinterpret_cast<const char *>(nub) + 0x88, sizeof(bound));
    sheng_pmgr_observation[6] = bound;
    if (bound != reinterpret_cast<uintptr_t>(provider)) break;
    sheng_pmgr_observation[0] = 3;
    uint32_t value = sheng_original_pmgr_nub_read(nub);
    sheng_pmgr_observation[7] = value;
    sheng_original_pmgr_nub_write(nub, uint64_t(value | 4));
    sheng_pmgr_observation[8] = sheng_original_pmgr_nub_read(nub);
    sheng_pmgr_observation[13] = nub->getProvider() != nullptr || provider->getProvider() != nullptr;
    sheng_pmgr_observation[14] = (nub->getState() | provider->getState()) & kIOServiceRegisteredState;
    if (value != 0xffffffff || sheng_pmgr_observation[8] != 0xffffffff ||
        sheng_pmgr_observation[13] || sheng_pmgr_observation[14]) break;
    if (mode == 3) provider->disableCPUCore(1);
    if (mode == 2) {
      IOService *platform = IOService::getPlatform();
      if (!platform || !provider->attach(platform)) break;
      if (!provider->start(platform)) { provider->detach(platform); break; }
      if (!nub->attach(provider)) {
        provider->stop(platform); provider->detach(platform); break;
      }
      if (!nub->start(provider)) {
        nub->detach(provider); provider->stop(platform); provider->detach(platform); break;
      }
      provider->registerService(kIOServiceSynchronous);
      nub->registerService(kIOServiceSynchronous);
      sheng_pmgr_observation[0] = 5;
      sheng_pmgr_observation[13] = nub->getProvider() == provider && provider->getProvider() == platform;
      sheng_pmgr_observation[14] = (nub->getState() & provider->getState()) & kIOServiceRegisteredState;
      auto matching = IOService::serviceMatching("ApplePMGRNub");
      IOService *matched = matching ? IOService::waitForMatchingService(matching, 1000000) : nullptr;
      OSSafeReleaseNULL(matching);
      // Check actual service discovery and genuine type identity, then retain
      // both live objects. The registry and original nub refer to this provider.
      if (matched != nub || matched->getMetaClass() != nubMeta ||
          !sheng_pmgr_observation[13] || !sheng_pmgr_observation[14]) {
        sheng_pmgr_checkpoint();
        panic("ShengPlatformPower: original nub publication failed");
      }
      sheng_pmgr_observation[10] = ShengPlatformPower::gMetaClass.getInstanceCount();
      sheng_pmgr_observation[12] = nubMeta->getInstanceCount();
      sheng_pmgr_observation[1] = kIOReturnSuccess;
      OSSafeReleaseNULL(matched);
      OSSafeReleaseNULL(description);
      sheng_pmgr_checkpoint();
      return;
    }
    sheng_pmgr_observation[1] = kIOReturnSuccess;
  } while (false);
  OSSafeReleaseNULL(nub);
  OSSafeReleaseNULL(description);
  OSSafeReleaseNULL(provider);
  sheng_pmgr_observation[10] = ShengPlatformPower::gMetaClass.getInstanceCount();
  sheng_pmgr_observation[12] = nubMeta->getInstanceCount();
  if (sheng_pmgr_observation[9] != sheng_pmgr_observation[10] ||
      sheng_pmgr_observation[11] != sheng_pmgr_observation[12]) sheng_pmgr_observation[1] = static_cast<uint32_t>(kIOReturnError);
  sheng_pmgr_observation[0] = 4;
  sheng_pmgr_checkpoint();
}
