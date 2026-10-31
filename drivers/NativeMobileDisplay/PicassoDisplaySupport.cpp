// SPDX-License-Identifier: BSD-2-Clause-Patent
// Firmware-backed CPU display. No Qualcomm display registers are changed.
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOUserClient.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSKext.h>
#include <libkern/OSKextLibPrivate.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>
#include "PicassoGop.h"

extern "C" {
extern kmod_info_t g_kernel_kmod_info;
void native_legacy_display_static_init(void) __asm__("__GLOBAL__sub_I_NativeLegacyDisplay.cpp");
IOService *native_legacy_display_allocate(void);
void picasso_keystore_probe(void) __attribute__((weak));
bool picasso_aks_ready(void) __attribute__((weak));
}

static IOService *pendingDisplay;
static IOService *activeDisplay;
static IOPlatformDevice *activeNub;
static bool initialized, screenReleased;
static unsigned reportCounts[12][2];
static IORegistryEntry *displayChosen;
static bool scaleAdded, rotationAdded;
static IOService *surfaceRoot;

extern "C" bool native_legacy_display_probe_ready(void) {
    return initialized && (pendingDisplay != nullptr || activeDisplay != nullptr);
}

static IOService *create_surface_root(void) {
    if (surfaceRoot) return surfaceRoot;
    OSObject *object = OSMetaClass::allocClassWithName("IOSurfaceRoot");
    IOService *root = object ? OSDynamicCast(IOService, object) : nullptr;
    if (!root) {
        OSSafeReleaseNULL(object);
        printf("PICASSO_DISPLAY: IOSurfaceRoot class unavailable\n");
        return nullptr;
    }
    IOService *parent = IOService::getResourceService();
    bool attached = root->init() && parent && root->attach(parent) &&
        root->start(parent);
    printf("PICASSO_DISPLAY: IOSurfaceRoot alloc=%p parent=%p init/attach/start=%u class=%s\n",
        root, parent, attached ? 1u : 0u,
        root->getMetaClass() ? root->getMetaClass()->getClassName() : "missing");
    if (!attached) {
        if (root->getProvider()) root->detach(parent);
        root->release();
        return nullptr;
    }
    root->registerService(kIOServiceSynchronous);
    surfaceRoot = root;
    return root;
}

static unsigned takeReport(unsigned *counter) {
    unsigned value = __atomic_load_n(counter, __ATOMIC_RELAXED);
    while (value < 4) {
        if (__atomic_compare_exchange_n(counter, &value, value + 1, false,
                __ATOMIC_RELAXED, __ATOMIC_RELAXED)) return value + 1;
    }
    return 0;
}

// Bounded per-event output, including failures, without logging every frame.
static void report(unsigned event, uint64_t a, uint64_t b, uint64_t c, bool failed = false) {
    if (event >= 12) return;
    unsigned sample = takeReport(&reportCounts[event][failed ? 1 : 0]);
    if (!sample) return;
    printf("PICASSO_DISPLAY event=%u sample=%u a=%llx b=%llx c=%llx failure=%u\n",
        event, sample, a, b, c, failed);
}

extern "C" bool native_legacy_prepare_scanout(void) {
    if (!screenReleased) {
        // Release the real XNU text console before the first system frame.
        // CDC owns its independent console ring and remains available.
        if (PE_initialize_console(nullptr, kPEReleaseScreen)) return false;
        screenReleased = true;
        report(8, 1, 0, 0);
    }
    return true;
}

