// SPDX-FileCopyrightText: Copyright 2026 tlg contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "video_core/renderer_vulkan/vk_ffx_api.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "../../../third_party/ffx-api/ffx_api/ffx_api.h"
#include "../../../third_party/ffx-api/ffx_api/ffx_upscale.h"
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

#endif

} // namespace Vulkan
