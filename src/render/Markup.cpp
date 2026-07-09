#define WLR_USE_UNSTABLE
#include "Markup.hpp"

#include <hyprland/src/render/Renderer.hpp>

#include <pango/pangocairo.h>

#include <format>
#include <list>
#include <unordered_map>

#include "../globals.hpp"

namespace {
    constexpr size_t CACHE_CAP = 64;

    struct SEntry {
        std::string          key;
        SP<Render::ITexture> tex;
    };

    // front = most recently used
    std::list<SEntry>                                            g_lru;
    std::unordered_map<std::string, std::list<SEntry>::iterator> g_index;

    // Render `markup` at `pxSize` px with `family` into an ARGB32 cairo surface
    // and upload it as a GL texture. Returns null on malformed markup or a
    // degenerate (empty) layout.
    SP<Render::ITexture> renderMarkup(const std::string& markup, int pxSize, const std::string& family) {
        // Reject malformed markup up-front so a bad string never reaches pango's
        // layout machinery (returns null -> caller draws no tooltip).
        {
            GError* perr = nullptr;
            if (!pango_parse_markup(markup.c_str(), -1, 0, nullptr, nullptr, nullptr, &perr)) {
                if (perr)
                    g_error_free(perr);
                return nullptr;
            }
        }

        // One font description reused for measurement and paint. pango copies it
        // into each layout via set_font_description, so we own/free it once here.
        PangoFontDescription* desc = pango_font_description_from_string(family.c_str());
        pango_font_description_set_absolute_size(desc, (double)pxSize * PANGO_SCALE);

        // ---- measure ----
        cairo_surface_t* measSurf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
        cairo_t*         measCr   = cairo_create(measSurf);
        PangoLayout*     measLay  = pango_cairo_create_layout(measCr);
        pango_layout_set_font_description(measLay, desc);
        pango_layout_set_markup(measLay, markup.c_str(), -1);

        PangoRectangle logRect{};
        pango_layout_get_pixel_extents(measLay, nullptr, &logRect);
        const int W = logRect.width;
        const int H = logRect.height;

        g_object_unref(measLay);
        cairo_destroy(measCr);
        cairo_surface_destroy(measSurf);

        if (W <= 0 || H <= 0) {
            pango_font_description_free(desc);
            return nullptr;
        }

        // ---- paint ----
        cairo_surface_t* surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
        cairo_t*         cr   = cairo_create(surf);

        // fully transparent background: the tooltip's own bg rect shows through
        cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
        cairo_paint(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

        // default foreground for any text not wrapped in a <span foreground=..>
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);

        PangoLayout* lay = pango_cairo_create_layout(cr);
        pango_layout_set_font_description(lay, desc);
        pango_layout_set_markup(lay, markup.c_str(), -1);
        // shift by the logical origin (normally 0,0) so nothing clips at the edge
        cairo_translate(cr, -logRect.x, -logRect.y);
        pango_cairo_show_layout(cr, lay);

        cairo_surface_flush(surf);

        SP<Render::ITexture> tex = g_pHyprRenderer->createTexture(surf);

        g_object_unref(lay);
        cairo_destroy(cr);
        cairo_surface_destroy(surf);
        pango_font_description_free(desc);

        return tex;
    }
}

SP<Render::ITexture> MarkupText::get(const std::string& pangoMarkup, int pxSize, const std::string& fontFamily) {
    if (pangoMarkup.empty() || pxSize <= 0)
        return nullptr;

    // primary family + a generic monospace fallback so grid columns still line
    // up if the configured family is not installed
    std::string family = fontFamily.empty() ? std::string{"monospace"} : fontFamily + ", monospace";

    // \x1f separates the free-form markup/font fields from the numeric tail
    const auto KEY = std::format("{}\x1f{}\x1f{}", pangoMarkup, family, pxSize);

    if (const auto IT = g_index.find(KEY); IT != g_index.end()) {
        g_lru.splice(g_lru.begin(), g_lru, IT->second);
        return IT->second->tex;
    }

    auto tex = renderMarkup(pangoMarkup, pxSize, family);
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

void MarkupText::clear() {
    g_index.clear();
    g_lru.clear(); // ITexture dtor makes the EGL context current itself
}
