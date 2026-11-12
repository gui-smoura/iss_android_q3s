// Adapted from ducvd89/Bloodborne-Android, commit 2e4726530b04f76e513e232d272d386e9973c73b.
// Original implementation: gpu/shim/vk_driver_loader.{cpp,h}. See docs/BIONIC_EXPERIMENT.md.
// gow3: Vulkan driver loading. Nothing in the process links against libvulkan: this file is the
// only dlopen of a Vulkan library and everything else comes from the entry point it returns
// (vulkan-hpp's dispatcher, VMA, FFX, Dear ImGui, the smoke test).
//
//  - ISS_VULKAN_DRIVER unset or empty: the platform's Vulkan loader, dlopen("libvulkan.so")
//    (libvulkan.so.1 on glibc). On Android that is the system loader and the phone's stock driver;
//    layers and ICD discovery are the loader's business. The entry point is vkGetInstanceProcAddr.
//  - ISS_VULKAN_DRIVER=/abs/path/libvulkan_xxx.so: that driver (e.g. Mesa Turnip from an adrenotools
//    style zip) is dlopen'ed directly, RTLD_NOW | RTLD_LOCAL, with no loader in between: no layers,
//    and the driver answers instance-level calls itself. The entry point is the ICD's
//    vk_icdGetInstanceProcAddr (after vk_icdNegotiateLoaderICDInterfaceVersion, asking for 5), or
//    its vkGetInstanceProcAddr when it exports only that.
//  - ... or, when the library exports neither but exports the symbol `HMI` (an adrenotools-style
//    Turnip pack built as an Android Vulkan HAL: hwvulkan_module_t), the HAL path below: open
//    device "vk0" through the module and use hwvulkan_device_t::GetInstanceProcAddr. There is no
//    Android loader in between, so nothing ever writes the dispatch word of the driver's
//    dispatchable handles (hwvulkan_dispatch_t, left at HWVULKAN_DISPATCH_MAGIC); that is fine
//    because every call goes straight to a driver function pointer.
#include "vk_driver_loader.h"

#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <link.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace {

// ---------------------------------------------------------------------------------------------
// Meta Quest 3 / 3S (Horizon OS) KGSL ioctl compatibility layer for Mesa Turnip.
//
// On Horizon OS (Quest 3 / 3S, Snapdragon XR2 Gen 2 / Adreno 740v3), untrusted_app SELinux rules
// allow opening /dev/kgsl-3d0 and querying properties (IOCTL_KGSL_DEVICE_GETPROPERTY), but block
// legacy KGSL memory management ioctls (IOCTL_KGSL_GPUMEM_ALLOC_ID and IOCTL_KGSL_GPUMEM_FREE_ID)
// and IOCTL_KGSL_CMDSTREAM_READTIMESTAMP_CTXTID with EACCES (Permission denied).
//
// Horizon OS permits the newer GPU object ioctls (IOCTL_KGSL_GPUOBJ_ALLOC, IOCTL_KGSL_GPUOBJ_INFO,
// IOCTL_KGSL_GPUOBJ_FREE) and IOCTL_KGSL_DEVICE_WAITTIMESTAMP_CTXTID. When a direct Turnip driver
// is loaded via ISS_VULKAN_DRIVER, we patch its ELF GOT slot for `ioctl` so that any EACCES/EPERM
// on those legacy KGSL ioctls transparently falls back to the allowed GPUOBJ / WAITTIMESTAMP
// ioctls (adapted from Droid-Deck PR #642, kgsl_compat.c & kgslpoll.c).
// ---------------------------------------------------------------------------------------------
constexpr unsigned int kKgslIocType = 0x09;

struct KgslGpumemAllocId {
    unsigned int id;
    unsigned int flags;
    size_t size;
    size_t mmapsize;
    uint64_t gpuaddr;
    uint64_t pad[2];
};

struct KgslGpumemFreeId {
    unsigned int id;
    unsigned int pad;
};

struct KgslGpumemGetInfo {
    uint64_t gpuaddr;
    unsigned int id;
    unsigned int flags;
    size_t size;
    size_t mmapsize;
    uint64_t useraddr;
    size_t pad[4];
};

struct KgslGpuobjAlloc {
    uint64_t size;
    uint64_t flags;
    uint64_t va_len;
    uint64_t mmapsize;
    unsigned int id;
    unsigned int metadata_len;
    uint64_t metadata;
};

struct KgslGpuobjFree {
    uint64_t flags;
    uint64_t priv;
    unsigned int id;
    unsigned int type;
    unsigned int len;
};

struct KgslGpuobjInfo {
    uint64_t gpuaddr;
    uint64_t flags;
    uint64_t size;
    uint64_t va_len;
    uint64_t va_addr;
    unsigned int id;
};

struct KgslCmdstreamReadtimestampCtxtid {
    unsigned int context_id;
    unsigned int type;
    unsigned int timestamp;
};

struct KgslDeviceWaittimestampCtxtid {
    unsigned int context_id;
    unsigned int timestamp;
    unsigned int timeout;
};

struct KgslDrawctxtCreate {
    unsigned int flags;
    unsigned int drawctxt_id;
};

struct KgslDrawctxtDestroy {
    unsigned int drawctxt_id;
};

struct KgslGpuCommand {
    uint64_t flags;
    uint64_t cmdlist;
    unsigned int cmdsize;
    unsigned int numcmds;
    uint64_t objlist;
    unsigned int objsize;
    unsigned int numobjs;
    uint64_t synclist;
    unsigned int syncsize;
    unsigned int numsyncs;
    unsigned int context_id;
    unsigned int timestamp;
};