extern "C" __attribute__((noreturn)) void native_legacy_backend_guard(unsigned slot, void *, void *caller) {
    panic("PICASSO_DISPLAY unsupported backend slot=%u caller=%p", slot, caller);
}
extern "C" void native_legacy_registration_observed(IOService *display) {
    report(1, reinterpret_cast<uintptr_t>(display), display->metaCast("IOMobileFramebuffer") != nullptr, 0);
}
extern "C" void native_legacy_client_enter(task_t, uint32_t type) { report(3, type, 0, 0); }
extern "C" void native_legacy_client_observed(IOReturn status, IOUserClient *client) {
    report(4, status, client != nullptr, 0, status != kIOReturnSuccess);
}
extern "C" void native_legacy_pm_observed(unsigned stage, IOReturn status, unsigned long state) {
    report(5, stage, status, state, status != kIOReturnSuccess);
}
extern "C" void native_legacy_swap_validation_observed(bool ok, unsigned reason, const void *, const void *) {
    report(6, ok, reason, 0, !ok);
}
extern "C" void native_legacy_surface_rejected(uint64_t width, uint64_t height, uint32_t format,
    uint32_t planes, uint64_t stride, uint64_t allocation, uint64_t length) {
    static unsigned count;
    if (takeReport(&count)) printf("PICASSO_DISPLAY surface rejected width=%llu height=%llu format=%x planes=%u stride=%llu allocation=%llu length=%llu\n",
        width, height, format, planes, stride, allocation, length);
}
extern "C" void native_legacy_transfer_observed(IOReturn status, const void *, uint64_t bytes) { report(7, status, bytes, 0, status != kIOReturnSuccess); }
extern "C" void native_legacy_retirement_failed(IOService *) { report(9, 0, 0, 0, true); }
extern "C" void native_legacy_retirement_observed(IOService *) { report(9, 1, 0, 0); }
extern "C" void native_legacy_copy_timing_observed(uint64_t ticks, uint64_t bytes) { report(10, ticks, bytes, 0); }
extern "C" void native_legacy_vsync_observed(unsigned event, unsigned hz) { report(11, event, hz, 0); }
extern "C" void native_legacy_frame_observed(unsigned kind, uint64_t pixels, uint64_t, uint64_t, uint64_t, uint64_t) {
    static unsigned count;
    if (takeReport(&count)) printf("PICASSO_DISPLAY frame kind=%u nonblack=%llu\n", kind, pixels);
}

static void finishDisplayPolicy(bool keep) {
    if (displayChosen && !keep) {
        if (scaleAdded) displayChosen->removeProperty("display-scale");
        if (rotationAdded) displayChosen->removeProperty("display-rotation");
    }
    OSSafeReleaseNULL(displayChosen);
    scaleAdded = rotationAdded = false;
}

static bool matchWord(IORegistryEntry *entry, const char *key, uint32_t value, bool &added) {
    OSObject *old = entry->getProperty(key);
    if (old) {
        OSData *data = OSDynamicCast(OSData, old);
        uint32_t actual = 0;
        if (!data || data->getLength() != sizeof(actual)) return false;
        memcpy(&actual, data->getBytesNoCopy(), sizeof(actual));
        return actual == value;
    }
    OSData *data = OSData::withBytes(&value, sizeof(value));
    bool ok = data && entry->setProperty(key, data);
    OSSafeReleaseNULL(data);
    added = ok;
    return ok;
}

// The original framebuffer start path expects the IOSurface family to have
// published IOCoreSurfaceRoot before it can attach its client. Keep this
// bounded and cached: a CDC-triggered probe must not repeat a five-second wait
// or turn a missing optional service into an unbounded boot dependency.
extern "C" bool native_legacy_surface_ready(void) {
    static unsigned state; // 0 unknown/retryable, 1 ready
    if (state == 1) return true;
    OSReturn status = OSKext::loadKextWithIdentifier(
        "com.apple.iokit.IOSurface", true, false,
        kOSKextExcludeNone, kOSKextExcludeAll, nullptr);
    OSDictionary *matching = IOService::nameMatching("IOCoreSurfaceRoot");
    IOService *surface = matching ?
        IOService::waitForMatchingService(matching, 5000000000ULL) : nullptr;
    bool found = surface != nullptr;
    OSSafeReleaseNULL(surface);
    OSSafeReleaseNULL(matching);
    if (!found) {
        printf("PICASSO_DISPLAY: IOCoreSurfaceRoot missing; creating IOSurfaceRoot\n");
        (void)create_surface_root();
        matching = IOService::nameMatching("IOCoreSurfaceRoot");
        surface = matching ?
            IOService::waitForMatchingService(matching, 5000000000ULL) : nullptr;
        found = surface != nullptr;
        OSSafeReleaseNULL(surface);
        OSSafeReleaseNULL(matching);
    }
    report(0, 2, status, found);
    state = found ? 1 : 0;
    return found;
}

