// SPDX-License-Identifier: BSD-2-Clause-Patent
// Runtime glue for the owned Legacy display. The original IOMobileGraphics
// implementation remains at its audited addresses; this file only supplies
// observation hooks and validates the retained GOP mapping.
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSData.h>
#include <mach/task.h>
#include <pexpert/pexpert.h>
#include "picasso-display-gop.h"

extern "C" {

static bool screenReleased;
static IORegistryEntry *displayChosen;
static bool scaleAdded;
static bool rotationAdded;

static bool matchWord(IORegistryEntry *entry, const char *key, uint32_t value,
    bool &added) {
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

bool native_legacy_prepare_scanout(void) {
    if (!screenReleased) {
        if (PE_initialize_console(nullptr, kPEReleaseScreen)) return false;
        screenReleased = true;
        printf("PICASSO DISPLAY: text console released\n");
    }
    return true;
}

void native_legacy_backend_guard(unsigned slot, void *object, void *caller) {
    panic("NativeLegacyDisplay unsupported backend slot=%u object=%p caller=%p",
        slot, object, caller);
}

bool native_legacy_gop_mapping_observed(IOService *, IOMemoryMap *map,
    uint32_t width, uint32_t height) {
    PE_Video video = {};
    if (!map || !map->getVirtualAddress() || PE_current_console(&video) ||
        map->getPhysicalAddress() != video.v_baseAddr ||
        width != video.v_width || height != video.v_height ||
        !picasso_gop_video(video))
        return false;
    if (!displayChosen) displayChosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
    // Two scanout pixels per UI point is display policy, not physical DPI.
    bool ok = displayChosen &&
        matchWord(displayChosen, "display-scale", 2, scaleAdded) &&
        matchWord(displayChosen, "display-rotation", 0, rotationAdded);
    printf("PICASSO DISPLAY: mapped=%u base=%llx bytes=%llu width=%u height=%u stride=%lu depth=%lu\n",
        ok, map->getPhysicalAddress(), map->getLength(), width, height,
        video.v_rowBytes, video.v_depth);
    return ok;
}

void native_legacy_registration_observed(IOService *display) {
    printf("PICASSO DISPLAY: registered=%p framebuffer=%u\n",
        display, display && display->metaCast("IOMobileFramebuffer") != nullptr);
}

void native_legacy_client_enter(task_t, uint32_t type) {
    printf("PICASSO DISPLAY: client enter type=%u\n", type);
}

void native_legacy_client_observed(IOReturn status, IOUserClient *client) {
    printf("PICASSO DISPLAY: client result=%d client=%p\n", status, client);
}

void native_legacy_pm_observed(unsigned stage, IOReturn status,
    unsigned long state) {
    printf("PICASSO DISPLAY: pm stage=%u status=%d state=%lu\n",
        stage, status, state);
}

void native_legacy_swap_validation_observed(bool ok, unsigned reason,
    const void *, const void *) {
    printf("PICASSO DISPLAY: swap validation=%u reason=%u\n", ok, reason);
}

void native_legacy_surface_rejected(uint64_t width, uint64_t height,
    uint32_t format, uint32_t planes, uint64_t stride, uint64_t allocation,
    uint64_t length) {
    printf("PICASSO DISPLAY: surface rejected width=%llu height=%llu format=%x planes=%u stride=%llu allocation=%llu length=%llu\n",
        width, height, format, planes, stride, allocation, length);
}

void native_legacy_transfer_observed(IOReturn status, const void *request,
    uint64_t bytes) {
    printf("PICASSO DISPLAY: transfer status=%d request=%p bytes=%llu\n",
        status, request, bytes);
}

void native_legacy_copy_timing_observed(uint64_t ticks, uint64_t bytes) {
    printf("PICASSO DISPLAY: copy ticks=%llu bytes=%llu\n", ticks, bytes);
}

void native_legacy_retirement_observed(IOService *display) {
    printf("PICASSO DISPLAY: retirement display=%p\n", display);
}

void native_legacy_retirement_failed(IOService *display) {
    printf("PICASSO DISPLAY: retirement failed display=%p\n", display);
}

void native_legacy_vsync_observed(unsigned event, unsigned hz) {
    printf("PICASSO DISPLAY: vsync event=%u hz=%u\n", event, hz);
}

void native_legacy_frame_observed(unsigned kind, uint64_t pixels,
    uint64_t minx, uint64_t miny, uint64_t maxx, uint64_t maxy) {
    printf("PICASSO DISPLAY: frame kind=%u pixels=%llu box=%llu,%llu-%llu,%llu\n",
        kind, pixels, minx, miny, maxx, maxy);
}

void native_legacy_frame_colors_observed(const uint32_t *colors,
    const uint64_t *counts, uint64_t other) {
    if (!colors || !counts) return;
    printf("PICASSO DISPLAY: colors=%08x/%llu %08x/%llu %08x/%llu %08x/%llu other=%llu\n",
        colors[0], counts[0], colors[1], counts[1], colors[2], counts[2],
        colors[3], counts[3], other);
}

void native_legacy_swap_stage(unsigned stage) {
    printf("PICASSO DISPLAY: swap stage=%u\n", stage);
}

bool native_legacy_gpu_offer(void *, IOMemoryDescriptor *, uint64_t, uint32_t,
    uint32_t) {
    return false;
}

void native_legacy_gpu_fallback_observed(int status) {
    printf("PICASSO DISPLAY: GPU fallback status=%d\n", status);
}

void native_legacy_display_probe(void) {
    // The USB command path initializes the class explicitly before allocation.
    printf("NATIVE LEGACY: display probe deferred to register-display/publish-display\n");
}

void native_legacy_display_late_probe(void) {
    printf("NATIVE LEGACY: display late probe has no pending object\n");
}

}
