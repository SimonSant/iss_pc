// SPDX-FileCopyrightText: Copyright 2026 tlg contributors
// SPDX-License-Identifier: GPL-2.0-or-later
//
// tlg: FSR 3.1 through AMD's FidelityFX API DLL (amd_fidelityfx_vk.dll), loaded at run time from
// next to tlg-probe.exe. This is the call OptiScaler intercepts: with OptiScaler installed next
// to the exe, its menu (Insert) can replace the output with FSR 4 or other upscalers. Without the
// DLL, or with TLG_FFX_DLL=0, the built-in FSR 3.1 runs as before.

#pragma once

#include <memory>

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
};

} // namespace Vulkan
