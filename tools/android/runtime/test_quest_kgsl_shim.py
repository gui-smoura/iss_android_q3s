"""Tests for Meta Quest 3 / 3S (Horizon OS) KGSL ioctl compatibility shim and LDS atomic CAS lowering."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class QuestKgslShimTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("c++"), "C++ compiler unavailable")
    def test_kgsl_gpuobj_and_timestamp_fallbacks_and_elf_got_hook(self):
        loader_cpp = ROOT / "port/gpu/shim/vk_driver_loader.cpp"
        loader_h = ROOT / "port/gpu/shim/vk_driver_loader.h"

        mock_driver_src = r"""
#include <sys/ioctl.h>
#include <cstddef>
#include <cstdint>

struct KgslGpumemAllocId {

    unsigned int id;
    unsigned int flags;
    size_t size;
    size_t mmapsize;
    uint64_t gpuaddr;
    uint64_t pad[2];
};

static constexpr unsigned int kKgslIocType = 0x09;
static constexpr unsigned long kIoctlKgslGpumemAllocId =
    _IOWR(kKgslIocType, 0x34, KgslGpumemAllocId);

extern "C" __attribute__((visibility("default"))) int
mock_driver_alloc_bo(int fd, uint64_t* out_gpuaddr, unsigned int* out_id) {
    KgslGpumemAllocId req{};
    req.size = 65536;
    req.flags = 0x1000;
    int ret = ioctl(fd, kIoctlKgslGpumemAllocId, &req);
    if (ret == 0) {
        *out_gpuaddr = req.gpuaddr;
        *out_id = req.id;
    }
    return ret;
}
"""

        harness_src = r"""
#include "vk_driver_loader.h"

#include <cassert>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <initializer_list>
#include <sys/ioctl.h>


namespace {
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

constexpr unsigned long kIoctlKgslDeviceWaittimestampCtxtid =
    _IOW(kKgslIocType, 0x07, KgslDeviceWaittimestampCtxtid);
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

bool g_fail_gpuobj_info = false;
unsigned int g_last_freed_id = 0;
unsigned int g_retired_on_gpu = 0;
uint64_t g_last_gpuobj_alloc_flags = 0;
uint64_t g_last_gpuobj_alloc_va_len = 0;

int MockQuestKernelIoctl(int /*fd*/, unsigned long request, void* arg) {
    if (request == kIoctlKgslGpumemAllocId ||
        request == kIoctlKgslGpumemFreeId ||
        request == kIoctlKgslGpumemGetInfo ||
        request == kIoctlKgslCmdstreamReadtimestampCtxtid) {
        errno = EACCES;
        return -1;
    }
    if (request == kIoctlKgslGpuobjAlloc) {
        auto* a = static_cast<KgslGpuobjAlloc*>(arg);
        g_last_gpuobj_alloc_flags = a->flags;
        g_last_gpuobj_alloc_va_len = a->va_len;
        a->id = 42;
        a->mmapsize = a->size + 4096;
        return 0;
    }
    if (request == kIoctlKgslGpuobjInfo) {
        if (g_fail_gpuobj_info) {
            errno = EINVAL;
            return -1;
        }
        auto* info = static_cast<KgslGpuobjInfo*>(arg);
        assert(info->id == 42);
        // Simulate kernel returning GPUOBJ_INFO with bit 28 set if not stripped by shim:
        info->gpuaddr = 0x700012340000ULL;
        info->flags = 0x10001000ULL;
        info->size = 65536;
        info->va_addr = 0x7f0000000000ULL;
        return 0;
    }
    if (request == kIoctlKgslGpuobjFree) {
        auto* f = static_cast<KgslGpuobjFree*>(arg);
        g_last_freed_id = f->id;
        return 0;
    }
    if (request == kIoctlKgslGpuCommand) {
        return 0;
    }
    if (request == kIoctlKgslDeviceWaittimestampCtxtid) {
        auto* w = static_cast<KgslDeviceWaittimestampCtxtid*>(arg);
        if (w->timestamp <= g_retired_on_gpu) {
            return 0;
        }
        errno = ETIMEDOUT;
        return -1;
    }
    errno = ENOTTY;
    return -1;
}

int ReplacementIoctlForGotTest(int fd, unsigned long request, ...) {
    va_list ap;
    va_start(ap, request);
    void* arg = va_arg(ap, void*);
    va_end(ap);
    return iss_quest_kgsl_ioctl_with_fn(&MockQuestKernelIoctl, fd, request, arg);
}

} // namespace