struct KgslGpuAuxCommand {
    uint64_t flags;
    uint64_t cmdlist;
    unsigned int cmdsize;
    unsigned int numcmds;
    uint64_t synclist;
    unsigned int syncsize;
    unsigned int numsyncs;
    unsigned int context_id;
    unsigned int timestamp;
};

struct KgslSparseCommand {
    uint64_t flags;
    uint64_t sparsemem;
    unsigned int size;
    unsigned int numcmds;
    uint64_t synclist;
    unsigned int syncsize;
    unsigned int numsyncs;
    unsigned int context_id;
    unsigned int timestamp;
};

#if INTPTR_MAX == INT64_MAX
static_assert(sizeof(KgslGpumemAllocId) == 48, "kgsl_gpumem_alloc_id layout");
static_assert(sizeof(KgslGpumemFreeId) == 8, "kgsl_gpumem_free_id layout");
static_assert(sizeof(KgslGpumemGetInfo) == 72, "kgsl_gpumem_get_info layout");
static_assert(sizeof(KgslGpuobjAlloc) == 48, "kgsl_gpuobj_alloc layout");
static_assert(sizeof(KgslGpuobjFree) == 32, "kgsl_gpuobj_free layout");
static_assert(sizeof(KgslGpuobjInfo) == 48, "kgsl_gpuobj_info layout");
static_assert(sizeof(KgslCmdstreamReadtimestampCtxtid) == 12,
              "kgsl_cmdstream_readtimestamp_ctxtid layout");
static_assert(sizeof(KgslDeviceWaittimestampCtxtid) == 12,
              "kgsl_device_waittimestamp_ctxtid layout");
static_assert(sizeof(KgslGpuCommand) == 64, "kgsl_gpu_command layout");
static_assert(sizeof(KgslGpuAuxCommand) == 48, "kgsl_gpu_aux_command layout");
static_assert(sizeof(KgslSparseCommand) == 48, "kgsl_sparse_command layout");
#endif

constexpr unsigned long kIoctlKgslDeviceWaittimestampCtxtid =
    _IOW(kKgslIocType, 0x07, KgslDeviceWaittimestampCtxtid);
constexpr unsigned long kIoctlKgslDrawctxtCreate =
    _IOWR(kKgslIocType, 0x13, KgslDrawctxtCreate);
constexpr unsigned long kIoctlKgslDrawctxtDestroy =
    _IOW(kKgslIocType, 0x14, KgslDrawctxtDestroy);
constexpr unsigned long kIoctlKgslCmdstreamReadtimestampCtxtid =
    _IOWR(kKgslIocType, 0x16, KgslCmdstreamReadtimestampCtxtid);
constexpr unsigned long kIoctlKgslGpumemAllocId =
    _IOWR(kKgslIocType, 0x34, KgslGpumemAllocId);
constexpr unsigned long kIoctlKgslGpumemFreeId =
    _IOWR(kKgslIocType, 0x35, KgslGpumemFreeId);
constexpr unsigned long kIoctlKgslGpumemGetInfo =
    _IOWR(kKgslIocType, 0x36, KgslGpumemGetInfo);
constexpr unsigned long kIoctlKgslGpuobjAlloc =
    _IOWR(kKgslIocType, 0x45, KgslGpuobjAlloc);
constexpr unsigned long kIoctlKgslGpuobjFree =
    _IOW(kKgslIocType, 0x46, KgslGpuobjFree);
constexpr unsigned long kIoctlKgslGpuobjInfo =
    _IOWR(kKgslIocType, 0x47, KgslGpuobjInfo);
constexpr unsigned long kIoctlKgslGpuCommand =
    _IOWR(kKgslIocType, 0x4a, KgslGpuCommand);
constexpr unsigned long kIoctlKgslGpuAuxCommand =
    _IOWR(kKgslIocType, 0x4c, KgslGpuAuxCommand);
constexpr unsigned long kIoctlKgslSparseCommand =
    _IOWR(kKgslIocType, 0x57, KgslSparseCommand);

constexpr bool IsKgslIoctlNr(unsigned long request, unsigned long nr, size_t expected_size) {
    return _IOC_TYPE(request) == kKgslIocType && _IOC_NR(request) == nr &&
           _IOC_SIZE(request) == expected_size;
}

// On 64-bit processes, KGSL_MEMFLAGS_USE_CPU_MAP (0x10000000) is invalid and the kernel's
// IOCTL_KGSL_GPUMEM_ALLOC_ID strips it from returned flags so Turnip's is_memory_type_supported()
// probe returns false (use_4gb_va = false). IOCTL_KGSL_GPUOBJ_ALLOC does NOT strip that bit and
// instead skips allocating a GPU IOVA (gpuaddr = 0), causing Turnip to overwrite bo->iova with
// the 64-bit CPU mmap address and fail on the very first vkQueueSubmit with VK_ERROR_DEVICE_LOST.
constexpr uint64_t kKgslMemflagsUseCpuMap = 0x10000000ULL;

constexpr unsigned int kKgslTimestampQueued = 1;
constexpr unsigned int kKgslTimestampRetired = 3;

constexpr size_t kMaxTrackedContexts = 64;
constexpr size_t kMaxTrackedTimestamps = 256;

struct TrackedKgslContext {
    bool active;
    unsigned int context_id;
    unsigned int latest_queued;
    unsigned int latest_retired;
    unsigned int count;
    unsigned int pending[kMaxTrackedTimestamps];
};

