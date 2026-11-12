// Adapted from Bloodborne-Android 2e472653; Android presentation infrastructure.
// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: see vk_swapchain_ahb.h.
#ifdef __ANDROID__
#include <android/hardware_buffer.h>
#include <poll.h>
#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <utility>
#include "common/assert.h"
#include "tlg_settings.h"
#include "iss_bionic_perf.h"
#include "video_core/renderer_vulkan/vk_wait_diagnostics.h"
#include "common/logging/log.h"
#include "imgui/renderer/imgui_core.h"
#include "sdl_window.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_swapchain_ahb.h"

namespace Vulkan {

extern std::atomic<u32> g_tlg_compiles;
extern std::atomic<u64> g_tlg_compile_ns;

namespace {

constexpr u32 AhbFormat = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
constexpr u64 AhbUsage = AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT |
                         AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                         AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY;
constexpr u64 AhbStagingUsage = AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN |
                                AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                                AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY;

/// ISS_PRESENT_BUFFERS: the number of buffers (3 by default: one shown, one pending, one rendered).
/// With Lossless Scaling frame generation (multiplier 2-4) a cycle holds one buffer per frame it shows
/// (the multiplier) while the app still has the last ones: multiplier + 3.
u32 BufferCount(u32 lsfg_multiplier) {
    if (const char* value = std::getenv("ISS_PRESENT_BUFFERS")) {
        const long count = std::strtol(value, nullptr, 10);
        if (count >= 2 && count <= static_cast<long>(8)) {
            return static_cast<u32>(count);
        }
    }
    if (lsfg_multiplier >= 2 && lsfg_multiplier <= 4) {
        return lsfg_multiplier + 3;
    }
    return 3;
}

/// Waits (up to timeout_ms) for a sync file to signal. True when signaled.
bool WaitSyncFile(int fd, int timeout_ms) {
    pollfd pfd{fd, POLLIN, 0};
    int ready;
    do {
        ready = poll(&pfd, 1, timeout_ms);
    } while (ready < 0 && errno == EINTR);
    return ready > 0;
}

} // namespace

u32 Swapchain::MaxImageCount() {
    return std::max(BufferCount(0), BufferCount(4)); // covers live 2x/3x/4x switches
}

Swapchain::Swapchain(const Instance& instance_, const Frontend::WindowSDL& window_, u32 lsfg_multiplier)
    : instance{instance_}, window{window_} {
    framegen_multiplier = lsfg_multiplier;
    image_count = BufferCount(lsfg_multiplier);
    use_staging = [&] {
        if (const char* env = std::getenv("ISS_AHB_STAGING")) {
            if (env[0] == '1') {
                return true;
            }
            if (env[0] == '0') {
                return false;
            }
        }
        // Mesa Turnip's Vulkan AHB import / UBWC layout can disagree with Horizon OS
        // SurfaceFlinger on Quest 3/3S (producing vertical static stripes). Render to an
        // internal device-local image and stage via linear CPU-locked AHB on non-Qualcomm drivers.
        return instance.GetDriverID() != vk::DriverId::eQualcommProprietary;
    }();
    if (!use_staging) {
        const vk::PhysicalDeviceExternalSemaphoreInfo semaphore_info{
            .handleType = vk::ExternalSemaphoreHandleTypeFlagBits::eSyncFd,
        };
        const auto semaphore_props =
            instance.GetPhysicalDevice().getExternalSemaphoreProperties(semaphore_info);
        ASSERT_MSG(
            (semaphore_props.externalSemaphoreFeatures &
             vk::ExternalSemaphoreFeatureFlagBits::eExportable) &&
                (semaphore_props.externalSemaphoreFeatures &
                 vk::ExternalSemaphoreFeatureFlagBits::eImportable),
            "The Vulkan driver cannot export and import sync-file semaphores (SYNC_FD): the AHB "
            "swapchain cannot synchronize with the app");
    } else {
        auto [pool_res, pool] = instance.GetDevice().createCommandPool({
            .flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
            .queueFamilyIndex = instance.GetGraphicsQueueFamilyIndex(),
        });
        ASSERT_MSG(pool_res == vk::Result::eSuccess,
                   "Failed to create AHB staging command pool: {}", vk::to_string(pool_res));
        staging_cmd_pool = pool;
    }
    std::printf("AHB swapchain: mode=%s (driver_id=%d)\n",
                use_staging ? "cpu-staged-linear" : "native-ahb",
                static_cast<int>(instance.GetDriverID()));
    if (IssPresent::Client::Configured()) {
        if (!client.Connect(window.GetWidth(), window.GetHeight(), AhbFormat, image_count,
                            TlgSettings::Get().vsync.load())) {
            std::printf("Present: no connection to the app: frames are rendered but not shown\n");
        }
    } else {
        std::printf("Present: ISS_PRESENT_SOCKET is not set: frames are rendered but not shown\n");
    }
    Create(window.GetWidth(), window.GetHeight());
}

// Bloodframe must not force FIFO: on a 60 Hz compositor that backpressures
// real frames to 30 FPS at 2x or 15 FPS at 4x, even with VSync disabled.
void Swapchain::UpdateVsync() {
    const bool fifo = TlgSettings::Get().vsync.load();
    if (client.Connected()) client.SetFifo(fifo);
}

Swapchain::~Swapchain() {
    Destroy();
    if (staging_cmd_pool) {
        instance.GetDevice().destroyCommandPool(staging_cmd_pool);
        staging_cmd_pool = {};
    }
    client.Disconnect();
}

std::pair<u32, u32> Swapchain::AcquireQueues() const {
    if (use_staging) {
        return {VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED};
    }
    return {VK_QUEUE_FAMILY_FOREIGN_EXT, instance.GetGraphicsQueueFamilyIndex()};
}

std::pair<u32, u32> Swapchain::ReleaseQueues() const {
    if (use_staging) {
        return {VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED};
    }
    return {instance.GetGraphicsQueueFamilyIndex(), VK_QUEUE_FAMILY_FOREIGN_EXT};
}

void Swapchain::Create(u32 width_, u32 height_) {
    width = width_;
    height = height_;
    needs_recreation = false;
    // A 0x0 output (no screen size) has no buffers; they come when the size is back.
    if (width == 0 || height == 0) {
        zero_extent = true;
        needs_recreation = true;
        return;
    }
    zero_extent = false;
    Destroy();
    extent = vk::Extent2D{width, height};
    capture_supported = true;
    ++generation;
    if (client.Connected()) {
        if (!client.BeginGeneration(generation, width, height, AhbFormat, image_count)) {
            PeerGone();
        }
    }
    const vk::Device device = instance.GetDevice();
    staging_coherent = true;
    buffers.resize(image_count);
    image_acquired.resize(image_count);
    present_ready.resize(image_count);
    for (u32 i = 0; i < image_count; ++i) {
        CreateBuffer(i);
        auto [acquired_result, acquired] = device.createSemaphore({});
        ASSERT_MSG(acquired_result == vk::Result::eSuccess, "Failed to create semaphore: {}",
                   vk::to_string(acquired_result));
        image_acquired[i] = acquired;
        if (use_staging) {
            auto [ready_result, ready] = device.createSemaphore({});
            ASSERT_MSG(ready_result == vk::Result::eSuccess,
                       "Failed to create staging present_ready semaphore: {}",
                       vk::to_string(ready_result));
            present_ready[i] = ready;
        } else {
            // The present-ready semaphore is exported as a sync file after its signal is submitted.
            const vk::ExportSemaphoreCreateInfo export_info{
                .handleTypes = vk::ExternalSemaphoreHandleTypeFlagBits::eSyncFd,
            };
            auto [ready_result, ready] = device.createSemaphore({.pNext = &export_info});
            ASSERT_MSG(ready_result == vk::Result::eSuccess, "Failed to create exportable semaphore: {}",
                       vk::to_string(ready_result));
            present_ready[i] = ready;
        }
        SetObjectName(device, image_acquired[i], "AHB swapchain image_acquired {}", i);
        SetObjectName(device, present_ready[i], "AHB swapchain present_ready {}", i);
        free_queue.push_back(i);
        if (client.Connected() &&
            !client.SendBuffer(generation, i, width, height, AhbFormat, buffers[i].ahb)) {
            PeerGone();
        }
    }
    image_index = 0;
    LOG_INFO(Render_Vulkan, "AHB swapchain: {} buffers {}x{} (generation {}, {})", image_count, width,
             height, generation, client.Connected() ? "app connected" : "no app");
}

void Swapchain::CreateBuffer(u32 index) {
    const vk::Device device = instance.GetDevice();
    Buffer& buffer = buffers[index];

    const AHardwareBuffer_Desc desc{
        .width = width,
        .height = height,
        .layers = 1,
        .format = AhbFormat,
        .usage = use_staging ? AhbStagingUsage : AhbUsage,
    };
    const int allocated = AHardwareBuffer_allocate(&desc, &buffer.ahb);
    ASSERT_MSG(allocated == 0 && buffer.ahb, "AHardwareBuffer_allocate {}x{} failed: {}", width,
               height, allocated);

    const auto memory_properties = instance.GetMemoryProperties();
    if (use_staging) {
        surface_format.format = vk::Format::eR8G8B8A8Unorm;
        const vk::ImageUsageFlags usage =
            vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst |
            vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eSampled;
        capture_supported = capture_supported && bool(usage & vk::ImageUsageFlagBits::eTransferSrc);
        const vk::ImageCreateInfo image_info{
            .imageType = vk::ImageType::e2D,
            .format = surface_format.format,
            .extent = {width, height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = usage,
            .sharingMode = vk::SharingMode::eExclusive,
            .initialLayout = vk::ImageLayout::eUndefined,
        };
        auto [image_result, image] = device.createImage(image_info);
        ASSERT_MSG(image_result == vk::Result::eSuccess, "Failed to create AHB staging image: {}",
                   vk::to_string(image_result));
        buffer.image = image;

        const auto image_reqs = device.getImageMemoryRequirements(image);
        u32 image_mem_type = ~0u;
        for (u32 i = 0; i < memory_properties.memoryTypeCount; ++i) {
            if (!(image_reqs.memoryTypeBits & (1u << i))) {
                continue;
            }
            if (image_mem_type == ~0u) {
                image_mem_type = i;
            }
            if (memory_properties.memoryTypes[i].propertyFlags & vk::MemoryPropertyFlagBits::eDeviceLocal) {
                image_mem_type = i;
                break;
            }
        }
        ASSERT_MSG(image_mem_type != ~0u, "No memory type for staging image (bits {:#x})",
                   image_reqs.memoryTypeBits);
        auto [mem_res, memory] = device.allocateMemory({
            .allocationSize = image_reqs.size,
            .memoryTypeIndex = image_mem_type,
        });
        ASSERT_MSG(mem_res == vk::Result::eSuccess, "Failed to allocate staging image memory: {}",
                   vk::to_string(mem_res));
        buffer.memory = memory;
        const auto bind_res = device.bindImageMemory(image, memory, 0);
        ASSERT_MSG(bind_res == vk::Result::eSuccess, "Failed to bind staging image: {}",
                   vk::to_string(bind_res));

        auto [view_result, view] = device.createImageView({
            .image = image,
            .viewType = vk::ImageViewType::e2D,
            .format = surface_format.format,
            .subresourceRange = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                 .levelCount = 1,
                                 .layerCount = 1},
        });
        ASSERT_MSG(view_result == vk::Result::eSuccess, "Failed to create staging image view: {}",
                   vk::to_string(view_result));
        buffer.view = view;
        SetObjectName(device, image, "AHB swapchain image {}", index);
        SetObjectName(device, view, "AHB swapchain view {}", index);

        const vk::DeviceSize staging_size = static_cast<vk::DeviceSize>(width) * height * 4u;
        auto [buf_res, staging_buf] = device.createBuffer({
            .size = staging_size,
            .usage = vk::BufferUsageFlagBits::eTransferDst,
            .sharingMode = vk::SharingMode::eExclusive,
        });
        ASSERT_MSG(buf_res == vk::Result::eSuccess, "Failed to create AHB staging buffer: {}",
                   vk::to_string(buf_res));
        buffer.staging_buffer = staging_buf;

        const auto buf_reqs = device.getBufferMemoryRequirements(staging_buf);
        u32 staging_mem_type = ~0u;
        bool coherent = false;
        for (u32 i = 0; i < memory_properties.memoryTypeCount; ++i) {
            if (!(buf_reqs.memoryTypeBits & (1u << i))) {
                continue;
            }
            const auto flags = memory_properties.memoryTypes[i].propertyFlags;
            if (!(flags & vk::MemoryPropertyFlagBits::eHostVisible)) {
                continue;
            }
            if (staging_mem_type == ~0u) {
                staging_mem_type = i;
                coherent = bool(flags & vk::MemoryPropertyFlagBits::eHostCoherent);
            }
            if (flags & vk::MemoryPropertyFlagBits::eHostCoherent) {
                staging_mem_type = i;
                coherent = true;
                break;
            }
        }
        ASSERT_MSG(staging_mem_type != ~0u, "No host-visible memory type for AHB staging buffer (bits {:#x})",
                   buf_reqs.memoryTypeBits);
        staging_coherent = staging_coherent && coherent;

        auto [s_mem_res, s_mem] = device.allocateMemory({
            .allocationSize = buf_reqs.size,
            .memoryTypeIndex = staging_mem_type,
        });
        ASSERT_MSG(s_mem_res == vk::Result::eSuccess, "Failed to allocate AHB staging buffer memory: {}",
                   vk::to_string(s_mem_res));
        buffer.staging_memory = s_mem;
        const auto s_bind_res = device.bindBufferMemory(staging_buf, s_mem, 0);
        ASSERT_MSG(s_bind_res == vk::Result::eSuccess, "Failed to bind AHB staging buffer: {}",
                   vk::to_string(s_bind_res));

        auto [map_res, mapped] = device.mapMemory(s_mem, 0, VK_WHOLE_SIZE);
        ASSERT_MSG(map_res == vk::Result::eSuccess && mapped, "Failed to map AHB staging buffer: {}",
                   vk::to_string(map_res));
        buffer.staging_mapped = mapped;

        const vk::CommandBufferAllocateInfo cmd_alloc{
            .commandPool = staging_cmd_pool,
            .level = vk::CommandBufferLevel::ePrimary,
            .commandBufferCount = 1,
        };
        const auto cmd_res = device.allocateCommandBuffers(&cmd_alloc, &buffer.copy_cmd);
        ASSERT_MSG(cmd_res == vk::Result::eSuccess, "Failed to allocate AHB copy command buffer: {}",
                   vk::to_string(cmd_res));

        auto [fence_res, fence] = device.createFence({});
        ASSERT_MSG(fence_res == vk::Result::eSuccess, "Failed to create AHB copy fence: {}",
                   vk::to_string(fence_res));
        buffer.copy_fence = fence;
        return;
    }

    auto [props_result, props_chain] = device.getAndroidHardwareBufferPropertiesANDROID<
        vk::AndroidHardwareBufferPropertiesANDROID, vk::AndroidHardwareBufferFormatPropertiesANDROID>(
        *buffer.ahb);
    ASSERT_MSG(props_result == vk::Result::eSuccess,
               "vkGetAndroidHardwareBufferPropertiesANDROID failed: {}", vk::to_string(props_result));
    const auto& props = props_chain.get<vk::AndroidHardwareBufferPropertiesANDROID>();
    const auto& format_props = props_chain.get<vk::AndroidHardwareBufferFormatPropertiesANDROID>();
    ASSERT_MSG(format_props.format != vk::Format::eUndefined,
               "The driver maps the AHardwareBuffer to no Vulkan format (external format {:#x})",
               format_props.externalFormat);
    const auto required = vk::FormatFeatureFlagBits::eColorAttachment | vk::FormatFeatureFlagBits::eBlitDst;
    ASSERT_MSG((format_props.formatFeatures & required) == required,
               "The driver cannot render/blit into the AHardwareBuffer format {} (features {})",
               vk::to_string(format_props.format), vk::to_string(format_props.formatFeatures));
    surface_format.format = format_props.format;

    vk::ImageUsageFlags usage =
        vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst;
    if (format_props.formatFeatures & vk::FormatFeatureFlagBits::eTransferSrc) {
        usage |= vk::ImageUsageFlagBits::eTransferSrc;
    }
    if (format_props.formatFeatures & vk::FormatFeatureFlagBits::eSampledImage) {
        usage |= vk::ImageUsageFlagBits::eSampled;
    }
    capture_supported = capture_supported && bool(usage & vk::ImageUsageFlagBits::eTransferSrc);
    const vk::ExternalMemoryImageCreateInfo external_info{
        .handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eAndroidHardwareBufferANDROID,
    };
    const vk::ImageCreateInfo image_info{
        .pNext = &external_info,
        .imageType = vk::ImageType::e2D,
        .format = format_props.format,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = usage,
        .sharingMode = vk::SharingMode::eExclusive,
        .initialLayout = vk::ImageLayout::eUndefined,
    };
    auto [image_result, image] = device.createImage(image_info);
    ASSERT_MSG(image_result == vk::Result::eSuccess, "Failed to create the AHB image: {}",
               vk::to_string(image_result));
    buffer.image = image;

    u32 memory_type = ~0u;
    for (u32 i = 0; i < memory_properties.memoryTypeCount; ++i) {
        if (!(props.memoryTypeBits & (1u << i))) {
            continue;
        }
        if (memory_type == ~0u) {
            memory_type = i;
        }
        if (memory_properties.memoryTypes[i].propertyFlags & vk::MemoryPropertyFlagBits::eDeviceLocal) {
            memory_type = i;
            break;
        }
    }
    ASSERT_MSG(memory_type != ~0u, "No memory type for the AHardwareBuffer (bits {:#x})",
               props.memoryTypeBits);
    const vk::MemoryDedicatedAllocateInfo dedicated{.image = image};
    const vk::ImportAndroidHardwareBufferInfoANDROID import_info{
        .pNext = &dedicated,
        .buffer = buffer.ahb,
    };
    auto [memory_result, memory] = device.allocateMemory({
        .pNext = &import_info,
        .allocationSize = props.allocationSize,
        .memoryTypeIndex = memory_type,
    });
    ASSERT_MSG(memory_result == vk::Result::eSuccess, "Failed to import the AHardwareBuffer: {}",
               vk::to_string(memory_result));
    buffer.memory = memory;
    const auto bind_result = device.bindImageMemory(image, memory, 0);
    ASSERT_MSG(bind_result == vk::Result::eSuccess, "Failed to bind the AHB image: {}",
               vk::to_string(bind_result));

    auto [view_result, view] = device.createImageView({
        .image = image,
        .viewType = vk::ImageViewType::e2D,
        .format = format_props.format,
        .subresourceRange = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                             .levelCount = 1,
                             .layerCount = 1},
    });
    ASSERT_MSG(view_result == vk::Result::eSuccess, "Failed to create the AHB image view: {}",
               vk::to_string(view_result));
    buffer.view = view;
    SetObjectName(device, image, "AHB swapchain image {}", index);
    SetObjectName(device, view, "AHB swapchain view {}", index);
}

