// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cmath>

namespace Vulkan::SceneResolution {
struct Size {
    uint32_t width = 1920, height = 1080;
    bool operator==(const Size&) const = default;
};
inline Size ForPreset(int preset, Size output = {}) {
    // Even dimensions for half-resolution effects; match the preset labels exactly.
    constexpr std::array<double, 5> scales{1.0, 1.5, 1.7, 2.0, 3.0};
    const double scale = scales[std::clamp(preset, 0, 4)];
    return {uint32_t(std::max(1l, std::lround(output.width / scale / 2)) * 2),
            uint32_t(std::max(1l, std::lround(output.height / scale / 2)) * 2)};
}
/// iss: the native size of the scene targets the reduced ones stand in for: 1920x1080, or the
/// output resolution with TLG_NATIVE_OUTPUT=1 (inFAMOUS at native 1440p to 2160p through its
/// resolution patches, reduced to the upscaler's preset and upscaled back to it).
inline Size& NativeRef() {
    static Size native{};
    return native;
}
inline Size Native() { return NativeRef(); }
/// The scene size the shaders' FragCoord conversion expects: their native constants are 1920x1080
/// (spirv emit_context: FragCoord * native / scene), so the reduced size is given as the same
/// fraction of 1920x1080 that it is of the native size.
inline Size ShaderScene(Size reduced) {
    const Size native = Native();
    return {uint32_t(std::lround(double(reduced.width) * 1920.0 / native.width)),
            uint32_t(std::lround(double(reduced.height) * 1080.0 / native.height))};
}
constexpr uint32_t Pack(Size size) { return size.width | (size.height << 16); }
constexpr Size Unpack(uint32_t packed) {
    return packed ? Size{packed & 65535u, packed >> 16} : Size{};
}
// A proxy and the guest-size image hold the same logical surface. Native shader reads
// need a resolve after proxy writes; native writes invalidate the cached proxy.
struct Coherence {
    bool valid = false, dirty = false;
    void ProxyWrite() { valid = dirty = true; }
    void CopiedToProxy() { valid = true; dirty = false; }
    void Resolved() { dirty = false; }
    void NativeWrite() { valid = dirty = false; }
};
} // namespace Vulkan::SceneResolution