int main(int argc, char** argv) {
    assert(argc == 2);

    // Verify that kIoctlKgslCmdstreamReadtimestampCtxtid matches Turnip's 0xc00c0916 (_IOWR).
    assert(kIoctlKgslCmdstreamReadtimestampCtxtid == 0xc00c0916UL);

    // 1. Test GPUMEM_ALLOC_ID -> GPUOBJ_ALLOC + GPUOBJ_INFO fallback and stripping of
    // KGSL_MEMFLAGS_USE_CPU_MAP (0x10000000) so Turnip's is_memory_type_supported(USE_CPU_MAP)
    // probe returns false on 64-bit processes and BOs keep valid GPU IOVAs.
    KgslGpumemAllocId alloc_req{};
    alloc_req.size = 65536;
    alloc_req.flags = 0x10001000U;
    int rc = iss_quest_kgsl_ioctl_with_fn(&MockQuestKernelIoctl, 99, kIoctlKgslGpumemAllocId, &alloc_req);
    assert(rc == 0);
    assert(alloc_req.id == 42);
    assert(g_last_gpuobj_alloc_flags == 0x1000ULL);
    assert(g_last_gpuobj_alloc_va_len == 65536ULL);
    assert((alloc_req.flags & 0x10000000U) == 0);
    assert(alloc_req.gpuaddr == 0x700012340000ULL);
    assert(alloc_req.size == 65536);
    assert(alloc_req.mmapsize == 65536 + 4096);

    // 1b. Test GPUMEM_GET_INFO -> GPUOBJ_INFO fallback.
    KgslGpumemGetInfo get_info_req{};
    get_info_req.id = 42;
    rc = iss_quest_kgsl_ioctl_with_fn(&MockQuestKernelIoctl, 99, kIoctlKgslGpumemGetInfo, &get_info_req);
    assert(rc == 0);
    assert(get_info_req.gpuaddr == 0x700012340000ULL);
    assert((get_info_req.flags & 0x10000000U) == 0);
    assert(get_info_req.size == 65536);
    assert(get_info_req.useraddr == 0x7f0000000000ULL);

    // 2. Test GPUMEM_FREE_ID -> GPUOBJ_FREE fallback.
    KgslGpumemFreeId free_req{};
    free_req.id = 42;
    rc = iss_quest_kgsl_ioctl_with_fn(&MockQuestKernelIoctl, 99, kIoctlKgslGpumemFreeId, &free_req);
    assert(rc == 0);
    assert(g_last_freed_id == 42);

    // 3. Test cleanup when GPUOBJ_INFO fails after GPUOBJ_ALLOC succeeds.
    g_last_freed_id = 0;
    g_fail_gpuobj_info = true;
    rc = iss_quest_kgsl_ioctl_with_fn(&MockQuestKernelIoctl, 99, kIoctlKgslGpumemAllocId, &alloc_req);
    assert(rc == -1 && errno == EINVAL);
    assert(g_last_freed_id == 42);
    g_fail_gpuobj_info = false;

    // 4. Test CMDSTREAM_READTIMESTAMP_CTXTID (0xc00c0916) fallback using GPU_COMMAND tracking + WAITTIMESTAMP.
    for (unsigned int ts : {10u, 11u, 12u}) {
        KgslGpuCommand cmd{};
        cmd.context_id = 7;
        cmd.timestamp = ts;
        rc = iss_quest_kgsl_ioctl_with_fn(&MockQuestKernelIoctl, 99, kIoctlKgslGpuCommand, &cmd);
        assert(rc == 0);
    }
    KgslCmdstreamReadtimestampCtxtid read_ts{};
    read_ts.context_id = 7;
    read_ts.type = 1; // KGSL_TIMESTAMP_QUEUED
    rc = iss_quest_kgsl_ioctl_with_fn(&MockQuestKernelIoctl, 99, kIoctlKgslCmdstreamReadtimestampCtxtid, &read_ts);
    assert(rc == 0 && read_ts.timestamp == 12);

    g_retired_on_gpu = 11;
    read_ts.type = 3; // KGSL_TIMESTAMP_RETIRED
    read_ts.timestamp = 0;
    rc = iss_quest_kgsl_ioctl_with_fn(&MockQuestKernelIoctl, 99, kIoctlKgslCmdstreamReadtimestampCtxtid, &read_ts);
    assert(rc == 0 && read_ts.timestamp == 11);

    // 5. Test ELF GOT patching on a real dlopen'd shared library.
    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    assert(lib != nullptr);
    using AllocBoFn = int (*)(int, uint64_t*, unsigned int*);
    auto fn = reinterpret_cast<AllocBoFn>(dlsym(lib, "mock_driver_alloc_bo"));
    assert(fn != nullptr);

    int patched = iss_patch_elf_ioctl_got(reinterpret_cast<void*>(fn), argv[1],
                                          reinterpret_cast<void*>(&ReplacementIoctlForGotTest));
    assert(patched >= 1);

    uint64_t gpuaddr = 0;
    unsigned int bo_id = 0;
    rc = fn(99, &gpuaddr, &bo_id);
    assert(rc == 0);
    assert(gpuaddr == 0x700012340000ULL);
    assert(bo_id == 42);

    dlclose(lib);
    return 0;
}
"""
        with tempfile.TemporaryDirectory() as temp:
            p = Path(temp)
            shutil.copy2(loader_cpp, p / "vk_driver_loader.cpp")
            shutil.copy2(loader_h, p / "vk_driver_loader.h")
            (p / "mock_driver.cpp").write_text(mock_driver_src)
            (p / "harness.cpp").write_text(harness_src)

            drv_compile = subprocess.run(
                ["c++", "-std=c++20", "-O2", "-fPIC", "-shared",
                 str(p / "mock_driver.cpp"), "-o", str(p / "libvulkan_freedreno.so")],
                capture_output=True, text=True
            )
            self.assertEqual(0, drv_compile.returncode, drv_compile.stderr)

            harness_compile = subprocess.run(
                ["c++", "-std=c++20", "-O2",
                 str(p / "harness.cpp"), str(p / "vk_driver_loader.cpp"),
                 "-ldl", "-lpthread", "-o", str(p / "harness")],
                capture_output=True, text=True
            )
            self.assertEqual(0, harness_compile.returncode, harness_compile.stderr)

            run_res = subprocess.run(
                [str(p / "harness"), str(p / "libvulkan_freedreno.so")],
                capture_output=True, text=True
            )
            self.assertEqual(0, run_res.returncode, run_res.stderr)

    def test_qualcomm_compute_opt_disable_and_lds_workgroup_clamp_are_wired(self):
        profile_h = (ROOT / "port/gpu/shadps4/shader_recompiler/profile.h").read_text()
        self.assertIn("bool needs_shared_atomic_minmax_cas{};", profile_h)

        cache_cpp = (
            ROOT / "port/gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp"
        ).read_text()
        self.assertIn(".needs_shared_atomic_minmax_cas =", cache_cpp)
        self.assertIn("instance.GetDriverID() == vk::DriverId::eQualcommProprietary", cache_cpp)
        cas_block = cache_cpp.split(".needs_shared_atomic_minmax_cas =", 1)[1].split("}(),", 1)[0]
        self.assertNotIn("#ifdef __ANDROID__", cas_block)

        atomic_cpp = (
            ROOT / "port/gpu/shadps4/shader_recompiler/backend/spirv/emit_spirv_atomic.cpp"
        ).read_text()
        for helper in (
            "SharedAtomicMinMaxCasImpl",
            "SharedAtomicBinOpCasImpl",
            "SharedAtomicCmpSwapNonAtomicImpl",
        ):
            self.assertIn(helper, atomic_cpp)
            body = atomic_cpp.split(f"Id {helper}(", 1)[1].split("\n}\n", 1)[0]
            self.assertNotIn("OpAtomic", body)
        self.assertIn("ctx.OpAtomicCompareExchange", atomic_cpp)
        for fn in (
            "EmitSharedAtomicIAdd32",
            "EmitSharedAtomicIAdd64",
            "EmitSharedAtomicISub32",
            "EmitSharedAtomicISub64",
            "EmitSharedAtomicUMax32",
            "EmitSharedAtomicUMax64",
            "EmitSharedAtomicSMax32",
            "EmitSharedAtomicSMax64",
            "EmitSharedAtomicUMin32",
            "EmitSharedAtomicUMin64",
            "EmitSharedAtomicSMin32",
            "EmitSharedAtomicSMin64",
            "EmitSharedAtomicAnd32",
            "EmitSharedAtomicAnd64",
            "EmitSharedAtomicOr32",
            "EmitSharedAtomicOr64",
            "EmitSharedAtomicXor32",
            "EmitSharedAtomicXor64",
            "EmitSharedAtomicCmpSwap32",
            "EmitSharedAtomicCmpSwap64",
            "EmitSharedAtomicInc32",
            "EmitSharedAtomicInc64",
            "EmitSharedAtomicDec32",
            "EmitSharedAtomicDec64",
        ):
            self.assertIn(fn, atomic_cpp)
        for cmp_fn in ("EmitSharedAtomicCmpSwap32", "EmitSharedAtomicCmpSwap64"):
            cmp_body = atomic_cpp.split(f"Id {cmp_fn}(", 1)[1].split("\n}\n", 1)[0]
            self.assertIn("ctx.profile.needs_shared_atomic_minmax_cas", cmp_body)
            self.assertIn("SharedAtomicCmpSwapNonAtomicImpl", cmp_body)

        compute_cpp = (
            ROOT / "port/gpu/shadps4/video_core/renderer_vulkan/vk_compute_pipeline.cpp"
        ).read_text()
        self.assertIn("vk::DriverId::eQualcommProprietary", compute_cpp)
        self.assertIn("vk::PipelineCreateFlagBits::eDisableOptimization", compute_cpp)
        self.assertIn("ISS_QCOM_CS_OPT", compute_cpp)
        self.assertIn("ISS_QCOM_CS_NOOP_FALLBACK", compute_cpp)
        self.assertIn("noop_spirv", compute_cpp)

        self.assertIn("bool needs_shared_atomics_to_storage{};", profile_h)
        self.assertIn(".needs_shared_atomics_to_storage =", cache_cpp)
        self.assertIn("ISS_QCOM_LDS_TO_SSBO", cache_cpp)

        lds_pass_cpp = (
            ROOT / "port/gpu/shadps4/shader_recompiler/ir/passes/shared_memory_to_storage_pass.cpp"
        ).read_text()
        self.assertIn("profile.needs_shared_atomics_to_storage", lds_pass_cpp)
        self.assertIn("HasSharedAtomics(program)", lds_pass_cpp)

        spirv_ctx_cpp = (
            ROOT / "port/gpu/shadps4/shader_recompiler/backend/spirv/spirv_emit_context.cpp"
        ).read_text()
        self.assertIn("std::max(1u, Common::DivCeil(shared_memory_size, element_size))", spirv_ctx_cpp)

        ser_cpp = (
            ROOT / "port/gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_serialization.cpp"
        ).read_text()
        self.assertIn("ShaderBinaryVersion = 16u;", ser_cpp)

        mem_c = (ROOT / "port/src/runtime_memory.c").read_text()
        self.assertIn("ISS_DIRECT_MEM_MB", mem_c)
        self.assertIn("mb = 4608;", mem_c)
        self.assertIn("direct_mem.bin", mem_c)
        self.assertIn("unlink(path);", mem_c)

        tex_cpp = (
            ROOT / "port/gpu/shadps4/video_core/texture_cache/texture_cache.cpp"
        ).read_text()
        view_cpp = (
            ROOT / "port/gpu/shadps4/video_core/texture_cache/image_view.cpp"
        ).read_text()
        self.assertIn("TLG_DROP_MIP0", tex_cpp)
        self.assertIn("TLG_DROP_MIP0", view_cpp)
        self.assertIn("ISS_ANDROID_BIONIC", tex_cpp)
        self.assertIn("ISS_ANDROID_BIONIC", view_cpp)

        probe_c = (ROOT / "port/src/probe.c").read_text()
        self.assertIn("--warmup-only", probe_c)
        self.assertIn("tlggpu_warmup_only(&gpu)", probe_c)
        self.assertLess(
            probe_c.index("tlggpu_warmup_only(&gpu)"),
            probe_c.index("guest_cpu_init(runtime_low_map)"),
        )

        main_java = (
            ROOT / "android/app/src/ro/cobrabm/fexdroid/BionicMainActivity.java"
        ).read_text()
        self.assertIn("Pré-compilar Shaders", main_java)
        self.assertIn("precompileShaders", main_java)
        self.assertIn("TLG_BIONIC_DRIVER", main_java)
        self.assertIn("Warmup-only: summary ", main_java)

        # QGL segfaults on ANY @llvm.qgpu.atomic.*.oob.p3i32.i32 intrinsic on Workgroup memory.
        self.assertNotIn("ctx.OpAtomicLoad(", atomic_cpp)
        self.assertEqual(3, atomic_cpp.count("const Id old_val{ctx.OpLoad(type, pointer)};"))

        if shutil.which("c++"):
            with tempfile.TemporaryDirectory() as temp:
                p = Path(temp)
                stub_dir = p / "shader_recompiler/backend/spirv"
                stub_dir.mkdir(parents=True)
                (stub_dir / "spirv_emit_context.h").write_text(r"""
