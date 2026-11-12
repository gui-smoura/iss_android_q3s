// Adapted from ducvd89/Bloodborne-Android, commit 2e4726530b04f76e513e232d272d386e9973c73b.
// Original implementation: gpu/shim/vk_driver_loader.{cpp,h}. See docs/BIONIC_EXPERIMENT.md.
// gow3: the one place that loads a Vulkan driver (see vk_driver_loader.cpp).
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*IssVkVoidFunction)(void);
typedef IssVkVoidFunction (*IssVkGetInstanceProcAddr)(void* instance, const char* name);

/// Loads the Vulkan driver once (ISS_VULKAN_DRIVER unset: the system loader's libvulkan.so; set to a
/// path: that driver library directly) and returns its vkGetInstanceProcAddr, or NULL (logged).
/// Every other Vulkan function of the process is resolved through it.
IssVkGetInstanceProcAddr iss_vk_get_instance_proc_addr(void);

typedef int (*IssRealIoctlFn)(int fd, unsigned long request, void* arg);

/// Quest 3 / 3S (Horizon OS) KGSL compatibility wrapper: translates SELinux-blocked legacy KGSL
/// ioctls (GPUMEM_ALLOC_ID, GPUMEM_FREE_ID, CMDSTREAM_READTIMESTAMP_CTXTID) into GPUOBJ_ALLOC /
/// GPUOBJ_INFO / GPUOBJ_FREE / DEVICE_WAITTIMESTAMP_CTXTID when the kernel returns EACCES/EPERM.
int iss_quest_kgsl_ioctl_with_fn(IssRealIoctlFn real_ioctl, int fd, unsigned long request,
                                 void* arg);

/// Patches the ELF GOT slot(s) for `ioctl` in the shared object containing `sym_in_driver` to
/// point to `replacement_ioctl`. Returns the number of patched GOT slots.
int iss_patch_elf_ioctl_got(void* sym_in_driver, const char* driver_path, void* replacement_ioctl);

#ifdef __cplusplus
}
#endif

