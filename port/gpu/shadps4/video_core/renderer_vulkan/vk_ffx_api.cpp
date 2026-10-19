// SPDX-FileCopyrightText: Copyright 2026 tlg contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include "video_core/renderer_vulkan/vk_ffx_api.h"
#include "video_core/renderer_vulkan/vk_instance.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "../../../third_party/ffx-api/ffx_api/ffx_api.h"
#include "../../../third_party/ffx-api/ffx_api/ffx_upscale.h"
#include "../../../third_party/ffx-api/ffx_api/ffx_framegeneration.h"
#include "../../../third_party/ffx-api/ffx_api/vk/ffx_api_vk.h"
#endif

namespace Vulkan {

#ifdef _WIN32

namespace {
constexpr wchar_t DllName[] = L"amd_fidelityfx_vk.dll";

std::filesystem::path ExecutableDirectory() {
    std::wstring path(MAX_PATH, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
    path.resize(length);
    return std::filesystem::path{path}.parent_path();
}

void Message(uint32_t type, const wchar_t* message) {
    std::printf("FFX API: %s%ls\n", type == FFX_API_MESSAGE_TYPE_WARNING ? "warning: " : "",
                message ? message : L"");
}

FfxApiResource Describe(const Dlss::Resource& r, uint32_t usage) {
    FfxApiResourceDescription desc{};
    desc.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    desc.format = ffxApiGetSurfaceFormatVK(VkFormat(r.format));
    desc.width = r.width;
    desc.height = r.height;
    desc.depth = 1;
    desc.mipCount = 1;
    desc.flags = FFX_API_RESOURCE_FLAGS_NONE;
    desc.usage = usage;
    // The port keeps these images in GENERAL; FFX returns them to it after the dispatch.
    return ffxApiGetResourceVK(reinterpret_cast<void*>(VkImage(r.image)), desc,
                               FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
}
} // namespace

struct FfxApi::Impl {
    HMODULE module = nullptr;
    PfnFfxCreateContext create = nullptr;
    PfnFfxDestroyContext destroy = nullptr;
    PfnFfxDispatch dispatch = nullptr;
    PfnFfxConfigure configure = nullptr;
    PfnFfxQuery query = nullptr;
    ffxContext context = nullptr;
    ContextDesc desc{};
    // Kept alive for the context's lifetime, as the API asks.
    ffxCreateBackendVKDesc backend{};
    ffxCreateContextDescUpscale upscale{};
};

FfxApi::FfxApi() : impl{std::make_unique<Impl>()} {}

FfxApi::~FfxApi() {
    ReleaseContext();
}

FfxApi* FfxApi::Get() {
    static FfxApi* const ffx = []() -> FfxApi* {
        const char* setting = std::getenv("TLG_FFX_DLL");
        if (setting && setting[0] == '0') {
            return nullptr;
        }
        const auto path = ExecutableDirectory() / DllName;
        if (!std::filesystem::exists(path)) {
            return nullptr;
        }
        // By name, so OptiScaler (already in the process) sees the load it hooks.
        HMODULE module = LoadLibraryW(DllName);
        if (!module) {
            module = LoadLibraryW(path.c_str());
        }
        if (!module) {
            std::printf("FFX API: %ls failed to load (error %lu)\n", DllName, GetLastError());
            return nullptr;
        }
        auto* api = new FfxApi();
        api->impl->module = module;
        api->impl->create =
            reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(module, "ffxCreateContext"));
        api->impl->destroy =
            reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(module, "ffxDestroyContext"));
        api->impl->dispatch =
            reinterpret_cast<PfnFfxDispatch>(GetProcAddress(module, "ffxDispatch"));
        api->impl->configure =
            reinterpret_cast<PfnFfxConfigure>(GetProcAddress(module, "ffxConfigure"));
        api->impl->query = reinterpret_cast<PfnFfxQuery>(GetProcAddress(module, "ffxQuery"));
        if (!api->impl->create || !api->impl->destroy || !api->impl->dispatch) {
            std::printf("FFX API: %ls has no ffxCreateContext/ffxDispatch\n", DllName);
            return nullptr;
        }
        std::printf("FFX API: %ls loaded; FSR 3.1 goes through it (OptiScaler hooks here)\n",
                    DllName);
        return api;
    }();
    return ffx;
}

bool FfxApi::HasContext(const ContextDesc& desc) const {
    return impl->context && impl->desc == desc;
}

bool FfxApi::CreateContext(vk::PhysicalDevice physical, vk::Device device,
                           const ContextDesc& desc) {
    ReleaseContext();
    auto& backend = impl->backend;
    backend = {};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    backend.vkDevice = VkDevice(device);
    backend.vkPhysicalDevice = VkPhysicalDevice(physical);
    backend.vkDeviceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr;
    auto& upscale = impl->upscale;
    upscale = {};
    upscale.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    upscale.header.pNext = &backend.header;
    // Same as the built-in FSR 3: HDR scene color with auto exposure, or the tonemapped frame.
    upscale.flags = desc.hdr ? FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE
                             : 0;
    upscale.maxRenderSize = {desc.render_width, desc.render_height};
    upscale.maxUpscaleSize = {desc.output_width, desc.output_height};
    upscale.fpMessage = Message;
    const ffxReturnCode_t result = impl->create(&impl->context, &upscale.header, nullptr);
    if (result != FFX_API_RETURN_OK) {
        std::printf("FFX API: context creation failed (%u)\n", result);
        impl->context = nullptr;
        return false;
    }
    impl->desc = desc;
    std::printf("FFX API: context %ux%u -> %ux%u%s\n", desc.render_width, desc.render_height,
                desc.output_width, desc.output_height, desc.hdr ? " (HDR)" : "");
    return true;
}

bool FfxApi::Dispatch(vk::CommandBuffer command, const Dlss::Frame& frame, const Camera& camera) {
    if (!impl->context) {
        return false;
    }
    ffxDispatchDescUpscale d{};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList = reinterpret_cast<void*>(VkCommandBuffer(command));
    d.color = Describe(frame.color, FFX_API_RESOURCE_USAGE_READ_ONLY);
    d.depth = Describe(frame.depth, FFX_API_RESOURCE_USAGE_DEPTHTARGET);
    d.motionVectors = Describe(frame.motion, FFX_API_RESOURCE_USAGE_READ_ONLY);
    d.output = Describe(frame.output, FFX_API_RESOURCE_USAGE_UAV);
    d.jitterOffset = {frame.jitter_x, frame.jitter_y};
    d.motionVectorScale = {1.0f, 1.0f};
    d.renderSize = {frame.color.width, frame.color.height};
    d.upscaleSize = {frame.output.width, frame.output.height};
    d.enableSharpening = frame.sharpness > 0.0f;
    d.sharpness = frame.sharpness;
    d.frameTimeDelta = frame.frame_ms;
    d.preExposure = 1.0f;
    d.reset = frame.reset;
    d.cameraNear = camera.near_plane;
    d.cameraFar = camera.far_plane;
    d.cameraFovAngleVertical = camera.vertical_fov;
    d.viewSpaceToMetersFactor = 1.0f;
    d.flags = 0;
    const ffxReturnCode_t result = impl->dispatch(&impl->context, &d.header);
    if (result != FFX_API_RETURN_OK) {
        std::printf("FFX API: dispatch failed (%u)\n", result);
        return false;
    }
    return true;
}

void FfxApi::ReleaseContext() {
    if (impl->context && impl->destroy) {
        impl->destroy(&impl->context, nullptr);
    }
    impl->context = nullptr;
}

// -- Frame generation ------------------------------------------------------------------------------

struct FrameGen::Impl {
    FfxApi::Impl* api = nullptr;
    std::mutex mutex;
    ffxContext swapchain_context = nullptr;
    ffxContext context = nullptr;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    ffxQueryDescSwapchainReplacementFunctionsVK functions{};
    vk::Format format{};
    vk::Extent2D extent{};
    uint64_t frame_id = 0;
    bool prepared = false; ///< a Prepare since the last present
    ffxCreateBackendVKDesc backend{};
    ffxCreateContextDescFrameGeneration create{};

