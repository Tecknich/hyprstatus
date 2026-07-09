#pragma once
#include <string>
#define WLR_USE_UNSTABLE
#include <hyprland/src/helpers/memory/Memory.hpp>
#include <hyprland/src/render/Texture.hpp>

// LRU cache over cairo+pango markup -> texture. Unlike TextCache (single color,
// via g_pHyprRenderer->renderText), the markup string carries ALL of its own
// colors via <span foreground=..>/<span background=..>/<b> tags, so the returned
// texture is drawn at alpha 1.0 with no extra tinting. Used by the clock's
// colored, today-highlighted calendar tooltip.
//
// Main thread only: createTexture() makes the EGL context current itself, so
// cache misses are safe outside the render pass.
namespace MarkupText {
    // pangoMarkup: a well-formed Pango markup string. Malformed markup yields a
    // null texture (never crashes). pxSize already includes the monitor scale.
    // fontFamily is the primary family (a monospace family is appended as a
    // fallback so calendar columns still align if the configured font is
    // missing); empty -> "monospace".
    SP<Render::ITexture> get(const std::string& pangoMarkup, int pxSize, const std::string& fontFamily);

    // config reload + plugin exit (drops all cached textures)
    void clear();
}