void Swapchain::SetFrameGeneration(u32 lsfg_multiplier) {
    framegen_multiplier = lsfg_multiplier;
    if (client.Connected() &&
        !client.SetFifo(TlgSettings::Get().vsync.load())) {
        PeerGone();
    }
    const u32 count = BufferCount(lsfg_multiplier);
    if (count != image_count) {
        image_count = count;
        Recreate(width != 0 ? width : window.GetWidth(), height != 0 ? height : window.GetHeight());
    }
}

void Swapchain::Recreate(u32 width_, u32 height_) {
    LOG_DEBUG(Render_Vulkan, "Recreate the AHB swapchain: width={} height={}", width_, height_);
    Create(width_, height_);
}

void Swapchain::Destroy() {
    vk::Device device = instance.GetDevice();
    const auto wait_result = device.waitIdle();
    if (wait_result != vk::Result::eSuccess) {
        LOG_WARNING(Render_Vulkan, "Failed to wait for device to become idle: {}",
                    vk::to_string(wait_result));
    }
    for (Buffer& buffer : buffers) {
        if (buffer.copy_fence) {
            device.destroyFence(buffer.copy_fence);
        }
        if (staging_cmd_pool && buffer.copy_cmd) {
            device.freeCommandBuffers(staging_cmd_pool, 1, &buffer.copy_cmd);
        }
        if (buffer.staging_mapped && buffer.staging_memory) {
            device.unmapMemory(buffer.staging_memory);
            buffer.staging_mapped = nullptr;
        }
        if (buffer.staging_buffer) {
            device.destroyBuffer(buffer.staging_buffer);
        }
        if (buffer.staging_memory) {
            device.freeMemory(buffer.staging_memory);
        }
        if (buffer.view) {
            device.destroyImageView(buffer.view);
        }
        if (buffer.image) {
            device.destroyImage(buffer.image);
        }
        if (buffer.memory) {
            device.freeMemory(buffer.memory);
        }
        // The app and the compositor keep the memory while they use it (reference counted).
        if (buffer.ahb) {
            AHardwareBuffer_release(buffer.ahb);
        }
        if (buffer.release_fd >= 0) {
            close(buffer.release_fd);
        }
    }
    buffers.clear();
    for (const auto sem : image_acquired) {
        device.destroySemaphore(sem);
    }
    for (const auto sem : present_ready) {
        device.destroySemaphore(sem);
    }
    image_acquired.clear();
    present_ready.clear();
    free_queue.clear();
}