std::mutex g_kgsl_mutex;
TrackedKgslContext g_kgsl_contexts[kMaxTrackedContexts]{};
std::atomic<bool> g_logged_gpuobj_fallback{false};
std::atomic<bool> g_logged_timestamp_fallback{false};
std::atomic<bool> g_logged_wait_denied{false};
std::atomic<bool> g_logged_wait_errno{false};
std::atomic<bool> g_logged_wait_ok{false};
std::atomic<int> g_logged_kgsl_errors{0};

bool TimestampGreater(unsigned int a, unsigned int b) {
    return static_cast<int32_t>(a - b) > 0;
}

TrackedKgslContext* FindOrCreateKgslContextLocked(unsigned int context_id) {
    TrackedKgslContext* free_slot = nullptr;
    for (auto& slot : g_kgsl_contexts) {
        if (slot.active && slot.context_id == context_id) {
            return &slot;
        }
        if (!slot.active && !free_slot) {
            free_slot = &slot;
        }
    }
    if (!free_slot) {
        free_slot = &g_kgsl_contexts[context_id % kMaxTrackedContexts];
    }
    std::memset(free_slot, 0, sizeof(*free_slot));
    free_slot->active = true;
    free_slot->context_id = context_id;
    return free_slot;
}

void RecordKgslSubmission(unsigned int context_id, unsigned int timestamp) {
    std::lock_guard<std::mutex> lock(g_kgsl_mutex);
    TrackedKgslContext* ctx = FindOrCreateKgslContextLocked(context_id);
    if (ctx->count == 0 && ctx->latest_queued == 0 && ctx->latest_retired == 0) {
        ctx->latest_queued = timestamp;
    } else if (TimestampGreater(timestamp, ctx->latest_queued)) {
        ctx->latest_queued = timestamp;
    }
    for (unsigned int i = 0; i < ctx->count; ++i) {
        if (ctx->pending[i] == timestamp) {
            return;
        }
    }
    if (ctx->count == kMaxTrackedTimestamps) {
        if (TimestampGreater(ctx->pending[0], ctx->latest_retired)) {
            ctx->latest_retired = ctx->pending[0];
        }
        std::memmove(&ctx->pending[0], &ctx->pending[1],
                     (kMaxTrackedTimestamps - 1) * sizeof(unsigned int));
        ctx->count = kMaxTrackedTimestamps - 1;
    }
    // Insert sorted in ascending timestamp order.
    unsigned int pos = ctx->count;
    while (pos > 0 && TimestampGreater(ctx->pending[pos - 1], timestamp)) {
        ctx->pending[pos] = ctx->pending[pos - 1];
        --pos;
    }
    ctx->pending[pos] = timestamp;
    ++ctx->count;
}

void DestroyKgslContext(unsigned int context_id) {
    std::lock_guard<std::mutex> lock(g_kgsl_mutex);
    for (auto& slot : g_kgsl_contexts) {
        if (slot.active && slot.context_id == context_id) {
            slot.active = false;
            slot.count = 0;
            return;
        }
    }
}

int FallbackGpumemAllocId(IssRealIoctlFn real_ioctl, int fd, KgslGpumemAllocId* req) {
    if (!req) {
        errno = EFAULT;
        return -1;
    }
    KgslGpuobjAlloc alloc{};
    alloc.size = req->size;
    alloc.flags = static_cast<uint64_t>(req->flags) & ~kKgslMemflagsUseCpuMap;
    alloc.va_len = req->size;
    if (real_ioctl(fd, kIoctlKgslGpuobjAlloc, &alloc) < 0) {
        return -1;
    }
    KgslGpuobjInfo info{};
    info.id = alloc.id;
    if (real_ioctl(fd, kIoctlKgslGpuobjInfo, &info) < 0) {
        const int saved_errno = errno;
        KgslGpuobjFree free_req{};
        free_req.id = alloc.id;
        real_ioctl(fd, kIoctlKgslGpuobjFree, &free_req);
        errno = saved_errno;
        return -1;
    }
    req->id = alloc.id;
    req->flags = static_cast<unsigned int>(info.flags & ~kKgslMemflagsUseCpuMap);
    req->size = static_cast<size_t>(info.size);
    req->mmapsize = static_cast<size_t>(alloc.mmapsize ? alloc.mmapsize : info.size);
    req->gpuaddr = info.gpuaddr;
    if (!g_logged_gpuobj_fallback.exchange(true)) {
        std::fprintf(stderr,
                     "Vulkan: Quest KGSL compatibility active (GPUMEM_ALLOC_ID -> GPUOBJ_ALLOC, "
                     "first id=%u size=%zu flags=0x%x gpuaddr=0x%llx)\n",
                     req->id, req->size, req->flags,
                     static_cast<unsigned long long>(req->gpuaddr));
    }
    return 0;
}

int FallbackGpumemFreeId(IssRealIoctlFn real_ioctl, int fd, KgslGpumemFreeId* req) {
    if (!req) {
        errno = EFAULT;
        return -1;
    }
    KgslGpuobjFree free_req{};
    free_req.id = req->id;
    const int ret = real_ioctl(fd, kIoctlKgslGpuobjFree, &free_req);
    if (ret < 0) {
        const int saved_errno = errno;
        const int logged = g_logged_kgsl_errors.fetch_add(1, std::memory_order_relaxed);
        if (logged < 8) {
            std::fprintf(stderr, "Vulkan: Quest KGSL GPUOBJ_FREE id=%u failed (errno=%d %s)\n",
                         req->id, saved_errno, std::strerror(saved_errno));
        }
        errno = saved_errno;
    }
    return ret;
}

