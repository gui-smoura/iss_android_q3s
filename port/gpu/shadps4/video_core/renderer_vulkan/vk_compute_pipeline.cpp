// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <boost/container/small_vector.hpp>

#include "common/logging/log.h"
#include "shader_recompiler/info.h"
#include "video_core/renderer_vulkan/vk_async_compiler.h"
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

ComputePipeline::ComputePipeline(const Instance& instance, Scheduler& scheduler,
                                 DescriptorHeap& desc_heap, const Shader::Profile& profile,
                                 vk::PipelineCache pipeline_cache, ComputePipelineKey compute_key_,
                                 const Shader::Info& info_, vk::ShaderModule module,
                                 SerializationSupport& sdata, bool preloading /*=false*/,
                                 const ComputeBuilder* builder /*=nullptr*/)
    : Pipeline{instance, scheduler, desc_heap, profile, pipeline_cache, true},
      compute_key{compute_key_} {
    auto& info = stages[int(Shader::SwStage::Compute)];
    info = &info_;
    const auto debug_str = GetDebugString();

    u32 binding{};
    boost::container::small_vector<vk::DescriptorSetLayoutBinding, 32> bindings;
    for (const auto& buffer : info->buffers) {
        // During deserialization, we don't have access to the UD to fetch sharp data. To address
        // this properly we need to track shaprs or portion of them in `sdata`, but since we're
        // interested only in "is storage" flag (which is not even effective atm), we can take a
        // shortcut there.
        const auto sharp = preloading ? AmdGpu::Buffer{} : buffer.GetSharp(*info);
        bindings.push_back({
            .binding = binding++,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        });
    }
    for (const auto& image : info->images) {
        const u32 num_bindings = image.NumBindings(*info);
        bindings.push_back({
            .binding = binding,
            .descriptorType = image.is_written ? vk::DescriptorType::eStorageImage
                                               : vk::DescriptorType::eSampledImage,
            .descriptorCount = num_bindings,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        });
        binding += num_bindings;
    }
    for (const auto& sampler : info->samplers) {
        bindings.push_back({
            .binding = binding++,
            .descriptorType = vk::DescriptorType::eSampler,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        });
    }

    const vk::PushConstantRange push_constants = {
        .stageFlags = vk::ShaderStageFlagBits::eCompute,
        .offset = 0,
        .size = sizeof(Shader::PushData),
    };

    uses_push_descriptors = binding < instance.MaxPushDescriptors();
    const auto flags = uses_push_descriptors
                           ? vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR
                           : vk::DescriptorSetLayoutCreateFlagBits{};
    const vk::DescriptorSetLayoutCreateInfo desc_layout_ci = {
        .flags = flags,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    };
    const auto device = instance.GetDevice();
    auto [descriptor_set_result, descriptor_set] =
        device.createDescriptorSetLayoutUnique(desc_layout_ci);
    ASSERT_MSG(descriptor_set_result == vk::Result::eSuccess,
               "Failed to create compute descriptor set layout: {}",
               vk::to_string(descriptor_set_result));
    desc_layout = std::move(descriptor_set);

    const vk::DescriptorSetLayout set_layout = *desc_layout;
    const vk::PipelineLayoutCreateInfo layout_info = {
        .setLayoutCount = 1U,
        .pSetLayouts = &set_layout,
        .pushConstantRangeCount = 1U,
        .pPushConstantRanges = &push_constants,
    };
    auto [layout_result, layout] = instance.GetDevice().createPipelineLayoutUnique(layout_info);
    ASSERT_MSG(layout_result == vk::Result::eSuccess,
               "Failed to create compute pipeline layout: {}", vk::to_string(layout_result));
    pipeline_layout = std::move(layout);
    BuildDescriptorTemplate({bindings.data(), bindings.size()});
    SetObjectName(device, *pipeline_layout, "Compute PipelineLayout {}", debug_str);

    static const bool force_disable_qcom_cs_opt = [] {
        const char* env = std::getenv("ISS_QCOM_CS_OPT");
        return !(env && env[0] == '1');
    }();
    const bool disable_cs_opt =
        force_disable_qcom_cs_opt && instance.GetDriverID() == vk::DriverId::eQualcommProprietary;
    // Quest 3S: libllvm-qgl.so rejects a few shaders with ErrorUnknown (cs_0x00000000ce1675e0),
    // even after the retries. Instead of ending the game, try again without
    // eDisableOptimization, then without the required subgroup size, and as a last resort bind
    // an empty compute shader (same layout) so that dispatch does nothing.
    // ISS_QCOM_CS_NOOP_FALLBACK=0 keeps the old assert.
    static const bool qcom_noop_fallback_env = [] {
        const char* env = std::getenv("ISS_QCOM_CS_NOOP_FALLBACK");
        return !(env && env[0] == '0');
    }();
    const bool qcom_fallback =
        qcom_noop_fallback_env && instance.GetDriverID() == vk::DriverId::eQualcommProprietary;

    // Runs on the compiler threads too: it only uses what stays fixed after construction and
    // the layout, which the destructor keeps alive until the build is done. Returns no pipeline
    // when one may only come from the driver's cache and it isn't there.
    const auto create = [&instance, module, layout = *pipeline_layout, debug_str,
                         disable_cs_opt, qcom_fallback](vk::PipelineCache cache,
                                         vk::PipelineCreateFlags flags) -> vk::UniquePipeline {
        const auto effective_flags =
            disable_cs_opt ? (flags | vk::PipelineCreateFlagBits::eDisableOptimization) : flags;
        const vk::PipelineShaderStageRequiredSubgroupSizeCreateInfo subgroup_size_ci = {
            .requiredSubgroupSize = 64,
        };
        const vk::ComputePipelineCreateInfo compute_pipeline_ci = {
            .flags = effective_flags,
            .stage{
                .pNext = instance.IsSubgroupSize64Supported() ? &subgroup_size_ci : nullptr,
                .stage = vk::ShaderStageFlagBits::eCompute,
                .module = module,
                .pName = "main",
            },
            .layout = layout,
        };
        const auto device = instance.GetDevice();
        auto created = device.createComputePipelineUnique(cache, compute_pipeline_ci);
        for (u32 attempt = 1;
             RetryPipelineCreation(created.result, attempt, "compute", debug_str); ++attempt) {
            created = device.createComputePipelineUnique(cache, compute_pipeline_ci);
        }
        const bool cache_probe_miss =
            created.result == vk::Result::ePipelineCompileRequired &&
            (effective_flags & vk::PipelineCreateFlagBits::eFailOnPipelineCompileRequired);
        if (qcom_fallback && !cache_probe_miss && created.result != vk::Result::eSuccess) {
            const vk::Result first_error = created.result;
            if (disable_cs_opt) {
                auto relaxed_ci = compute_pipeline_ci;
                relaxed_ci.flags = flags;
                created = device.createComputePipelineUnique(cache, relaxed_ci);
            }
            if (created.result != vk::Result::eSuccess &&
                created.result != vk::Result::ePipelineCompileRequired) {
                auto plain_ci = compute_pipeline_ci;
                plain_ci.flags = flags;
                plain_ci.stage.pNext = nullptr;
                created = device.createComputePipelineUnique(cache, plain_ci);
            }
            if (created.result == vk::Result::ePipelineCompileRequired &&
                (flags & vk::PipelineCreateFlagBits::eFailOnPipelineCompileRequired)) {
                return {};
            }
            if (created.result == vk::Result::eSuccess) {
                LOG_WARNING(Render_Vulkan,
                            "iss: compute pipeline {} failed with {} and was built with fewer "
                            "driver flags",
                            debug_str, vk::to_string(first_error));
            } else {
                // Minimal SPIR-V: GLCompute "main", LocalSize 1 1 1, returns at once. It uses no
                // binding, so it is compatible with this pipeline's layout.
                static constexpr std::array<u32, 35> noop_spirv = {
                    0x07230203u, 0x00010000u, 0x00000000u, 0x00000005u, 0x00000000u, // header
                    0x00020011u, 0x00000001u,                           // OpCapability Shader
                    0x0003000Eu, 0x00000000u, 0x00000001u,              // OpMemoryModel GLSL450
                    0x0005000Fu, 0x00000005u, 0x00000001u, 0x6E69616Du, // OpEntryPoint GLCompute
                    0x00000000u,                                        //   %1 "main"
                    0x00060010u, 0x00000001u, 0x00000011u, 0x00000001u, // OpExecutionMode %1
                    0x00000001u, 0x00000001u,                           //   LocalSize 1 1 1
                    0x00020013u, 0x00000002u,                           // %2 = OpTypeVoid
                    0x00030021u, 0x00000003u, 0x00000002u,              // %3 = OpTypeFunction %2
                    0x00050036u, 0x00000002u, 0x00000001u, 0x00000000u, // %1 = OpFunction %2
                    0x00000003u,                                        //   None %3
                    0x000200F8u, 0x00000004u,                           // %4 = OpLabel
                    0x000100FDu,                                        // OpReturn
                    0x00010038u,                                        // OpFunctionEnd
                };
                auto [module_result, noop_module] = device.createShaderModuleUnique({
                    .codeSize = noop_spirv.size() * sizeof(u32),
                    .pCode = noop_spirv.data(),
                });
                if (module_result == vk::Result::eSuccess) {
                    auto noop_ci = compute_pipeline_ci;
                    noop_ci.flags = {};
                    noop_ci.stage.pNext = nullptr;
                    noop_ci.stage.module = *noop_module;
                    created = device.createComputePipelineUnique(cache, noop_ci);
                }
                if (created.result == vk::Result::eSuccess) {
                    LOG_WARNING(Render_Vulkan,
                                "iss: compute pipeline {} failed with {}: replaced by an empty "
                                "shader, its dispatches do nothing",
                                debug_str, vk::to_string(first_error));
                    std::printf("GPU: compute pipeline %s rejected by the Qualcomm driver (%s), "
                                "dispatches skipped\n",
                                std::string{debug_str}.c_str(),
                                vk::to_string(first_error).c_str());
                }
            }
        }
        if (created.result == vk::Result::ePipelineCompileRequired &&
            (effective_flags & vk::PipelineCreateFlagBits::eFailOnPipelineCompileRequired)) {
            return {};
        }
        if (created.result == vk::Result::eSuccess) {
            g_tlg_driver_pipelines_created.fetch_add(1, std::memory_order_relaxed);
        }
        ASSERT_MSG(created.result == vk::Result::eSuccess, "Failed to create compute pipeline: {}",
                   vk::to_string(created.result));
        SetObjectName(device, *created.value, "Compute Pipeline {}", debug_str);
        return std::move(created.value);
    };

    // tlg: a dispatch is waiting for a pipeline met in game, and compute work cannot be skipped
    // (5e7995ac skipped it: not taken, for fidelity). Built on the GPU thread, a new one took
    // 60-190 ms in inFAMOUS Second Son and an effect brings a few dozen: freezes of 0.6-3.8 s.
    // TLG_COMPUTE_ASYNC_BUILD=0 builds every one here as before.
    static const bool async_build = [] {
        const char* env = std::getenv("TLG_COMPUTE_ASYNC_BUILD");
        return !(env && env[0] == '0');
    }();
    if (builder && !preloading && async_build &&
        instance.IsPipelineCreationCacheControlSupported()) {
        // One the driver built before (its disk cache) comes at once.
        pipeline = create(pipeline_cache,
                          vk::PipelineCreateFlagBits::eFailOnPipelineCompileRequired);
        if (pipeline) {
            RecordWaveSizes();
            return;
        }
        const bool deferred_bind = scheduler.RecordsOnThread();
        // When disable_cs_opt is active and we do not defer binding to the recording thread,
        // both the immediate pipeline and the background replacement would be compiled with
        // eDisableOptimization, so skip queuing a redundant background build.
        if (!disable_cs_opt || deferred_bind) {
            auto state = std::make_shared<Build>();
            const auto caches = builder->caches;
            // With the Vulkan recording thread, the dispatch binds the pipeline there (Handle()
            // waits for it), so the GPU thread goes on and the next new pipelines are built
            // alongside: a burst costs about one build instead of their sum. NVIDIA builds one
            // without optimizations about as slowly, so the optimized one is built directly.
            // Otherwise one is built here without optimizations, several times quicker on some
            // drivers, and the optimized one replaces it (SwapInOptimized).
            const bool queued = builder->compiler->Submit(
                [state, create, caches, deferred_bind, debug_str] {
                    const auto start = std::chrono::steady_clock::now();
                    auto built = create(*caches[AsyncCompiler::WorkerIndex()], {});
                    LOG_INFO(Render_Vulkan, "tlg: compute pipeline {} built on a compiler thread in "
                             "{:.1f} ms{}", debug_str,
                             std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now() - start).count(),
                             deferred_bind ? "" : " (replaces the unoptimized one)");
                    {
                        std::scoped_lock lock{state->mutex};
                        if (deferred_bind) {
                            state->handle.store(static_cast<VkPipeline>(*built),
                                                std::memory_order_release);
                        }
                        state->result = std::move(built);
                        state->done.store(true, std::memory_order_release);
                    }
                    state->cv.notify_all();
                },
                deferred_bind);
            if (queued) {
                build = std::move(state);
                if (!deferred_bind) {
                    pipeline =
                        create(pipeline_cache, vk::PipelineCreateFlagBits::eDisableOptimization);
                    build->handle.store(static_cast<VkPipeline>(*pipeline),
                                        std::memory_order_release);
                    optimized_pending = true;
                }
                return;
            }
        }
        // The compiler queue is full (or disable_cs_opt is active without deferred_bind): built here.
    }
    pipeline = create(pipeline_cache, {});
    RecordWaveSizes();
}

ComputePipeline::~ComputePipeline() {
    // A build still queued uses the layout and module.
    if (build) {
        std::unique_lock lock{build->mutex};
        build->cv.wait(lock, [this] { return build->done.load(std::memory_order_acquire); });
    }
}

void ComputePipeline::SwapInOptimized() {
    if (!optimized_pending || !build->done.load(std::memory_order_acquire)) {
        return;
    }
    optimized_pending = false;
    vk::UniquePipeline old = std::move(pipeline);
    pipeline = std::move(build->result);
    build->handle.store(static_cast<VkPipeline>(*pipeline), std::memory_order_release);
    // Commands recorded so far, some perhaps still queued for the recording thread or the draw
    // pipe, may bind the old one: it is destroyed once the GPU is past the command buffer being
    // recorded now and then past the one recorded when that is done.
    scheduler.DeferOperation([scheduler = &scheduler, old = std::move(old)]() mutable {
        scheduler->DeferOperation([old = std::move(old)]() mutable { old.reset(); });
    });
}

} // namespace Vulkan
