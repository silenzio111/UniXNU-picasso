// SPDX-License-Identifier: BSD-2-Clause-Patent
// Link-only support for NativeLegacyDisplay. The M3 image does not enter the
// display lifecycle, so instrumentation remains inert and unsupported leaves
// fail loudly if they are ever reached.
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <libkern/libkern.h>
#include <mach/task.h>

extern "C" {

void native_legacy_display_probe(void)
{
	printf("NATIVE LEGACY: probe entry (link-only stub)\n");
}

void native_legacy_display_late_probe(void)
{
}

__attribute__((noreturn)) void
native_legacy_backend_guard(unsigned slot, void *object, void *caller)
{
	panic("NativeLegacyDisplay link-only guard reached: slot=%u object=%p caller=%p",
	    slot, object, caller);
}

bool native_legacy_gop_mapping_observed(IOService *, IOMemoryMap *, uint32_t,
    uint32_t)
{
	return false;
}

void native_legacy_registration_observed(IOService *)
{
}

void native_legacy_swap_stage(unsigned)
{
}

void native_legacy_client_enter(task_t, uint32_t)
{
}

void native_legacy_client_observed(IOReturn, IOUserClient *)
{
}

void native_legacy_pm_observed(unsigned, IOReturn, unsigned long)
{
}

void native_legacy_frame_observed(unsigned, uint64_t, uint64_t, uint64_t,
    uint64_t, uint64_t)
{
}

void native_legacy_frame_colors_observed(const uint32_t *, const uint64_t *,
    uint64_t)
{
}

void native_legacy_swap_validation_observed(bool, unsigned, const void *,
    const void *)
{
}

void native_legacy_surface_rejected(uint64_t, uint64_t, uint32_t, uint32_t,
    uint64_t, uint64_t, uint64_t)
{
}

bool native_legacy_prepare_scanout(void)
{
	return true;
}

void native_legacy_transfer_observed(IOReturn, const void *, uint64_t)
{
}

void native_legacy_copy_timing_observed(uint64_t, uint64_t)
{
}

void native_legacy_retirement_observed(IOService *)
{
}

bool native_legacy_gpu_offer(void *, IOMemoryDescriptor *, uint64_t, uint32_t,
    uint32_t)
{
	return false;
}

void native_legacy_gpu_fallback_observed(int)
{
}

void native_legacy_vsync_observed(unsigned, unsigned)
{
}

}