int FallbackGpumemGetInfo(IssRealIoctlFn real_ioctl, int fd, KgslGpumemGetInfo* req) {
    if (!req) {
        errno = EFAULT;
        return -1;
    }
    KgslGpuobjInfo info{};
    info.id = req->id;
    if (real_ioctl(fd, kIoctlKgslGpuobjInfo, &info) < 0) {
        return -1;
    }
    req->gpuaddr = info.gpuaddr;
    req->id = info.id;
    req->flags = static_cast<unsigned int>(info.flags & ~kKgslMemflagsUseCpuMap);
    req->size = static_cast<size_t>(info.size);
    req->mmapsize = static_cast<size_t>(info.size);
    req->useraddr = info.va_addr;
    return 0;
}

int FallbackReadtimestampCtxtid(IssRealIoctlFn real_ioctl, int fd,
                                KgslCmdstreamReadtimestampCtxtid* req) {
    if (!req) {
        errno = EFAULT;
        return -1;
    }
    if (!g_logged_timestamp_fallback.exchange(true)) {
        std::fprintf(stderr,
                     "Vulkan: Quest KGSL timestamp compatibility active "
                     "(READTIMESTAMP_CTXTID -> WAITTIMESTAMP_CTXTID)\n");
    }
    unsigned int pending_copy[kMaxTrackedTimestamps];
    unsigned int pending_count = 0;
    unsigned int latest_queued = 0;
    unsigned int latest_retired = 0;
    {
        std::lock_guard<std::mutex> lock(g_kgsl_mutex);
        TrackedKgslContext* ctx = FindOrCreateKgslContextLocked(req->context_id);
        latest_queued = ctx->latest_queued;
        latest_retired = ctx->latest_retired;
        if (req->type != kKgslTimestampRetired) {
            req->timestamp = latest_queued;
            errno = 0;
            return 0;
        }
        pending_count = ctx->count;
        if (pending_count > 0) {
            std::memcpy(pending_copy, ctx->pending, pending_count * sizeof(unsigned int));
        }
    }

    if (pending_count == 0) {
        req->timestamp = latest_retired;
        errno = 0;
        return 0;
    }

    unsigned int retired = latest_retired;
    unsigned int completed_count = 0;
    for (unsigned int i = 0; i < pending_count; ++i) {
        KgslDeviceWaittimestampCtxtid wait_req{};
        wait_req.context_id = req->context_id;
        wait_req.timestamp = pending_copy[i];
        wait_req.timeout = 0;
        if (real_ioctl(fd, kIoctlKgslDeviceWaittimestampCtxtid, &wait_req) == 0) {
            if (!g_logged_wait_ok.exchange(true)) {
                std::fprintf(stderr,
                             "Vulkan: Quest KGSL WAITTIMESTAMP_CTXTID works (ctx=%u ts=%u)\n",
                             req->context_id, pending_copy[i]);
            }
            retired = pending_copy[i];
            completed_count = i + 1;
        } else if (errno == EACCES || errno == EPERM) {
            // If WAITTIMESTAMP_CTXTID is also denied by SELinux, retire up to latest_queued so
            // Turnip never stalls or returns VK_ERROR_DEVICE_LOST.
            if (!g_logged_wait_denied.exchange(true)) {
                std::fprintf(stderr,
                             "Vulkan: Quest KGSL WAITTIMESTAMP_CTXTID denied (errno=%d); "
                             "retiring timestamps without GPU sync (expect corruption)\n",
                             errno);
            }
            retired = latest_queued;
            completed_count = pending_count;
            break;
        } else {
            if (!g_logged_wait_errno.exchange(true)) {
                std::fprintf(stderr,
                             "Vulkan: Quest KGSL WAITTIMESTAMP_CTXTID pending (errno=%d %s)\n",
                             errno, std::strerror(errno));
            }
            break;
        }
    }

    // If none completed with timeout=0, do a 1ms poll on the earliest pending timestamp so
    // Turnip's spin-wait loop yields smoothly instead of burning CPU.
    if (completed_count == 0) {
        KgslDeviceWaittimestampCtxtid wait_req{};
        wait_req.context_id = req->context_id;
        wait_req.timestamp = pending_copy[0];
        wait_req.timeout = 1;
        if (real_ioctl(fd, kIoctlKgslDeviceWaittimestampCtxtid, &wait_req) == 0) {
            retired = pending_copy[0];
            completed_count = 1;
        }
    }

    if (completed_count > 0) {
        std::lock_guard<std::mutex> lock(g_kgsl_mutex);
        TrackedKgslContext* ctx = FindOrCreateKgslContextLocked(req->context_id);
        if (TimestampGreater(retired, ctx->latest_retired) || ctx->latest_retired == 0) {
            ctx->latest_retired = retired;
        }
        unsigned int write_idx = 0;
        for (unsigned int i = 0; i < ctx->count; ++i) {
            if (TimestampGreater(ctx->pending[i], retired)) {
                ctx->pending[write_idx++] = ctx->pending[i];
            }
        }
        ctx->count = write_idx;
    }

    req->timestamp = retired;
    errno = 0;
    return 0;
}