    void Configure(bool enabled) {
        ffxConfigureDescFrameGeneration config{};
        config.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
        config.swapChain = swapchain;
        config.frameGenerationCallback = [](ffxDispatchDescFrameGeneration* params,
                                            void* user) -> ffxReturnCode_t {
            auto* self = static_cast<Impl*>(user);
            return self->api->dispatch(&self->context, &params->header);
        };
        config.frameGenerationCallbackUserContext = this;
        config.frameGenerationEnabled = enabled;
        config.allowAsyncWorkloads = false;
        config.generationRect = {0, 0, int32_t(extent.width), int32_t(extent.height)};
        config.frameID = frame_id;
        const ffxReturnCode_t result = api->configure(&context, &config.header);
        static bool logged = false;
        if (result != FFX_API_RETURN_OK && !logged) {
            std::printf("Frame generation: configure failed (%u)\n", result);
            logged = true;
        }
    }
};

FrameGen* FrameGen::Get() {
    static FrameGen* const fg = []() -> FrameGen* {
        const char* setting = std::getenv("TLG_FRAMEGEN");
        if (!setting || setting[0] != '1') {
            return nullptr;
        }
        FfxApi* api = FfxApi::Get();
        if (!api || !api->impl->configure || !api->impl->query) {
            std::printf("Frame generation: needs amd_fidelityfx_vk.dll beside the game; off\n");
            return nullptr;
        }
        auto* frame_gen = new FrameGen();
        frame_gen->impl = std::make_unique<Impl>();
        frame_gen->impl->api = api->impl.get();
        return frame_gen;
    }();
    return fg;
}

FrameGen::~FrameGen() {
    DestroySwapchain();
}

bool FrameGen::Active() const {
    return impl->swapchain_context != nullptr;
}

bool FrameGen::CreateSwapchain(const Instance& instance, vk::SurfaceKHR surface,
                               const vk::SwapchainCreateInfoKHR& info,
                               vk::SwapchainKHR* swapchain) {
    std::scoped_lock lock{impl->mutex};
    auto& d = *impl;
    const auto physical = instance.GetPhysicalDevice();
    // Queues of their own: present (transfer + present) and image acquire (anything).
    const auto& extra = instance.GetExtraQueues();
    const Instance::ExtraQueue* present = nullptr;
    const Instance::ExtraQueue* acquire = nullptr;
    for (const auto& q : extra) {
        const auto [result, supported] = physical.getSurfaceSupportKHR(q.family, surface);
        if (!present && result == vk::Result::eSuccess && supported) {
            present = &q;
        } else if (!acquire) {
            acquire = &q;
        }
    }
    if (!present || !acquire) {
        std::printf("Frame generation: no spare present/acquire queues (%zu extra); off\n",
                    extra.size());
        return false;
    }
    ffxCreateContextDescFrameGenerationSwapChainVK desc{};
    desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_VK;
    desc.physicalDevice = VkPhysicalDevice(physical);
    desc.device = VkDevice(instance.GetDevice());
    d.swapchain = VK_NULL_HANDLE;
    desc.swapchain = &d.swapchain;
    desc.createInfo = VkSwapchainCreateInfoKHR(info);
    desc.gameQueue = {VkQueue(instance.GetGraphicsQueue()), instance.GetGraphicsQueueFamilyIndex(),
                      nullptr};
    desc.asyncComputeQueue = {VkQueue(present->queue), present->family, nullptr};
    desc.presentQueue = {VkQueue(present->queue), present->family, nullptr};
    desc.imageAcquireQueue = {VkQueue(acquire->queue), acquire->family, nullptr};
    ffxReturnCode_t result = d.api->create(&d.swapchain_context, &desc.header, nullptr);
    if (result != FFX_API_RETURN_OK) {
        std::printf("Frame generation: swapchain creation failed (%u); off\n", result);
        d.swapchain_context = nullptr;
        return false;
    }
    d.functions = {};
    d.functions.header.type = FFX_API_QUERY_DESC_TYPE_FGSWAPCHAIN_FUNCTIONS_VK;
    d.api->query(&d.swapchain_context, &d.functions.header);
    d.backend = {};
    d.backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    d.backend.vkDevice = VkDevice(instance.GetDevice());
    d.backend.vkPhysicalDevice = VkPhysicalDevice(physical);
    d.backend.vkDeviceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr;
    d.create = {};
    d.create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    d.create.header.pNext = &d.backend.header;
    d.create.displaySize = {info.imageExtent.width, info.imageExtent.height};
    d.create.maxRenderSize = {std::max(info.imageExtent.width, 3840u),
                              std::max(info.imageExtent.height, 2160u)};
    d.create.backBufferFormat = ffxApiGetSurfaceFormatVK(VkFormat(info.imageFormat));
    result = d.api->create(&d.context, &d.create.header, nullptr);
    if (result != FFX_API_RETURN_OK || !d.functions.pOutAcquireNextImageKHR) {
        std::printf("Frame generation: context creation failed (%u); off\n", result);
        d.context = nullptr;
        d.api->destroy(&d.swapchain_context, nullptr);
        d.swapchain_context = nullptr;
        return false;
    }
    d.format = info.imageFormat;
    d.extent = info.imageExtent;
    d.frame_id = 0;
    d.prepared = false;
    *swapchain = vk::SwapchainKHR(d.swapchain);
    std::printf("Frame generation: on, %ux%u (present queue family %u, acquire family %u)\n",
                info.imageExtent.width, info.imageExtent.height, present->family, acquire->family);
    return true;
}

void FrameGen::DestroySwapchain() {
    std::scoped_lock lock{impl->mutex};
    auto& d = *impl;
    if (d.context) {
        d.api->destroy(&d.context, nullptr);
        d.context = nullptr;
    }
    if (d.swapchain_context) {
        d.api->destroy(&d.swapchain_context, nullptr); // destroys the swapchain too
        d.swapchain_context = nullptr;
    }
    d.swapchain = VK_NULL_HANDLE;
}

vk::Result FrameGen::GetImages(vk::Device device, vk::SwapchainKHR swapchain,
                               std::vector<vk::Image>& images) {
    u32 count = 0;
    auto fn = impl->functions.pOutGetSwapchainImagesKHR;
    VkResult result = fn(VkDevice(device), VkSwapchainKHR(swapchain), &count, nullptr);
    std::vector<VkImage> raw(count);
    result = fn(VkDevice(device), VkSwapchainKHR(swapchain), &count, raw.data());
    images.assign(raw.begin(), raw.begin() + count);
    return vk::Result(result);
}

vk::Result FrameGen::Acquire(vk::Device device, vk::SwapchainKHR swapchain, vk::Semaphore semaphore,
                             u32* index) {
    return vk::Result(impl->functions.pOutAcquireNextImageKHR(
        VkDevice(device), VkSwapchainKHR(swapchain), UINT64_MAX, VkSemaphore(semaphore),
        VK_NULL_HANDLE, index));
}

vk::Result FrameGen::Present(vk::Queue queue, const vk::PresentInfoKHR& info) {
    {
        // A frame the upscaler did not prepare (menus, loading) is presented as it is.
        std::scoped_lock lock{impl->mutex};
        if (!impl->prepared && impl->context) {
            impl->Configure(false);
        }
        impl->prepared = false;
    }
    const VkPresentInfoKHR raw = info;
    return vk::Result(impl->functions.pOutQueuePresentKHR(VkQueue(queue), &raw));
}

void FrameGen::Prepare(vk::CommandBuffer command, const Dlss::Resource& depth,
                       const Dlss::Resource& motion, float jitter_x, float jitter_y, float frame_ms,
                       const FfxApi::Camera& camera) {
    std::scoped_lock lock{impl->mutex};
    auto& d = *impl;
    if (!d.context) {
        return;
    }
    ++d.frame_id;
    d.Configure(true);
    ffxDispatchDescFrameGenerationPrepare prepare{};
    prepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE;
    prepare.frameID = d.frame_id;
    prepare.commandList = reinterpret_cast<void*>(VkCommandBuffer(command));
    prepare.renderSize = {depth.width, depth.height};
    prepare.jitterOffset = {jitter_x, jitter_y};
    prepare.motionVectorScale = {1.0f, 1.0f};
    prepare.frameTimeDelta = frame_ms;
    prepare.cameraNear = camera.near_plane;
    prepare.cameraFar = camera.far_plane;
    prepare.cameraFovAngleVertical = camera.vertical_fov;
    prepare.viewSpaceToMetersFactor = 1.0f;
    prepare.depth = Describe(depth, FFX_API_RESOURCE_USAGE_DEPTHTARGET);
    prepare.motionVectors = Describe(motion, FFX_API_RESOURCE_USAGE_READ_ONLY);
    const ffxReturnCode_t result = d.api->dispatch(&d.context, &prepare.header);
    static bool logged = false;
    if (result != FFX_API_RETURN_OK && !logged) {
        std::printf("Frame generation: prepare failed (%u)\n", result);
        logged = true;
    }
    d.prepared = result == FFX_API_RETURN_OK;
}

#else

struct FfxApi::Impl {};
FfxApi::FfxApi() = default;
FfxApi::~FfxApi() = default;
FfxApi* FfxApi::Get() {
    return nullptr;
}
bool FfxApi::HasContext(const ContextDesc&) const {
    return false;
}
bool FfxApi::CreateContext(vk::PhysicalDevice, vk::Device, const ContextDesc&) {
    return false;
}
bool FfxApi::Dispatch(vk::CommandBuffer, const Dlss::Frame&, const Camera&) {
    return false;
}
void FfxApi::ReleaseContext() {}

struct FrameGen::Impl {};
FrameGen* FrameGen::Get() {
    return nullptr;
}
FrameGen::~FrameGen() = default;
bool FrameGen::CreateSwapchain(const Instance&, vk::SurfaceKHR, const vk::SwapchainCreateInfoKHR&,
                               vk::SwapchainKHR*) {
    return false;
}
void FrameGen::DestroySwapchain() {}
bool FrameGen::Active() const {
    return false;
}
vk::Result FrameGen::GetImages(vk::Device, vk::SwapchainKHR, std::vector<vk::Image>&) {
    return vk::Result::eErrorFeatureNotPresent;
}
vk::Result FrameGen::Acquire(vk::Device, vk::SwapchainKHR, vk::Semaphore, u32*) {
    return vk::Result::eErrorFeatureNotPresent;
}
vk::Result FrameGen::Present(vk::Queue, const vk::PresentInfoKHR&) {
    return vk::Result::eErrorFeatureNotPresent;
}
void FrameGen::Prepare(vk::CommandBuffer, const Dlss::Resource&, const Dlss::Resource&, float,
                       float, float, const FfxApi::Camera&) {}

#endif

} // namespace Vulkan
