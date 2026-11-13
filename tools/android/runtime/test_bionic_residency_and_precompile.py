"""Unit tests and source invariants for BufferCache residency eviction, VK_INCOMPLETE handling,
precompilation job discovery / compute safety, and launcher warmup progress parsing."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
SHADPS4 = ROOT / "port/gpu/shadps4"
RENDERER = SHADPS4 / "video_core/renderer_vulkan"
BUFFER_CACHE = SHADPS4 / "video_core/buffer_cache"
SHIM = ROOT / "port/gpu/shim"
JAVA_DIR = ROOT / "android/app/src/ro/cobrabm/fexdroid"
CXX = shutil.which("c++")
JAVA = shutil.which("java")


def slice_between(text: str, start_marker: str, end_marker: str) -> str:
    start = text.index(start_marker)
    end = text.index(end_marker, start)
    return text[start:end]


class BionicResidencyAndPrecompileTests(unittest.TestCase):
    @unittest.skipUnless(CXX, "C++ compiler unavailable")
    def test_interval_list_remove_real_header(self):
        code = r'''
#include <cassert>
#include <vector>
#include "common/interval_set.h"

struct TaggedInterval : public Interval {
    int tag{};
    constexpr bool CanMergeWith(const TaggedInterval& other) const noexcept {
        return tag == other.tag;
    }
    constexpr TaggedInterval SubRange(u64 a, u64 b) const noexcept {
        return {{a, b}, tag};
    }
};

static void CheckIntervals(const IntervalList<TaggedInterval>& list,
                           std::vector<TaggedInterval> expected) {
    assert(list.Size() == expected.size());
    size_t i = 0;
    for (const auto& iv : list) {
        assert(iv.start == expected[i].start);
        assert(iv.end == expected[i].end);
        assert(iv.tag == expected[i].tag);
        ++i;
    }
}

int main() {
    IntervalList<TaggedInterval> list;
    list.Add({{0, 100}, 1});
    // Empty or inverted remove is a no-op.
    list.Remove(50, 50);
    list.Remove(60, 40);
    // Disjoint remove before / after is a no-op.
    list.Remove(100, 120);
    CheckIntervals(list, {{{0, 100}, 1}});

    // Middle split preserves both ends and tags.
    list.Remove(20, 40);
    CheckIntervals(list, {{{0, 20}, 1}, {{40, 100}, 1}});

    // Trim trailing edge of first and leading edge of second across gap.
    list.Remove(10, 60);
    CheckIntervals(list, {{{0, 10}, 1}, {{60, 100}, 1}});

    // Remove spanning multiple intervals completely.
    list.Add({{20, 30}, 2});
    list.Add({{30, 50}, 3});
    list.Remove(5, 80);
    CheckIntervals(list, {{{0, 5}, 1}, {{80, 100}, 1}});

    // Exact interval removal leaves list intact and eventually empty.
    list.Remove(0, 5);
    CheckIntervals(list, {{{80, 100}, 1}});
    list.Remove(70, 120);
    assert(list.Empty());
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            src = tmp / "test_interval.cpp"
            binary = tmp / "test_interval"
            src.write_text(code)
            subprocess.run(
                [CXX, "-std=c++20", "-Wall", "-Wextra", "-Wno-sign-compare", "-Werror",
                 f"-I{SHADPS4}", str(src), "-o", str(binary)],
                check=True, capture_output=True, text=True,
            )
            subprocess.run([str(binary)], check=True, capture_output=True, text=True)

    @unittest.skipUnless(CXX, "C++ compiler unavailable")
    def test_vk_platform_e_incomplete_real_header(self):
        code = r'''
#include <algorithm>
#include <cassert>
#include <cstddef>
#include "video_core/renderer_vulkan/vk_platform.h"

int g_asserts = 0;
int g_warns = 0;

int main() {
    static_assert(Vulkan::IsVulkanOk(vk::Result::eSuccess));
    static_assert(Vulkan::IsVulkanOk(vk::Result::eIncomplete));
    static_assert(!Vulkan::IsVulkanOk(vk::Result::eErrorDeviceLost));

    // Check(vk::Result) accepts eSuccess and eIncomplete without asserting.
    Vulkan::Check(vk::Result::eSuccess);
    Vulkan::Check<"query">(vk::Result::eIncomplete);
    assert(g_asserts == 0);
    assert(g_warns == 1);

    // Check(vk::ResultValue<T>) returns moved value on eIncomplete.
    auto val = Vulkan::Check<"merge">(vk::ResultValue<int>{vk::Result::eIncomplete, 42});
    assert(val == 42);
    assert(g_asserts == 0);
    assert(g_warns == 2);

    // Rate limiting: logs 1..8 and powers of 2 thereafter.
    for (int i = 3; i <= 20; ++i) {
        Vulkan::WarnIncomplete("ctx");
    }
    // Calls 1..8, 16 => 9 warnings out of 20 calls.
    assert(g_warns == 9);

    // Fatal error still hits ASSERT_MSG.
    Vulkan::Check<"create">(vk::Result::eErrorDeviceLost);
    assert(g_asserts == 1);
    Vulkan::Check(vk::ResultValue<int>{vk::Result::eErrorOutOfHostMemory, 0});
    assert(g_asserts == 2);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            inc = tmp / "stubs"
            (inc / "common/logging").mkdir(parents=True)
            (inc / "fmt").mkdir(parents=True)
            (inc / "video_core/renderer_vulkan").mkdir(parents=True)

            (inc / "common/assert.h").write_text(r'''
#pragma once
extern int g_asserts;
#define ASSERT_MSG(cond, ...) do { if (!(cond)) { ++g_asserts; } } while (0)
''')
            (inc / "common/logging/log.h").write_text(r'''
#pragma once
extern int g_warns;
#define LOG_WARNING(cls, ...) do { ++g_warns; } while (0)
#define LOG_DEBUG(cls, ...) do {} while (0)
''')
            (inc / "common/string_literal.h").write_text((SHADPS4 / "common/string_literal.h").read_text())
            (inc / "common/types.h").write_text((SHADPS4 / "common/types.h").read_text())
            (inc / "fmt/format.h").write_text(r'''
#pragma once
#include <string>
#include <string_view>
namespace fmt {
struct format_args {};
template <typename... Args>
inline format_args make_format_args(const Args&...) { return {}; }
inline std::string vformat(std::string_view fmt, format_args) { return std::string(fmt); }
}
''')
            (inc / "video_core/renderer_vulkan/vk_common.h").write_text(r'''
#pragma once
#include <string>
#include <utility>
#define VK_API_VERSION_1_3 13
using VkSurfaceKHR = void*;
namespace vk {
enum class Result { eSuccess = 0, eIncomplete = 5, eErrorOutOfHostMemory = -1, eErrorDeviceLost = -4 };
template <typename T>
struct ResultValue { Result result{}; T value{}; };
class SurfaceKHR {};
class Instance {};
struct DebugUtilsObjectNameInfoEXT {};
class Device {
public:
    Result setDebugUtilsObjectNameEXT(const DebugUtilsObjectNameInfoEXT&) const { return Result::eSuccess; }
};
class UniqueInstance {};
class UniqueDebugUtilsMessengerEXT {};
template <typename T> struct isVulkanHandleType : std::false_type {};
inline std::string to_string(Result r) { return std::to_string(static_cast<int>(r)); }
}
''')
            src = tmp / "test_vk_platform.cpp"
            binary = tmp / "test_vk_platform"
            src.write_text(code)
            # Compile with stubs first, fallback only for vk_platform.h itself via explicit -include path.
            plat_dir = tmp / "real_plat/video_core/renderer_vulkan"
            plat_dir.mkdir(parents=True)
            (plat_dir / "vk_platform.h").write_text((RENDERER / "vk_platform.h").read_text())
            subprocess.run(
                [CXX, "-std=c++20", "-Wall", "-Wextra", "-Werror",
                 f"-I{tmp / 'real_plat'}", f"-I{inc}", str(src), "-o", str(binary)],
                check=True, capture_output=True, text=True,
            )
            subprocess.run([str(binary)], check=True, capture_output=True, text=True)

    @unittest.skipUnless(CXX, "C++ compiler unavailable")
    def test_residency_granule_and_budget_policies(self):
        bc_cpp = (BUFFER_CACHE / "buffer_cache.cpp").read_text()
        parse_fn = slice_between(bc_cpp, "bool TryParseStrictUnsignedMb(", "} // namespace")
        policies = slice_between(bc_cpp, "u64 BufferCache::ResidencyGranuleSize()", "u64 BufferCache::MemoryKey(")

        for flags, bionic in (
            (["-D__ANDROID__=1", "-DISS_ANDROID_BIONIC=1"], True),
            (["-D__ANDROID__=1"], False),
            ([], False),
        ):
            # Strip static locals inside test wrapper so env overrides can be re-evaluated per call.
            granule_fn = policies[:policies.index("u64 BufferCache::ResidencyBudgetBytes()")]
            budget_fn = policies[policies.index("u64 BufferCache::ResidencyBudgetBytes()"):]
            granule_fn = granule_fn.replace("BufferCache::", "PolicyGranule_").replace("static const u64 bytes =", "const u64 bytes =")
            budget_fn = budget_fn.replace("BufferCache::", "PolicyBudget_").replace("static const u64 bytes =", "const u64 bytes =")

            code = f'''
#include <algorithm>
#include <bit>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
using u64 = uint64_t;

{parse_fn}

{granule_fn}
{budget_fn}

int main() {{
    unsetenv("ISS_RESIDENCY_GRANULE_MB");
    unsetenv("TLG_RESIDENCY_GRANULE_MB");
    unsetenv("ISS_BUFFER_CACHE_BUDGET_MB");
    unsetenv("TLG_BUFFER_CACHE_BUDGET_MB");

    constexpr bool bionic = {int(bionic)};
    assert(PolicyGranule_ResidencyGranuleSize() == (bionic ? (4ull << 20) : (16ull << 20)));
    assert(PolicyBudget_ResidencyBudgetBytes() == (bionic ? (1024ull << 20) : 0ull));

    // ISS_* takes precedence over TLG_*, non-powers floor, clamped [1, 64] MiB.
    setenv("TLG_RESIDENCY_GRANULE_MB", "32", 1);
    assert(PolicyGranule_ResidencyGranuleSize() == (32ull << 20));
    setenv("ISS_RESIDENCY_GRANULE_MB", "12", 1);
    assert(PolicyGranule_ResidencyGranuleSize() == (8ull << 20));
    setenv("ISS_RESIDENCY_GRANULE_MB", "0", 1);
    assert(PolicyGranule_ResidencyGranuleSize() == (1ull << 20));
    setenv("ISS_RESIDENCY_GRANULE_MB", "999", 1);
    assert(PolicyGranule_ResidencyGranuleSize() == (64ull << 20));
    // Invalid strict unsigned string is ignored.
    setenv("ISS_RESIDENCY_GRANULE_MB", "16junk", 1);
    assert(PolicyGranule_ResidencyGranuleSize() == (32ull << 20));
    unsetenv("TLG_RESIDENCY_GRANULE_MB");
    setenv("ISS_RESIDENCY_GRANULE_MB", "-4", 1);
    assert(PolicyGranule_ResidencyGranuleSize() == (bionic ? (4ull << 20) : (16ull << 20)));

    // Budget policy: 0 disables even on Bionic, otherwise clamped [128, 4096] MiB.
    setenv("ISS_BUFFER_CACHE_BUDGET_MB", "0", 1);
    assert(PolicyBudget_ResidencyBudgetBytes() == 0ull);
    setenv("ISS_BUFFER_CACHE_BUDGET_MB", "64", 1);
    assert(PolicyBudget_ResidencyBudgetBytes() == (128ull << 20));
    setenv("ISS_BUFFER_CACHE_BUDGET_MB", "8192", 1);
    assert(PolicyBudget_ResidencyBudgetBytes() == (4096ull << 20));
    setenv("ISS_BUFFER_CACHE_BUDGET_MB", "abc", 1);
    setenv("TLG_BUFFER_CACHE_BUDGET_MB", "768", 1);
    assert(PolicyBudget_ResidencyBudgetBytes() == (768ull << 20));
    return 0;
}}
'''
            with tempfile.TemporaryDirectory() as directory:
                tmp = Path(directory)
                src = tmp / "test_policy.cpp"
                binary = tmp / "test_policy"
                src.write_text(code)
                subprocess.run(
                    [CXX, "-std=c++20", "-Wall", "-Wextra", "-Werror", *flags, str(src), "-o", str(binary)],
                    check=True, capture_output=True, text=True,
                )
                subprocess.run([str(binary)], check=True, capture_output=True, text=True)

    @unittest.skipUnless(CXX, "C++ compiler unavailable")
    def test_precompile_thread_budget_policy(self):
        pre_cpp = (RENDERER / "vk_precompile.cpp").read_text()
        build_threads_fn = slice_between(pre_cpp, "unsigned BuildThreads()", "} // namespace")

        for flags, bionic in (
            (["-D__ANDROID__=1", "-DISS_ANDROID_BIONIC=1"], True),
            (["-D__ANDROID__=1"], False),
            ([], False),
        ):
            code = f'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdlib>
using u32 = uint32_t;
namespace TlgThreads {{ unsigned cores = 8; unsigned Available() {{ return cores; }} }}

{build_threads_fn}

int main() {{
    unsetenv("ISS_PRECOMPILE_THREADS");
    unsetenv("TLG_PRECOMPILE_THREADS");
    constexpr bool bionic = {int(bionic)};

    TlgThreads::cores = 4;
    assert(BuildThreads() == (bionic ? 1u : 3u));
    TlgThreads::cores = 8;
    assert(BuildThreads() == (bionic ? 2u : 7u));
    TlgThreads::cores = 16;
    assert(BuildThreads() == (bionic ? 2u : 15u));

    setenv("TLG_PRECOMPILE_THREADS", "3", 1);
    assert(BuildThreads() == 3u);
    setenv("ISS_PRECOMPILE_THREADS", "0", 1);
    assert(BuildThreads() == 0u);
    return 0;
}}
'''
            with tempfile.TemporaryDirectory() as directory:
                tmp = Path(directory)
                src = tmp / "test_threads.cpp"
                binary = tmp / "test_threads"
                src.write_text(code)
                subprocess.run(
                    [CXX, "-std=c++20", "-Wall", "-Wextra", "-Werror", *flags, str(src), "-o", str(binary)],
                    check=True, capture_output=True, text=True,
                )
                subprocess.run([str(binary)], check=True, capture_output=True, text=True)

    @unittest.skipUnless(JAVA, "Java unavailable")
    def test_java_warmup_fraction_parser(self):
        main_java = (JAVA_DIR / "BionicMainActivity.java").read_text()
        warmup_field = slice_between(main_java, "private static String warmupField(", "private static int[] parseWarmupFraction(")
        parse_frac = slice_between(main_java, "private static int[] parseWarmupFraction(", "private void precompileShaders(")
        test_java = f'''package ro.cobrabm.fexdroid;
import java.util.Arrays;
public class WarmupParseTest {{
 {warmup_field}
 {parse_frac}
 public static void main(String[] args) {{
  assert warmupField("precompiled=10/20 pipelines=5/5 shaders=12 status=compiled (Turnip)", "status").equals("compiled");
  assert warmupField("precompiled=0/0 status=up_to_date", "missing") == null;
  assert warmupField(null, "status") == null;

  assert Arrays.equals(parseWarmupFraction("Warmup-only: precompile 1 / 500", "Warmup-only: precompile "), new int[]{{1, 500}});
  assert Arrays.equals(parseWarmupFraction("Warmup-only: precompile 600 / 500 (extra)", "Warmup-only: precompile "), new int[]{{500, 500}});
  assert Arrays.equals(parseWarmupFraction("Warmup-only: pipeline cache 42 / 100", "Warmup-only: pipeline cache "), new int[]{{42, 100}});
  assert parseWarmupFraction("Warmup-only: precompile 0 / 0", "Warmup-only: precompile ") == null;
  assert parseWarmupFraction("Warmup-only: precompile abc / 10", "Warmup-only: precompile ") == null;
  assert parseWarmupFraction("Warmup-only: pipeline cache 1 / 10", "Warmup-only: precompile ") == null;
  assert parseWarmupFraction(null, "Warmup-only: precompile ") == null;
 }}
}}
'''
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            src = tmp / "WarmupParseTest.java"
            src.write_text(test_java)
            res = subprocess.run(
                [JAVA, "com.sun.tools.javac.Main", "-encoding", "UTF-8", "-d", str(tmp), str(src)],
                capture_output=True, text=True,
            )
            self.assertEqual(res.returncode, 0, res.stderr)
            subprocess.run([JAVA, "-ea", "-cp", str(tmp), "ro.cobrabm.fexdroid.WarmupParseTest"],
                           check=True, capture_output=True, text=True)

    def test_source_invariants_eviction_precompile_and_cache_save(self):
        bc_cpp = (BUFFER_CACHE / "buffer_cache.cpp").read_text()
        self.assertIn("ResidencyGranuleSize()", bc_cpp)
        self.assertIn("ResidencyBudgetBytes()", bc_cpp)
        # TouchResidencyRange must precede the allocation retry loop in EnsureResident.
        self.assertLess(
            bc_cpp.index("TouchResidencyRange(bind_first, bind_end);"),
            bc_cpp.index("for (u32 attempt = 0;; ++attempt)"),
        )
        # EvictUnusedResidency drains in-flight work before setting evicting_residency.
        evict_fn = bc_cpp[bc_cpp.index("u64 BufferCache::EvictUnusedResidency("):bc_cpp.index("void BufferCache::RunGarbageCollector()")]
        self.assertLess(evict_fn.index("scheduler.PopPendingOperations();"), evict_fn.index("evicting_residency = true;"))
        self.assertIn("deferred_count.load(std::memory_order_acquire)", evict_fn)
        self.assertIn("FinishDeferredReadback(*rb, true)", evict_fn)
        self.assertIn("DownloadMemory(chunk.arena, addr, size);", evict_fn)
        self.assertIn("gpu_modified_ranges.Subtract(addr, size);", evict_fn)
        self.assertIn("memory_tracker->MarkRegionAsCpuModified(addr, size);", evict_fn)
        self.assertIn("runtime.FillBuffer(bda_pagetable_buffer.get(),", evict_fn)
        self.assertIn("resident_ranges.Remove(chunk.start_block, chunk.end_block);", evict_fn)
        # Flush and unbind GPU work before freeing device memory.
        self.assertLess(evict_fn.index("resident_ranges.Remove(chunk.start_block, chunk.end_block);"),
                        evict_fn.index("device.freeMemory(victim.alloc.memory);"))
        self.assertIn("EvictUnusedResidency(target, 2)", bc_cpp)

        rast_cpp = (RENDERER / "vk_rasterizer.cpp").read_text()
        on_submit = rast_cpp[rast_cpp.index("void Rasterizer::OnSubmit()"):rast_cpp.index("if (HugeWarmEnabled())")]
        self.assertIn("buffer_cache.RunGarbageCollector();", on_submit)

        cache_cpp = (RENDERER / "vk_pipeline_cache.cpp").read_text()
        self.assertIn('#include "video_core/renderer_vulkan/vk_platform.h"', cache_cpp)
        finish_fn = cache_cpp[cache_cpp.index("void PipelineCache::FinishCompilations()"):cache_cpp.index("bool PipelineCache::AsyncCompilation()")]
        self.assertIn("if (!sources.empty())", finish_fn)
        self.assertIn("g_tlg_driver_pipelines_created.fetch_add(1", finish_fn)

        pre_cpp = (RENDERER / "vk_precompile.cpp").read_text()
        self.assertIn('"ISS_PRECOMPILE_JOBS"', pre_cpp)
        self.assertIn('"/sdcard/Download"', pre_cpp)
        self.assertIn('"/storage/emulated/0/Download"', pre_cpp)
        self.assertIn('"CUSA00004"', pre_cpp)
        self.assertIn("const size_t queue_limit = caches.size() * 2;", pre_cpp)
        # Compute records must initialize regs before RefreshComputeKey.
        cs_block = pre_cpp[pre_cpp.index("selection.cs_regs = &cs;") - 120:pre_cpp.index("RefreshComputeKey(selection, key)")]
        self.assertIn("regs->reg_array.fill(0);", cs_block)
        self.assertIn("selection.regs = regs.get();", cs_block)
        self.assertIn("g_tlg_driver_pipelines_created.fetch_add(1", pre_cpp)

        shim_cpp = (SHIM / "tlggpu.cpp").read_text()
        warmup_fn = shim_cpp[shim_cpp.index('extern "C" int tlggpu_warmup_only'):shim_cpp.index('extern "C" int tlggpu_init(')]
        self.assertIn("Warmup-only: jobs=", warmup_fn)
        self.assertIn("Warmup-only: precompile ", warmup_fn)
        self.assertIn("status=%s", warmup_fn)
        self.assertIn('precompile_status == "up_to_date"', warmup_fn)
        self.assertIn("if (jobs_covered)", warmup_fn)
        self.assertIn("cache.SaveDriverCache()", warmup_fn)

        main_java = (JAVA_DIR / "BionicMainActivity.java").read_text()
        self.assertIn("progress.setIndeterminate(false)", main_java)
        self.assertIn('"up_to_date".equals(jobStatus)', main_java)
        self.assertIn('"gave_up".equals(jobStatus)', main_java)
        self.assertIn('"invalid_jobs".equals(jobStatus)', main_java)
        self.assertIn('"missing_jobs".equals(jobStatus)', main_java)
        self.assertIn("50_000_000L", main_java)


if __name__ == "__main__":
    unittest.main()