int RealKernelIoctl(int fd, unsigned long request, void* arg) {
    static IssRealIoctlFn real_fn = []() -> IssRealIoctlFn {
        if (void* libc = dlopen("libc.so", RTLD_LAZY | RTLD_NOLOAD)) {
            if (void* sym = dlsym(libc, "ioctl")) {
                return reinterpret_cast<IssRealIoctlFn>(sym);
            }
        }
        if (void* sym = dlsym(RTLD_DEFAULT, "ioctl")) {
            return reinterpret_cast<IssRealIoctlFn>(sym);
        }
        return nullptr;
    }();
    if (!real_fn) {
        errno = ENOSYS;
        return -1;
    }
    return real_fn(fd, request, arg);
}

int DriverIoctlHook(int fd, unsigned long request, ...) {
    va_list ap;
    va_start(ap, request);
    void* arg = va_arg(ap, void*);
    va_end(ap);
    return iss_quest_kgsl_ioctl_with_fn(&RealKernelIoctl, fd, request, arg);
}

struct PatchGotContext {
    uintptr_t target_addr;
    const char* driver_path;
    void* replacement;
    int patched_count;
};

bool PathEndsWith(const char* full, const char* suffix) {
    if (!full || !suffix || !*full || !*suffix) {
        return false;
    }
    if (std::strcmp(full, suffix) == 0) {
        return true;
    }
    const char* base = std::strrchr(suffix, '/');
    const char* suffix_name = base ? base + 1 : suffix;
    const size_t full_len = std::strlen(full);
    const size_t name_len = std::strlen(suffix_name);
    return full_len >= name_len &&
           std::strcmp(full + (full_len - name_len), suffix_name) == 0;
}

