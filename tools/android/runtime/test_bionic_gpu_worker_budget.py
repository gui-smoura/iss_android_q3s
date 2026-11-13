"""Compile the actual worker policies, checking Bionic isolation and A/B overrides."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test_bionic_resource_reuse import method

ROOT = Path(__file__).resolve().parents[3]
GPU = ROOT / "port/gpu/shadps4/video_core/renderer_vulkan"
CXX = shutil.which("c++")


@unittest.skipUnless(CXX, "C++ compiler unavailable")
class GpuWorkerBudgetTests(unittest.TestCase):
    def check_policy(self, flags, bionic):
        prep = method((GPU / "vk_draw_prep.cpp").read_text(), "u32 DefaultWorkerCount()")
        pipe = method((GPU / "vk_rasterizer.cpp").read_text(), "bool Rasterizer::DrawPipeWanted()")
        cache = (GPU / "vk_pipeline_cache.cpp").read_text()
        start = cache.index("#if defined(__ANDROID__) && defined(ISS_ANDROID_BIONIC)",
                            cache.index("// tlg: the compiler threads always exist"))
        budget = cache[start:cache.index("        for (unsigned i = 0; i < count; ++i)", start)]
        code = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <utility>
using u32 = uint32_t;
namespace TlgThreads {unsigned cores=8; unsigned Available() {return cores;}}
struct Rasterizer {static bool DrawPipeWanted();};
''' + prep + pipe + "\nstd::pair<unsigned,unsigned> Budget() {\n" + budget + r'''
    // The production initializer does not capture this constexpr in its lambda.
    auto capture_priority = [] {return native_bionic ? 0 : 1;};
    assert(capture_priority() == int(!native_bionic));
    return {count,capture_count};
}
int main() {
    unsetenv("TLG_PREP_WORKERS");unsetenv("TLG_DRAW_PIPE");
    unsetenv("TLG_GPU_COMPILER_WORKERS");
    constexpr bool bionic=EXPECTED_BIONIC;
    for(unsigned cores : {2u,8u,32u}) {
        TlgThreads::cores=cores;
        assert(DefaultWorkerCount()==(bionic ? 0u : std::clamp(cores/2,1u,8u)));
        assert(Rasterizer::DrawPipeWanted()==(!bionic && cores>=8));
        auto [compile,capture]=Budget();
        assert(compile==(bionic ? 1u : std::clamp(cores/2,1u,6u)));
        assert(capture==(bionic ? 1u : std::min(2u,std::max(1u,cores/4))));
    }
    setenv("TLG_PREP_WORKERS","4",1);assert(DefaultWorkerCount()==4);
    setenv("TLG_PREP_WORKERS","99",1);assert(DefaultWorkerCount()==16);
    setenv("TLG_PREP_WORKERS","-1",1);assert(DefaultWorkerCount()==0);
    setenv("TLG_DRAW_PIPE","1",1);assert(Rasterizer::DrawPipeWanted());
    setenv("TLG_DRAW_PIPE","0",1);assert(!Rasterizer::DrawPipeWanted());
    TlgThreads::cores=8;
    for(const char* value : {"1","6"}) {
        setenv("TLG_GPU_COMPILER_WORKERS",value,1);
        assert(Budget().first==unsigned(std::atoi(value)));
    }
    for(const char* value : {"0","7","-1","","2junk"}) {
        setenv("TLG_GPU_COMPILER_WORKERS",value,1);
        assert(Budget().first==(bionic ? 1u : 4u));
    }
}
'''
        with tempfile.TemporaryDirectory() as directory:
            src = Path(directory) / "policy.cpp"
            binary = Path(directory) / "policy"
            src.write_text(code)
            subprocess.run([CXX, "-std=c++20", "-Wall", "-Wextra", "-Werror", *flags,
                            f"-DEXPECTED_BIONIC={int(bionic)}", str(src), "-o", str(binary)],
                           check=True, capture_output=True, text=True)
            subprocess.run([str(binary)], check=True, capture_output=True, text=True)

    def test_native_bionic(self):
        self.check_policy(["-D__ANDROID__=1", "-DISS_ANDROID_BIONIC=1"], True)

    def test_glibc_keeps_existing_defaults(self):
        self.check_policy(["-DISS_ANDROID_GLIBC=1"], False)

    def test_other_android_keeps_existing_defaults(self):
        self.check_policy(["-D__ANDROID__=1"], False)


if __name__ == "__main__":
    unittest.main()