void Swapchain::PeerGone() const {
    // The app is the game's screen: without it (closed, killed) the process ends, as when the
    // window of the glibc build is closed.
    LOG_INFO(Frontend, "The app closed the frame connection");
    std::fflush(stdout);
    std::_Exit(0);
}

void Swapchain::HandleRelease(IssPresent::Release& release) {
    if (release.generation != generation || release.index >= buffers.size()) {
        if (release.fence_fd >= 0) {
            close(release.fence_fd); // a buffer of a set that is gone
        }
        return;
    }
    Buffer& buffer = buffers[release.index];
    if (buffer.release_fd >= 0) {
        close(buffer.release_fd);
    }
    buffer.release_fd = release.fence_fd;
    free_queue.push_back(release.index);
}

bool Swapchain::AcquireNextImage() {
    const IssBionicPerf::Scope timer{IssBionicPerf::Stage::AhbAcquire};
    if (zero_extent || buffers.empty()) {
        needs_recreation = true;
        return false;
    }
    IssPresent::Release release;
    // Releases that arrived meanwhile.
    while (client.Connected()) {
        const int got = client.WaitRelease(release, 0);
        if (got < 0) {
            PeerGone();
        }
        if (got == 0) {
            break;
        }
        HandleRelease(release);
    }
    // All buffers are at the app: the display is slower than the game. Wait for one.
    const auto wait_start = std::chrono::steady_clock::now();
    bool warned = false;
    while (free_queue.empty()) {
        const int got = client.WaitRelease(release, 1000);
        if (got < 0) {
            PeerGone();
        }
        if (got > 0) {
            HandleRelease(release);
        } else if (!warned && std::chrono::steady_clock::now() - wait_start > std::chrono::seconds(5)) {
            warned = true;
            LOG_WARNING(Render_Vulkan, "AHB swapchain: no buffer released by the app for 5 s");
        }
    }
    image_index = free_queue.front();
    free_queue.pop_front();
    ImportAcquire(image_index);
    return !needs_recreation;
}