void PatchRelaTable(uintptr_t base_addr, const ElfW(Rela)* rela, size_t rela_count,
                    const ElfW(Sym)* symtab, const char* strtab, void* replacement,
                    int* patched_count) {
    if (!rela || !symtab || !strtab || !replacement) {
        return;
    }
    const long page_size = sysconf(_SC_PAGESIZE);
    const uintptr_t page_mask = ~(static_cast<uintptr_t>(page_size > 0 ? page_size : 4096) - 1);

    for (size_t i = 0; i < rela_count; ++i) {
#if INTPTR_MAX == INT64_MAX
        const size_t sym_idx = ELF64_R_SYM(rela[i].r_info);
#else
        const size_t sym_idx = ELF32_R_SYM(rela[i].r_info);
#endif
        if (sym_idx == 0) {
            continue;
        }
        const char* sym_name = strtab + symtab[sym_idx].st_name;
        if (std::strcmp(sym_name, "ioctl") != 0 &&
            std::strcmp(sym_name, "__ioctl") != 0) {
            continue;
        }
        auto* got_slot = reinterpret_cast<void**>(base_addr + rela[i].r_offset);
        const uintptr_t page_start = reinterpret_cast<uintptr_t>(got_slot) & page_mask;
        const size_t protect_len =
            (reinterpret_cast<uintptr_t>(got_slot) + sizeof(void*)) - page_start;
        if (mprotect(reinterpret_cast<void*>(page_start), protect_len,
                     PROT_READ | PROT_WRITE) == 0 ||
            mprotect(reinterpret_cast<void*>(page_start), protect_len,
                     PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
            *got_slot = replacement;
            ++(*patched_count);
        }
    }
}


int IteratePhdrCallback(struct dl_phdr_info* info, size_t /*size*/, void* data) {
    auto* ctx = static_cast<PatchGotContext*>(data);
    bool matches = false;
    if (ctx->target_addr != 0) {
        for (int i = 0; i < info->dlpi_phnum; ++i) {
            if (info->dlpi_phdr[i].p_type != PT_LOAD) {
                continue;
            }
            const uintptr_t seg_start = info->dlpi_addr + info->dlpi_phdr[i].p_vaddr;
            const uintptr_t seg_end = seg_start + info->dlpi_phdr[i].p_memsz;
            if (ctx->target_addr >= seg_start && ctx->target_addr < seg_end) {
                matches = true;
                break;
            }
        }
    }
    if (!matches && PathEndsWith(info->dlpi_name, ctx->driver_path)) {
        matches = true;
    }
    if (!matches) {
        return 0;
    }

    const ElfW(Dyn)* dyn = nullptr;
    for (int i = 0; i < info->dlpi_phnum; ++i) {
        if (info->dlpi_phdr[i].p_type == PT_DYNAMIC) {
            dyn = reinterpret_cast<const ElfW(Dyn)*>(info->dlpi_addr + info->dlpi_phdr[i].p_vaddr);
            break;
        }
    }
    if (!dyn) {
        return 0;
    }

    const ElfW(Sym)* symtab = nullptr;
    const char* strtab = nullptr;
    const ElfW(Rela)* jmprel = nullptr;
    size_t pltrelsz = 0;
    const ElfW(Rela)* rela = nullptr;
    size_t relasz = 0;

    auto resolve_ptr = [&](ElfW(Addr) ptr) -> uintptr_t {
        // On glibc, dynamic section pointers may already be relocated; on Android Bionic they are
        // relative to dlpi_addr unless > dlpi_addr.
        return (ptr < info->dlpi_addr) ? (info->dlpi_addr + ptr) : ptr;
    };

    for (; dyn->d_tag != DT_NULL; ++dyn) {
        switch (dyn->d_tag) {
        case DT_SYMTAB:
            symtab = reinterpret_cast<const ElfW(Sym)*>(resolve_ptr(dyn->d_un.d_ptr));
            break;
        case DT_STRTAB:
            strtab = reinterpret_cast<const char*>(resolve_ptr(dyn->d_un.d_ptr));
            break;
        case DT_JMPREL:
            jmprel = reinterpret_cast<const ElfW(Rela)*>(resolve_ptr(dyn->d_un.d_ptr));
            break;
        case DT_PLTRELSZ:
            pltrelsz = dyn->d_un.d_val;
            break;
        case DT_RELA:
            rela = reinterpret_cast<const ElfW(Rela)*>(resolve_ptr(dyn->d_un.d_ptr));
            break;
        case DT_RELASZ:
            relasz = dyn->d_un.d_val;
            break;
        default:
            break;
        }
    }

    if (jmprel && pltrelsz >= sizeof(ElfW(Rela))) {
        PatchRelaTable(info->dlpi_addr, jmprel, pltrelsz / sizeof(ElfW(Rela)), symtab, strtab,
                       ctx->replacement, &ctx->patched_count);
    }
    if (rela && relasz >= sizeof(ElfW(Rela))) {
        PatchRelaTable(info->dlpi_addr, rela, relasz / sizeof(ElfW(Rela)), symtab, strtab,
                       ctx->replacement, &ctx->patched_count);
    }
    return 0;
}

// ---------------------------------------------------------------------------------------------
// Android hwvulkan HAL ABI. The NDK ships neither <hardware/hardware.h> nor <hardware/hwvulkan.h>,
// so the structs are declared here. Layouts, constants and field order are copied from AOSP
// hardware/libhardware/include/hardware/hardware.h and hwvulkan.h (Apache-2.0; the identical
// copies are in Mesa's include/android_stub/hardware/, which Turnip builds against):
//   hw_module_t     { u32 tag; u16 module_api_version; u16 hal_api_version; const char* id, *name,
//                     *author; hw_module_methods_t* methods; void* dso; u64 reserved[32 - 7]; }
//   hw_device_t     { u32 tag; u32 version; hw_module_t* module; u64 reserved[12];
//                     int (*close)(hw_device_t*); }
//   hwvulkan_device_t { hw_device_t common; PFN_vkEnumerateInstanceExtensionProperties;
//                     PFN_vkCreateInstance; PFN_vkGetInstanceProcAddr; }
//   HARDWARE_MODULE_TAG 'HWMT', HARDWARE_DEVICE_TAG 'HWDT', HAL_MODULE_INFO_SYM = HMI,
//   HWVULKAN_HARDWARE_MODULE_ID "vulkan", HWVULKAN_DEVICE_0 "vk0",
//   HWVULKAN_DEVICE_API_VERSION_0_1 = HARDWARE_MAKE_API_VERSION_2(0, 1, 0) = 0x00010000,
//   HWVULKAN_DISPATCH_MAGIC 0x01CDC0DE (= ICD_LOADER_MAGIC of the LunarG loader).
// hwvulkan_module_t is just { hw_module_t common; }.
// ---------------------------------------------------------------------------------------------
constexpr uint32_t kHwModuleTag = (uint32_t('H') << 24) | (uint32_t('W') << 16) |
                                  (uint32_t('M') << 8) | uint32_t('T');
constexpr uint32_t kHwDeviceTag = (uint32_t('H') << 24) | (uint32_t('W') << 16) |
                                  (uint32_t('D') << 8) | uint32_t('T');
constexpr uint32_t kHwvulkanDeviceApiVersion01 = 0x00010000;
constexpr uint32_t kHwApiVersion2MajMinMask = 0xffff0000;
constexpr const char* kHwvulkanModuleId = "vulkan";
constexpr const char* kHwvulkanDevice0 = "vk0";

struct HwDevice;
struct HwModule;
struct HwModuleMethods {
    int (*open)(const HwModule* module, const char* id, HwDevice** device);
};
struct HwModule {
    uint32_t tag;
    uint16_t module_api_version;
    uint16_t hal_api_version;
    const char* id;
    const char* name;
    const char* author;
    HwModuleMethods* methods;
    void* dso;
    uint64_t reserved[32 - 7];
};
struct HwDevice {
    uint32_t tag;
    uint32_t version;
    HwModule* module;
    uint64_t reserved[12];
    int (*close)(HwDevice* device);
};
struct HwvulkanDevice {
    HwDevice common;
    IssVkVoidFunction EnumerateInstanceExtensionProperties; // really PFN_vkEnumerateInstanceExtensionProperties
    IssVkVoidFunction CreateInstance;                       // really PFN_vkCreateInstance
    IssVkGetInstanceProcAddr GetInstanceProcAddr;
};
#if INTPTR_MAX == INT64_MAX
static_assert(sizeof(HwModule) == 248 && offsetof(HwModule, methods) == 32 &&
              offsetof(HwModule, dso) == 40, "hw_module_t layout");
static_assert(sizeof(HwDevice) == 120 && offsetof(HwDevice, close) == 112, "hw_device_t layout");
static_assert(sizeof(HwvulkanDevice) == 144 && offsetof(HwvulkanDevice, GetInstanceProcAddr) == 136,
              "hwvulkan_device_t layout");
#endif

// The HAL's device stays open for the process lifetime (the Android loader never closes it either
// until unload). The three entry points of the device back-fill what its GetInstanceProcAddr
// might not answer for a NULL instance, and vkEnumerateInstanceLayerProperties, which the Android
// loader implements and a HAL need not, reports no layers.
HwvulkanDevice* g_hal_device = nullptr;

int HalNoLayers(uint32_t* count, void* /*properties*/) {
    if (count) {
        *count = 0;
    }
    return 0; // VK_SUCCESS
}

IssVkVoidFunction HalGetInstanceProcAddr(void* instance, const char* name) {
    IssVkVoidFunction fn = g_hal_device->GetInstanceProcAddr(instance, name);
    if (fn || instance || !name) {
        return fn;
    }
    if (!std::strcmp(name, "vkEnumerateInstanceExtensionProperties")) {
        return g_hal_device->EnumerateInstanceExtensionProperties;
    }
    if (!std::strcmp(name, "vkCreateInstance")) {
        return g_hal_device->CreateInstance;
    }
    if (!std::strcmp(name, "vkEnumerateInstanceLayerProperties")) {
        return reinterpret_cast<IssVkVoidFunction>(&HalNoLayers);
    }
    return nullptr;
}

IssVkGetInstanceProcAddr OpenHal(void* hmi, const char* driver) {
    auto* module = static_cast<HwModule*>(hmi);
    if (module->tag != kHwModuleTag) {
        std::fprintf(stderr, "Vulkan: driver %s: HMI has the wrong module tag 0x%08x\n", driver,
                     module->tag);
        return nullptr;
    }
    if (!module->id || std::strcmp(module->id, kHwvulkanModuleId) != 0) {
        std::fprintf(stderr, "Vulkan: driver %s: HMI is module \"%s\", not \"%s\"\n", driver,
                     module->id ? module->id : "(null)", kHwvulkanModuleId);
        return nullptr;
    }
    if (!module->methods || !module->methods->open) {
        std::fprintf(stderr, "Vulkan: driver %s: HAL module has no open method\n", driver);
        return nullptr;
    }
    HwDevice* device = nullptr;
    const int status = module->methods->open(module, kHwvulkanDevice0, &device);
    if (status != 0 || !device) {
        std::fprintf(stderr, "Vulkan: driver %s: opening HAL device %s failed (%d)\n", driver,
                     kHwvulkanDevice0, status);
        return nullptr;
    }
    if (device->tag != kHwDeviceTag ||
        (device->version & kHwApiVersion2MajMinMask) != kHwvulkanDeviceApiVersion01) {
        std::fprintf(stderr, "Vulkan: driver %s: unexpected HAL device (tag 0x%08x, version 0x%08x)\n",
                     driver, device->tag, device->version);
        if (device->close) {
            device->close(device);
        }
        return nullptr;
    }
    auto* vk_device = reinterpret_cast<HwvulkanDevice*>(device);
    if (!vk_device->GetInstanceProcAddr) {
        std::fprintf(stderr, "Vulkan: driver %s: HAL device has no GetInstanceProcAddr\n", driver);
        if (device->close) {
            device->close(device);
        }
        return nullptr;
    }
    g_hal_device = vk_device;
    std::fprintf(stderr, "Vulkan: direct driver %s via HAL module HMI (%s by %s, module API %u.%u, "
                         "HAL API %u.%u)\n",
                 driver, module->name ? module->name : "?", module->author ? module->author : "?",
                 module->module_api_version >> 8, module->module_api_version & 0xff,
                 module->hal_api_version >> 8, module->hal_api_version & 0xff);
    return &HalGetInstanceProcAddr;
}

void MaybeHookDirectDriverIoctl(void* sym_in_driver, const char* driver_path) {
    const char* disable = std::getenv("ISS_QUEST_KGSL_SHIM");
    if (disable && std::strcmp(disable, "0") == 0) {
        return;
    }
    const int patched = iss_patch_elf_ioctl_got(
        sym_in_driver, driver_path, reinterpret_cast<void*>(&DriverIoctlHook));
    if (patched > 0) {
        std::fprintf(stderr, "Vulkan: installed Quest KGSL ioctl hook (%d GOT slot%s) for %s\n",
                     patched, patched == 1 ? "" : "s", driver_path);
    }
}

IssVkGetInstanceProcAddr Load() {
    const char* driver = std::getenv("ISS_VULKAN_DRIVER");
    if (!driver || !*driver) {
#ifdef __ANDROID__
        static const char* const names[] = {"libvulkan.so"};
#else
        static const char* const names[] = {"libvulkan.so.1", "libvulkan.so"};
#endif
        for (const char* name : names) {
            if (void* lib = dlopen(name, RTLD_NOW | RTLD_LOCAL)) {
                if (void* gipa = dlsym(lib, "vkGetInstanceProcAddr")) {
                    std::fprintf(stderr, "Vulkan: system loader %s\n", name);
                    return reinterpret_cast<IssVkGetInstanceProcAddr>(gipa);
                }
            }
        }
        std::fprintf(stderr, "Vulkan: no system loader (%s)\n", dlerror());
        return nullptr;
    }
    void* lib = dlopen(driver, RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        std::fprintf(stderr, "Vulkan: cannot load driver %s: %s\n", driver, dlerror());
        return nullptr;
    }
    using NegotiateFn = int (*)(uint32_t*);
    if (auto negotiate = reinterpret_cast<NegotiateFn>(
            dlsym(lib, "vk_icdNegotiateLoaderICDInterfaceVersion"))) {
        uint32_t version = 5;
        if (negotiate(&version) != 0) {
            std::fprintf(stderr, "Vulkan: driver %s rejected ICD interface version 5\n", driver);
            return nullptr;
        }
    }
    const char* entry = "vk_icdGetInstanceProcAddr";
    void* gipa = dlsym(lib, entry);
    if (!gipa) {
        entry = "vkGetInstanceProcAddr";
        gipa = dlsym(lib, entry);
    }
    if (!gipa) {
        if (void* hmi = dlsym(lib, "HMI")) {
            MaybeHookDirectDriverIoctl(hmi, driver);
            return OpenHal(hmi, driver);
        }
        std::fprintf(stderr, "Vulkan: driver %s exports none of vk_icdGetInstanceProcAddr, "
                             "vkGetInstanceProcAddr and HMI (Android Vulkan HAL module)\n", driver);
        return nullptr;
    }
    MaybeHookDirectDriverIoctl(gipa, driver);
    std::fprintf(stderr, "Vulkan: direct driver %s via %s\n", driver, entry);
    return reinterpret_cast<IssVkGetInstanceProcAddr>(gipa);
}

} // namespace