#pragma once
#include <cstdint>
#include <sirit/sirit.h>
using u32 = std::uint32_t;
using u64 = std::uint64_t;
#define UNREACHABLE_MSG(...) __builtin_trap()
namespace Shader::IR { class Inst; }
namespace Shader::Backend::SPIRV {
using Sirit::Id;
struct ProfileStub {
    bool needs_shared_atomic_minmax_cas{};
    bool supports_buffer_fp32_atomic_min_max{};
    bool supports_image_fp32_atomic_min_max{};
};
class EmitContext : public Sirit::Module {
public:
    enum class PointerType { U32, F32, U64 };
    enum class PointerSize { B32, B64 };
    struct BufferStub {
        Id Offset(PointerSize) const { return {}; }
        std::pair<Id, Id> Alias(PointerType) const { return {}; }
    };
    struct ImageStub { Id id{}; };
    Id ConstU32(u32) { return {}; }
    Id EmitSharedMemoryAccess(Id, Id, Id) { return {}; }
    Id u32_zero_value{};
    Id true_value{};
    Id U32[5]{};
    Id F32[5]{};
    Id U1[5]{};
    Id U64{};
    Id shared_u32{}, shared_memory_u32{};
    Id shared_u64{}, shared_memory_u64{};
    Id image_u32{}, image_f32{};
    BufferStub buffers[16]{};
    ImageStub images[16]{};
    ProfileStub profile{};
};
} // namespace Shader::Backend::SPIRV
""")
                (stub_dir / "emit_spirv_instructions.h").write_text(
                    (ROOT / "port/gpu/shadps4/shader_recompiler/backend/spirv/emit_spirv_instructions.h")
                    .read_text()
                    .replace(
                        '#include "common/types.h"',
                        "#include <cstdint>\nusing u32 = std::uint32_t;\nusing u64 = std::uint64_t;\n",
                    )
                    .replace('#include "shader_recompiler/ir/value.h"', "namespace Shader::IR { class Inst; }")
                )
                chk = subprocess.run(
                    [
                        "c++", "-std=c++20", "-fsyntax-only",
                        "-I", str(p),
                        "-I", str(ROOT / "port/gpu/third_party/sirit/include"),
                        "-I", str(ROOT / "port/gpu/third_party/sirit/externals/SPIRV-Headers/include"),
                        str(ROOT / "port/gpu/shadps4/shader_recompiler/backend/spirv/emit_spirv_atomic.cpp"),
                    ],
                    capture_output=True, text=True
                )
                self.assertEqual(0, chk.returncode, chk.stderr)


if __name__ == "__main__":
    unittest.main()
