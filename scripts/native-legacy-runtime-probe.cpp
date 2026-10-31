// SPDX-License-Identifier: BSD-2-Clause-Patent
// Bounded runtime qualification for the owned Legacy framebuffer. The probe
// allocates and initializes the object, then releases it without attaching to
// or publishing a display service.
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSMetaClass.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>

extern "C" {
extern kmod_info_t g_kernel_kmod_info;
void native_legacy_display_static_init(void)
    __asm__("__GLOBAL__sub_I_NativeLegacyDisplay.cpp");
IOService *native_legacy_display_allocate(void);

volatile uint32_t native_legacy_stage;
volatile int native_legacy_meta_status = -1;
volatile uint32_t native_legacy_init_result;
volatile uint32_t native_legacy_parent_match;
volatile uint32_t native_legacy_object_size;
volatile uint32_t native_legacy_released;
volatile uint64_t native_legacy_object;
volatile uint32_t native_legacy_guard_slot;
volatile uint64_t native_legacy_guard_object;
volatile uint64_t native_legacy_guard_caller;
struct NativeLegacyMetaStorage;
extern NativeLegacyMetaStorage NativeLegacyDisplay_gMetaClass
    __asm("__ZN19NativeLegacyDisplay10gMetaClassE");

__attribute__((noreturn)) void
native_legacy_backend_guard(unsigned slot, void *object, void *caller)
{
	native_legacy_guard_slot = slot;
	native_legacy_guard_object = reinterpret_cast<uintptr_t>(object);
	native_legacy_guard_caller = reinterpret_cast<uintptr_t>(caller);
	panic("NativeLegacyDisplay runtime leaf=%u caller=%p stage=%u",
	    slot, caller, native_legacy_stage);
}

void native_legacy_display_probe(void)
{
	if (native_legacy_stage) {
		return;
	}
	printf("NATIVE LEGACY: runtime probe begin\n");
	native_legacy_stage = 1;
	printf("NATIVE LEGACY: kmod name=%s\n", g_kernel_kmod_info.name);
	void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
	printf("NATIVE LEGACY: preModLoad handle=%p\n", handle);
	if (!handle) {
		printf("NATIVE LEGACY: preModLoad failed\n");
		return;
	}
	native_legacy_display_static_init();
	native_legacy_meta_status = OSMetaClass::postModLoad(handle);
	printf("NATIVE LEGACY: postModLoad status=%d\n",
	    native_legacy_meta_status);
	if (native_legacy_meta_status) {
		return;
	}

	native_legacy_stage = 2;
	IOService *display = native_legacy_display_allocate();
	native_legacy_object = reinterpret_cast<uintptr_t>(display);
	printf("NATIVE LEGACY: allocate object=%p\n", display);
	if (!display) {
		return;
	}
	native_legacy_object_size =
	    display->getMetaClass()->getClassSize();
	const char *class_name =
	    display->getMetaClass() ? display->getMetaClass()->getClassName() : "?";
	const OSMetaClass *object_meta = display->getMetaClass();
	const OSMetaClass *expected_meta =
	    reinterpret_cast<const OSMetaClass *>(&NativeLegacyDisplay_gMetaClass);
	const OSMetaClass *native_super =
	    object_meta ? object_meta->getSuperClass() : nullptr;
	const OSMetaClass *native_super_super =
	    native_super ? native_super->getSuperClass() : nullptr;
	const OSMetaClass *native_super_super_super =
	    native_super_super ? native_super_super->getSuperClass() : nullptr;
	printf("NATIVE LEGACY: meta object=%p expected=%p super=%p size=%u\n",
	    object_meta, expected_meta, native_super,
	    object_meta ? object_meta->getClassSize() : 0);
	printf("NATIVE LEGACY: chain super=%s/%p super2=%s/%p super3=%s/%p\n",
	    native_super ? native_super->getClassName() : "?",
	    native_super,
	    native_super_super ? native_super_super->getClassName() : "?",
	    native_super_super,
	    native_super_super_super ?
	    native_super_super_super->getClassName() : "?",
	    native_super_super_super);
	bool legacy_match = display->metaCast("IOMobileFramebufferLegacy") != nullptr;
	bool framebuffer_match =
	    display->metaCast("IOMobileFramebuffer") != nullptr;
	bool service_match = display->metaCast("IOService") != nullptr;
	native_legacy_parent_match =
	    legacy_match && framebuffer_match && service_match;
	printf("NATIVE LEGACY: class=%s legacy=%u framebuffer=%u service=%u\n",
	    class_name, legacy_match, framebuffer_match, service_match);
	printf("NATIVE LEGACY: size=%u parent_match=%u\n",
	    native_legacy_object_size, native_legacy_parent_match);

	native_legacy_stage = 3;
	native_legacy_init_result = display->init();
	printf("NATIVE LEGACY: init=%u\n", native_legacy_init_result);

	native_legacy_stage = 7;
	display->release();
	native_legacy_released = 1;
	native_legacy_stage = 8;
	printf("NATIVE LEGACY: release complete\n");
}

}