extern "C" __attribute__((visibility("default"))) int
iss_quest_kgsl_ioctl_with_fn(IssRealIoctlFn real_ioctl, int fd, unsigned long request, void* arg) {
    if (!real_ioctl) {
        errno = EINVAL;
        return -1;
    }
    const int ret = real_ioctl(fd, request, arg);
    if (ret == 0) {
        if (IsKgslIoctlNr(request, 0x4a, sizeof(KgslGpuCommand)) && arg) {
            const auto* cmd = static_cast<const KgslGpuCommand*>(arg);
            RecordKgslSubmission(cmd->context_id, cmd->timestamp);
        } else if (IsKgslIoctlNr(request, 0x4c, sizeof(KgslGpuAuxCommand)) && arg) {
            const auto* aux = static_cast<const KgslGpuAuxCommand*>(arg);
            RecordKgslSubmission(aux->context_id, aux->timestamp);
        } else if (IsKgslIoctlNr(request, 0x57, sizeof(KgslSparseCommand)) && arg) {
            const auto* sparse = static_cast<const KgslSparseCommand*>(arg);
            RecordKgslSubmission(sparse->context_id, sparse->timestamp);
        } else if (IsKgslIoctlNr(request, 0x13, sizeof(KgslDrawctxtCreate)) && arg) {
            const auto* create_req = static_cast<const KgslDrawctxtCreate*>(arg);
            std::lock_guard<std::mutex> lock(g_kgsl_mutex);
            FindOrCreateKgslContextLocked(create_req->drawctxt_id);
        } else if (IsKgslIoctlNr(request, 0x14, sizeof(KgslDrawctxtDestroy)) && arg) {
            const auto* destroy_req = static_cast<const KgslDrawctxtDestroy*>(arg);
            DestroyKgslContext(destroy_req->drawctxt_id);
        }
        return 0;
    }

    const int saved_errno = errno;
    if (saved_errno == EACCES || saved_errno == EPERM) {
        if (IsKgslIoctlNr(request, 0x34, sizeof(KgslGpumemAllocId))) {
            return FallbackGpumemAllocId(real_ioctl, fd, static_cast<KgslGpumemAllocId*>(arg));
        }
        if (IsKgslIoctlNr(request, 0x35, sizeof(KgslGpumemFreeId))) {
            return FallbackGpumemFreeId(real_ioctl, fd, static_cast<KgslGpumemFreeId*>(arg));
        }
        if (IsKgslIoctlNr(request, 0x36, sizeof(KgslGpumemGetInfo))) {
            return FallbackGpumemGetInfo(real_ioctl, fd, static_cast<KgslGpumemGetInfo*>(arg));
        }
        if (IsKgslIoctlNr(request, 0x16, sizeof(KgslCmdstreamReadtimestampCtxtid))) {
            return FallbackReadtimestampCtxtid(
                real_ioctl, fd, static_cast<KgslCmdstreamReadtimestampCtxtid*>(arg));
        }
    }

    // Ignore benign ENODEV on IOCTL_KGSL_DEVICE_GETPROPERTY (nr=0x02) when Turnip probes optional
    // kernel properties.
    const bool benign_getprop_enodev =
        (_IOC_NR(request) == 0x02 && (saved_errno == ENODEV || saved_errno == EINVAL));
    if (_IOC_TYPE(request) == kKgslIocType && !benign_getprop_enodev && saved_errno != EAGAIN &&
        saved_errno != EINTR && saved_errno != ETIMEDOUT) {
        const int logged = g_logged_kgsl_errors.fetch_add(1, std::memory_order_relaxed);
        if (logged < 8) {
            std::fprintf(stderr,
                         "Vulkan: KGSL ioctl 0x%lx (nr=0x%02lx, size=%lu) failed with errno=%d (%s)\n",
                         request, static_cast<unsigned long>(_IOC_NR(request)),
                         static_cast<unsigned long>(_IOC_SIZE(request)), saved_errno,
                         std::strerror(saved_errno));
        }
    }

    errno = saved_errno;
    return ret;
}

extern "C" __attribute__((visibility("default"))) int
iss_patch_elf_ioctl_got(void* sym_in_driver, const char* driver_path, void* replacement_ioctl) {
    if (!replacement_ioctl) {
        return 0;
    }
    PatchGotContext ctx{
        reinterpret_cast<uintptr_t>(sym_in_driver),
        driver_path,
        replacement_ioctl,
        0,
    };
    dl_iterate_phdr(&IteratePhdrCallback, &ctx);
    return ctx.patched_count;
}

extern "C" __attribute__((visibility("default"))) IssVkGetInstanceProcAddr
iss_vk_get_instance_proc_addr(void) {
    static std::once_flag once;
    static IssVkGetInstanceProcAddr gipa;
    std::call_once(once, [] { gipa = Load(); });
    return gipa;
}