void Swapchain::ImportAcquire(u32 index) {
    const IssBionicPerf::Scope timer{IssBionicPerf::Stage::AhbImport};
    Buffer& buffer = buffers[index];
    buffer.acquire_imported = false;
    const int fd = buffer.release_fd;
    if (fd < 0) {
        return; // never shown, or released at once: nothing to wait for
    }
    buffer.release_fd = -1;
    if (use_staging) {
        while (!WaitSyncFile(fd, 1000)) {
            LOG_WARNING(Render_Vulkan, "AHB swapchain: release fence of buffer {} still pending", index);
        }
        close(fd);
        return;
    }
    if (sync_import_ok) {
        const vk::ImportSemaphoreFdInfoKHR import_info{
            .semaphore = image_acquired[index],
            .flags = vk::SemaphoreImportFlagBits::eTemporary,
            .handleType = vk::ExternalSemaphoreHandleTypeFlagBits::eSyncFd,
            .fd = fd,
        };
        const auto result = instance.GetDevice().importSemaphoreFdKHR(import_info);
        if (result == vk::Result::eSuccess) {
            buffer.acquire_imported = true; // the driver owns the fd now
            return;
        }
        LOG_WARNING(Render_Vulkan,
                    "Importing the release fence failed ({}): waiting for fences on the CPU instead",
                    vk::to_string(result));
        sync_import_ok = false;
    }
    // CPU wait: the fence signals when the display is done with the buffer.
    while (!WaitSyncFile(fd, 1000)) {
        LOG_WARNING(Render_Vulkan, "AHB swapchain: release fence of buffer {} still pending", index);
    }
    close(fd);
}