extern "C" bool native_legacy_gop_mapping_observed(IOService *, IOMemoryMap *map, uint32_t width, uint32_t height) {
    PE_Video video = {};
    if (PE_current_console(&video) || map->getPhysicalAddress() != video.v_baseAddr ||
        width != video.v_width || height != video.v_height) return false;
    if (displayChosen) return false;
    displayChosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
    // Two scanout pixels per UI point. This is UI policy, not physical DPI.
    bool ok = displayChosen && matchWord(displayChosen, "display-scale", 2, scaleAdded) &&
        matchWord(displayChosen, "display-rotation", 0, rotationAdded);
    if (!ok) finishDisplayPolicy(false);
    printf("PICASSO_DISPLAY mapped ok=%u base=%llx bytes=%llx width=%u height=%u stride=%lu depth=%lu console=%lu scale=2\n",
        ok, map->getPhysicalAddress(), map->getLength(), width, height,
        video.v_rowBytes, video.v_depth, video.v_display);
    return ok;
}

extern "C" void native_legacy_display_probe(void) {
    if (picasso_keystore_probe) picasso_keystore_probe();
    if (picasso_aks_ready) printf("PICASSO_AKS ready=%u at display probe\n", picasso_aks_ready() ? 1u : 0u);
    if (initialized) return;
    initialized = true;
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) { report(0, 1, kIOReturnNoMemory, 0); return; }
    native_legacy_display_static_init();
    OSReturn status = OSMetaClass::postModLoad(handle);
    report(0, 1, status, 0);
    if (status) return;
    IOService *display = native_legacy_display_allocate();
    if (!display || !display->metaCast("IOMobileFramebufferLegacy") || !display->init()) {
        OSSafeReleaseNULL(display); return;
    }
    pendingDisplay = display;
}

extern "C" void native_legacy_display_late_probe(void) {
    if (picasso_keystore_probe) picasso_keystore_probe();
    IOService *display = pendingDisplay;
    if (!display) return;
    pendingDisplay = nullptr;
    const OSReturn status = OSKext::loadKextWithIdentifier(
        "com.apple.iokit.IOSurface", true, false,
        kOSKextExcludeNone, kOSKextExcludeAll, nullptr);
    OSDictionary *matching = IOService::nameMatching("IOCoreSurfaceRoot");
    IOService *surface = matching ? IOService::waitForMatchingService(matching, 5000000000ULL) : nullptr;
    bool found = surface != nullptr;
    OSSafeReleaseNULL(surface); OSSafeReleaseNULL(matching);
    report(0, 2, status, found);
    PE_Video video = {};
    if (status || !found || PE_current_console(&video) ||
        !picasso_gop_video(video)) {
        printf("PICASSO_DISPLAY handoff rejected base=%lx width=%lu height=%lu stride=%lu depth=%lu rotation=%u\n",
            video.v_baseAddr, video.v_width, video.v_height, video.v_rowBytes, video.v_depth, video.v_rotate);
        display->release(); return;
    }
    IOService *parent = IOService::getResourceService();
    IOPlatformDevice *nub = new IOPlatformDevice;
    IODeviceMemory *memory = IODeviceMemory::withRange(video.v_baseAddr, video.v_rowBytes * video.v_height);
    OSArray *ranges = OSArray::withCapacity(1);
    IORegistryEntry *treeParent = IORegistryEntry::fromPath("/arm-io", gIODTPlane);
    const char type[] = "display-subsystem";
    OSData *typeData = OSData::withBytes(type, sizeof(type));
    bool nubAttached = false, nubStarted = false, dtAttached = false, attached = false, started = false;
    if (nub && memory && ranges && treeParent && typeData && parent && nub->init() && ranges->setObject(memory)) {
        nub->setDeviceMemory(ranges); nub->setName("NativeGOPFramebuffer");
        nubAttached = nub->attach(parent);
        if (nubAttached) nubStarted = nub->start(parent);
        if (nubStarted && nub->setProperty("device_type", typeData)) {
            nub->setName("gop-display", gIODTPlane);
            dtAttached = nub->attachToParent(treeParent, gIODTPlane);
            SInt32 score = 0;
            if (dtAttached && display->probe(nub, &score) == display) attached = display->attach(nub);
            if (attached) started = display->start(nub);
        }
    }
    report(0, 3, started, dtAttached);
    finishDisplayPolicy(started);
    OSSafeReleaseNULL(typeData); OSSafeReleaseNULL(ranges); OSSafeReleaseNULL(memory);
    if (started) { activeDisplay = display; activeNub = nub; }
    else {
        if (attached) display->detach(nub);
        if (dtAttached) nub->detachFromParent(treeParent, gIODTPlane);
        if (nubStarted) nub->stop(parent);
        if (nubAttached) nub->detach(parent);
        OSSafeReleaseNULL(nub); display->release();
    }
    OSSafeReleaseNULL(treeParent);
}
