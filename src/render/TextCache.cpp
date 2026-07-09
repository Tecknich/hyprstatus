#define WLR_USE_UNSTABLE
#include "TextCache.hpp"

#include <hyprland/src/render/Renderer.hpp>

#include <format>
#include <list>
#include <unordered_map>

#include "../globals.hpp"

namespace {
    constexpr size_t CACHE_CAP = 256;

    struct SEntry {
        std::string          key;
        SP<Render::ITexture> tex;
    };

    // front = most recently used
    std::list<SEntry>                                            g_lru;
    std::unordered_map<std::string, std::list<SEntry>::iterator> g_index;
}

SP<Render::ITexture> TextCache::get(const std::string& text, const CHyprColor& color, int pxSize, bool bold, int maxWidthPx) {
    if (text.empty() || pxSize <= 0)
        return nullptr;

    const auto FONT   = g_cfg.fontFamily ? g_cfg.fontFamily->value() : std::string{"Sans"};
    const int  WEIGHT = bold ? 700 : 400;
    // \x1f separates free-form fields (text/font) from the fixed-form tail
    const auto KEY = std::format("{}\x1f{}\x1f{}|{}|{:08x}|{}", text, FONT, pxSize, WEIGHT, color.getAsHex(), maxWidthPx);

    if (const auto IT = g_index.find(KEY); IT != g_index.end()) {
        g_lru.splice(g_lru.begin(), g_lru, IT->second);
        return IT->second->tex;
    }

    auto tex = g_pHyprRenderer->renderText(text, color, pxSize, false /* italic */, FONT, maxWidthPx, WEIGHT);
    if (!tex || tex->m_texID == 0)
        return tex; // never cache invalid textures

    g_lru.push_front(SEntry{KEY, tex});
    g_index[KEY] = g_lru.begin();

    if (g_lru.size() > CACHE_CAP) {
        g_index.erase(g_lru.back().key);
        g_lru.pop_back();
    }

    return tex;
}

void TextCache::clear() {
    g_index.clear();
    g_lru.clear(); // ITexture dtor makes the EGL context current itself
}
