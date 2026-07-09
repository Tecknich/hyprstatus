#pragma once
#include <string>
#define WLR_USE_UNSTABLE
#include <hyprland/src/helpers/Color.hpp>
#include <hyprland/src/render/Texture.hpp>

// LRU cache over g_pHyprRenderer->renderText(). pxSize must already include
// the monitor scale; textures are drawn 1:1 at tex->m_size. Main thread only
// (renderText makes the EGL context current itself, so cache misses are safe
// outside the render pass too).
namespace TextCache {
    SP<Render::ITexture> get(const std::string& text, const CHyprColor& color, int pxSize,
                             bool bold = false, int maxWidthPx = 0);
    void                 clear(); // config reload + plugin exit
}