int Swapchain::ExportPresentReady() {
    const IssBionicPerf::Scope timer{IssBionicPerf::Stage::AhbExport};
    const auto [result, fd] = instance.GetDevice().getSemaphoreFdKHR({
        .semaphore = present_ready[image_index],
        .handleType = vk::ExternalSemaphoreHandleTypeFlagBits::eSyncFd,
    });
    if (result != vk::Result::eSuccess) {
        // Without the fence the app would read a half-drawn buffer: wait for the GPU here.
        static bool logged = false;
        if (!logged) {
            logged = true;
            LOG_ERROR(Render_Vulkan,
                      "Exporting the present-ready semaphore as a sync file failed ({}): falling "
                      "back to waiting for the GPU on every frame",
                      vk::to_string(result));
        }
        const auto idle = instance.GetDevice().waitIdle();
        ASSERT_MSG(idle == vk::Result::eSuccess, "waitIdle failed: {}", vk::to_string(idle));
        return -1;
    }
    return fd; // -1 is possible too: the semaphore was already signaled
}

void Swapchain::StageAndCopyToAhb(u32 index) {
    const IssBionicPerf::Scope timer{IssBionicPerf::Stage::AhbExport};
    const vk::Device device = instance.GetDevice();
    Buffer& buffer = buffers[index];

    const auto reset_cmd_res = buffer.copy_cmd.reset();
    ASSERT_MSG(reset_cmd_res == vk::Result::eSuccess, "Failed to reset AHB copy command buffer: {}",
               vk::to_string(reset_cmd_res));
    const auto begin_res = buffer.copy_cmd.begin(vk::CommandBufferBeginInfo{
        .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
    });
    ASSERT_MSG(begin_res == vk::Result::eSuccess, "Failed to begin AHB copy command buffer: {}",
               vk::to_string(begin_res));

    const vk::ImageSubresourceRange color_range{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };
    const vk::ImageMemoryBarrier pre_barrier{
        .srcAccessMask = {},
        .dstAccessMask = vk::AccessFlagBits::eTransferRead,
        .oldLayout = PresentLayout(),
        .newLayout = vk::ImageLayout::eTransferSrcOptimal,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = buffer.image,
        .subresourceRange = color_range,
    };
    buffer.copy_cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                                    vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, pre_barrier);

    const vk::BufferImageCopy region{
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .imageOffset = {0, 0, 0},
        .imageExtent = {width, height, 1},
    };
    buffer.copy_cmd.copyImageToBuffer(buffer.image, vk::ImageLayout::eTransferSrcOptimal,
                                      buffer.staging_buffer, region);

    const vk::BufferMemoryBarrier buf_barrier{
        .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
        .dstAccessMask = vk::AccessFlagBits::eHostRead,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = buffer.staging_buffer,
        .offset = 0,
        .size = VK_WHOLE_SIZE,
    };
    const vk::ImageMemoryBarrier post_barrier{
        .srcAccessMask = vk::AccessFlagBits::eTransferRead,
        .dstAccessMask = {},
        .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
        .newLayout = PresentLayout(),
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = buffer.image,
        .subresourceRange = color_range,
    };
    buffer.copy_cmd.pipelineBarrier(
        vk::PipelineStageFlagBits::eTransfer,
        vk::PipelineStageFlagBits::eTransfer | vk::PipelineStageFlagBits::eHost, {}, {},
        buf_barrier, post_barrier);

    const auto end_res = buffer.copy_cmd.end();
    ASSERT_MSG(end_res == vk::Result::eSuccess, "Failed to end AHB copy command buffer: {}",
               vk::to_string(end_res));

    const auto reset_fence_res = device.resetFences(buffer.copy_fence);
    ASSERT_MSG(reset_fence_res == vk::Result::eSuccess, "Failed to reset AHB copy fence: {}",
               vk::to_string(reset_fence_res));

    const vk::PipelineStageFlags wait_stage = vk::PipelineStageFlagBits::eTransfer;
    const vk::SubmitInfo submit_info{
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &present_ready[index],
        .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 1,
        .pCommandBuffers = &buffer.copy_cmd,
    };
    const auto submit_res = instance.GetGraphicsQueue().submit(submit_info, buffer.copy_fence);
    ASSERT_MSG(submit_res == vk::Result::eSuccess, "Failed to submit AHB staging copy: {}",
               vk::to_string(submit_res));

    vk::Result wait_res{};
    while ((wait_res = device.waitForFences(buffer.copy_fence, true,
                                            std::numeric_limits<u64>::max())) !=
           vk::Result::eSuccess) {
        ASSERT_MSG(wait_res == vk::Result::eTimeout, "AHB staging fence wait failed: {}",
                   vk::to_string(wait_res));
    }

    if (!staging_coherent) {
        const vk::MappedMemoryRange range{
            .memory = buffer.staging_memory,
            .offset = 0,
            .size = VK_WHOLE_SIZE,
        };
        const auto inv_res = device.invalidateMappedMemoryRanges(range);
        ASSERT_MSG(inv_res == vk::Result::eSuccess,
                   "AHB staging invalidateMappedMemoryRanges failed: {}", vk::to_string(inv_res));
    }

    AHardwareBuffer_Desc ahb_desc{};
    AHardwareBuffer_describe(buffer.ahb, &ahb_desc);
    void* ahb_ptr = nullptr;
    const int lock_res = AHardwareBuffer_lock(
        buffer.ahb, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, nullptr, &ahb_ptr);
    if (lock_res == 0 && ahb_ptr) {
        const u8* src = static_cast<const u8*>(buffer.staging_mapped);
        u8* dst = static_cast<u8*>(ahb_ptr);
        const size_t row_bytes = static_cast<size_t>(width) * 4u;
        const u32 dst_stride_pixels = ahb_desc.stride >= width ? ahb_desc.stride : width;
        const size_t dst_stride_bytes = static_cast<size_t>(dst_stride_pixels) * 4u;
        if (dst_stride_bytes == row_bytes) {
            std::memcpy(dst, src, row_bytes * height);
        } else {
            for (u32 y = 0; y < height; ++y) {
                std::memcpy(dst + static_cast<size_t>(y) * dst_stride_bytes,
                            src + static_cast<size_t>(y) * row_bytes, row_bytes);
            }
        }
        const int unlock_res = AHardwareBuffer_unlock(buffer.ahb, nullptr);
        if (unlock_res != 0) {
            LOG_WARNING(Render_Vulkan, "AHardwareBuffer_unlock failed: {}", unlock_res);
        }
    } else {
        LOG_ERROR(Render_Vulkan, "AHardwareBuffer_lock failed: {}", lock_res);
    }
}

