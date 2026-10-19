// SPDX-FileCopyrightText: Copyright 2026 tlg contributors
// SPDX-License-Identifier: GPL-2.0-or-later
//
// tlg: FSR 3.1 through AMD's FidelityFX API DLL (amd_fidelityfx_vk.dll), loaded at run time from
// next to tlg-probe.exe. This is the call OptiScaler intercepts: with OptiScaler installed next
// to the exe, its menu (Insert) can replace the output with FSR 4 or other upscalers. Without the
// DLL, or with TLG_FFX_DLL=0, the built-in FSR 3.1 runs as before.

#pragma once

#include <memory>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vk_dlss.h"

namespace Vulkan {

class FfxApi {
public:
    /// The process-wide instance, or null when the DLL is absent or TLG_FFX_DLL=0.
    static FfxApi* Get();
    ~FfxApi();

    struct ContextDesc {
        u32 render_width, render_height, output_width, output_height;
        bool hdr;
        bool operator==(const ContextDesc&) const = default;
    };
    struct Camera {
        float near_plane, far_plane, vertical_fov;
    };

    [[nodiscard]] bool HasContext(const ContextDesc& desc) const;
    /// The GPU must be done with the previous context.
    bool CreateContext(vk::PhysicalDevice physical, vk::Device device, const ContextDesc& desc);
    bool Dispatch(vk::CommandBuffer command, const Dlss::Frame& frame, const Camera& camera);
    void ReleaseContext();

private:
    FfxApi();
    struct Impl;
    std::unique_ptr<Impl> impl;
    friend class FrameGen;
};

class Instance;

/// iss: FSR 3.1 frame generation through the same DLL (TLG_FRAMEGEN=1): its swapchain replaces
/// the port's, and every frame the upscaler hands it the depth and motion vectors (Prepare).
class FrameGen {
public:
    /// The process-wide instance, or null without TLG_FRAMEGEN=1 or the DLL.
    static FrameGen* Get();
    ~FrameGen();

    /// Creates the frame generation swapchain in place of vkCreateSwapchainKHR; false keeps the
    /// normal swapchain (the reason is logged).
    bool CreateSwapchain(const Instance& instance, vk::SurfaceKHR surface,
                         const vk::SwapchainCreateInfoKHR& info, vk::SwapchainKHR* swapchain);
    void DestroySwapchain();
    [[nodiscard]] bool Active() const;

    // The swapchain functions while Active().
    vk::Result GetImages(vk::Device device, vk::SwapchainKHR swapchain, std::vector<vk::Image>& images);
    vk::Result Acquire(vk::Device device, vk::SwapchainKHR swapchain, vk::Semaphore semaphore,
                       u32* index);
    vk::Result Present(vk::Queue queue, const vk::PresentInfoKHR& info);

    /// Records the frame's preparation (depth, motion vectors at render size) and turns
    /// generation on for the next present.
    void Prepare(vk::CommandBuffer command, const Dlss::Resource& depth,
                 const Dlss::Resource& motion, float jitter_x, float jitter_y, float frame_ms,
                 const FfxApi::Camera& camera);

private:
    FrameGen() = default;
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Vulkan