bool Swapchain::Present() {
    if (use_staging && !buffers.empty()) {
        StageAndCopyToAhb(image_index);
    }
    const int fence_fd = use_staging ? -1 : ExportPresentReady();
    ++frame_counter;
    const int trace_index = std::exchange(capture_index, -1);
    if (trace_index >= 0) {
        const char* dir = std::getenv("TLG_DUMP_DIR"); char path[512];
        std::snprintf(path, sizeof(path), "%s/f%03d_presenter_info.txt", dir && *dir ? dir : "out/dump", trace_index);
        if (FILE* f = std::fopen(path, "wb")) {
            const auto now = std::chrono::steady_clock::now().time_since_epoch();
            std::fprintf(f, "serial=%llu\nframe=%llu\ngeneration=%u\nbuffer=%u\nsteady_ns=%lld\nahb_extent=%ux%u\nahb_crop=%u,%u,%u,%u\nsource_frame_extent=%ux%u\nahb_status=%s\n",
                (unsigned long long)capture_serial, (unsigned long long)frame_counter, generation, image_index,
                (long long)std::chrono::duration_cast<std::chrono::nanoseconds>(now).count(), width, height,
                capture_crop[0], capture_crop[1], capture_crop[2], capture_crop[3], capture_frame_size[0], capture_frame_size[1], capture_status);
            std::fclose(f);
        }
    }
    if (client.Connected()) {
        const IssBionicPerf::Scope send_timer{IssBionicPerf::Stage::AhbSend};
        if (!client.SendFrame(generation, image_index, frame_counter, width, height, fence_fd, trace_index)) {
            PeerGone();
        }
        // The buffer is the app's until its release.
    } else {
        // Nobody shows it: the sync file is dropped, the buffer is free at once.
        free_queue.push_back(image_index);
        buffers[image_index].release_fd = -1;
    }
    if (fence_fd >= 0) {
        close(fence_fd);
    }
    buffers[image_index].acquire_imported = false;
    if (IssBionicPerf::OnPresent(g_tlg_compiles.load(std::memory_order_relaxed),
                                g_tlg_compile_ns.load(std::memory_order_relaxed))) {
        WaitDiagnostics::Report(true);
    }
    return !needs_recreation;
}

} // namespace Vulkan
#endif // __ANDROID__
