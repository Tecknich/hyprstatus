#define WLR_USE_UNSTABLE
#include "Bar.hpp"

#include <hyprland/src/config/shared/complex/ComplexDataTypes.hpp>
#include <hyprland/src/config/shared/parserUtils/ParserUtils.hpp>
#include <hyprland/src/helpers/Monitor.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>
#include <unordered_map>

#include "../globals.hpp"
#include "../render/Markup.hpp"
#include "../render/TextCache.hpp"
#include "BarManager.hpp"

using namespace Render::GL;

// ---- color resolution ------------------------------------------------------

// semantic names usable in module "color"/"color.<cls>" option values
static std::optional<CHyprColor> parseColorString(const std::string& v) {
    static const std::unordered_map<std::string, SP<Config::Values::CColorValue> SGlobalConfig::*> SEMANTIC = {
        {"foreground", &SGlobalConfig::colForeground}, {"foreground_bright", &SGlobalConfig::colForegroundBright},
        {"accent", &SGlobalConfig::colAccent},         {"accent_dim", &SGlobalConfig::colAccentDim},
        {"ok", &SGlobalConfig::colOk},                 {"warn", &SGlobalConfig::colWarn},
        {"err", &SGlobalConfig::colErr},
    };

    if (const auto IT = SEMANTIC.find(v); IT != SEMANTIC.end())
        return cfgColor(g_cfg.*(IT->second));

    if (const auto RES = Config::ParserUtils::parseColor(v); RES.has_value())
        return CHyprColor{(uint64_t)*RES};

    return std::nullopt;
}

static std::optional<CHyprColor> builtinClassColor(const std::string& cls) {
    static const std::unordered_map<std::string, SP<Config::Values::CColorValue> SGlobalConfig::*> MAP = {
        {"active", &SGlobalConfig::colAccent},   {"urgent", &SGlobalConfig::colErr},       {"warning", &SGlobalConfig::colWarn},
        {"critical", &SGlobalConfig::colErr},    {"charging", &SGlobalConfig::colOk},      {"plugged", &SGlobalConfig::colOk},
        {"muted", &SGlobalConfig::colErr},       {"has-updates", &SGlobalConfig::colWarn}, {"notification", &SGlobalConfig::colAccent},
        {"performance", &SGlobalConfig::colAccent}, {"power-saver", &SGlobalConfig::colOk},
    };

    if (const auto IT = MAP.find(cls); IT != MAP.end())
        return cfgColor(g_cfg.*(IT->second));

    return std::nullopt;
}

// segment.fg -> "color.<cls>" opt -> builtin class map -> "color" opt -> col.foreground
static CHyprColor resolveSegmentColor(IModule* mod, const SSegment& seg) {
    if (seg.fg.has_value())
        return *seg.fg;

    if (!seg.cls.empty()) {
        if (const auto V = mod->opt("color." + seg.cls); !V.empty())
            if (const auto C = parseColorString(V); C.has_value())
                return *C;

        if (const auto C = builtinClassColor(seg.cls); C.has_value())
            return *C;
    }

    if (const auto V = mod->opt("color"); !V.empty())
        if (const auto C = parseColorString(V); C.has_value())
            return *C;

    return cfgColor(g_cfg.colForeground);
}

static uint64_t steadyMs() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ---- CBar ------------------------------------------------------------------

CBar::CBar(PHLMONITOR mon) : m_monitor(mon) {}

CBox CBar::barBoxLocal() const {
    const auto MON = m_monitor.lock();
    if (!MON)
        return {};

    const double MARGIN = g_cfg.margin->value();
    const double HEIGHT = std::max<double>(0.0, g_cfg.height->value());
    // A large margin on a narrow output can drive width negative; a degenerate
    // (negative) box must never reach the live-blur path (RASSERT). Clamp to 0.
    const double W = std::max<double>(0.0, MON->m_size.x - 2 * MARGIN);
    const double Y = g_cfg.position->value() == "bottom" ? MON->m_size.y - MARGIN - HEIGHT : MARGIN;

    return {MARGIN, Y, W, HEIGHT};
}

CBox CBar::barBoxGlobal() const {
    const auto MON = m_monitor.lock();
    if (!MON)
        return {};

    return barBoxLocal().translate(MON->m_position);
}

double CBar::reservedLogical() const {
    return g_cfg.margin->value() + g_cfg.height->value();
}

// one measured segment; x is logical, relative to the bar box's left edge
struct SLaidSegment {
    IModule*             module = nullptr;
    SSegment             seg;
    SP<Render::ITexture> tex; // text texture; null for icon segments
    double               x       = 0;
    double               w       = 0; // 2*pad + content
    double               pad     = 0;
    bool                 passive = false;
};

void CBar::draw() {
    m_hitRegions.clear();

    const auto MON = m_monitor.lock();
    if (!MON || !g_barManager)
        return;

    const float SCALE    = MON->m_scale;
    const CBox  BARLOCAL = barBoxLocal();
    const CBox  PIXBAR   = BARLOCAL.copy().scale(SCALE).round();
    if (PIXBAR.w < 1 || PIXBAR.h < 1)
        return;

    const int ROUNDING = (int)std::round(g_cfg.rounding->value() * SCALE);

    g_pHyprOpenGL->renderRect(PIXBAR, cfgColor(g_cfg.colBackground),
                              {.round = ROUNDING, .roundingPower = 2.F, .blur = g_cfg.blur->value(), .blurA = 1.F});

    if (const auto BORDER = g_cfg.borderSize->value(); BORDER > 0)
        g_pHyprOpenGL->renderBorder(PIXBAR, Config::CGradientValueData{cfgColor(g_cfg.colBorder)},
                                    {.round = ROUNDING, .roundingPower = 2.F, .borderSize = (int)std::round(BORDER * SCALE)});

    // ---- layout (logical px; scale only at draw) ----

    const int    FONTPX   = (int)std::round(g_cfg.fontSize->value() * SCALE);
    const double SPACING  = g_cfg.spacing->value();
    const double ICONSIZE = g_cfg.trayIconSize->value();

    // running future m_hitRegions index; groups are measured and later drawn
    // in the same left -> center -> right order, so indices line up
    int regionIdx = 0;

    auto measureGroup = [&](const std::vector<IModule*>& mods) {
        std::vector<std::vector<SLaidSegment>> group;
        for (auto* mod : mods) {
            if (!mod || mod->hidden(MON))
                continue;

            const double PAD     = (double)mod->optInt("padding", g_cfg.padding->value());
            const bool   MODBOLD = mod->optBool("bold", false);

            std::vector<SLaidSegment> laid;
            for (auto& seg : mod->segments(MON)) {
                if (seg.text.empty() && !seg.icon)
                    continue;

                SLaidSegment L;
                L.module  = mod;
                L.pad     = PAD;
                L.passive = seg.cls == "passive";

                CHyprColor col = resolveSegmentColor(mod, seg);
                if (regionIdx == m_hoveredIdx && seg.hoverable)
                    col = cfgColor(g_cfg.colForegroundBright);

                double contentW = 0;
                if (seg.icon)
                    contentW = ICONSIZE;
                else {
                    L.tex = TextCache::get(seg.text, col, FONTPX, seg.bold || MODBOLD);
                    if (!L.tex || L.tex->m_texID == 0)
                        continue;
                    contentW = L.tex->m_size.x / SCALE;
                }

                L.seg = std::move(seg);
                L.w   = 2 * PAD + contentW;
                regionIdx++;
                laid.emplace_back(std::move(L));
            }

            if (!laid.empty())
                group.emplace_back(std::move(laid));
        }
        return group;
    };

    auto groupWidth = [&](const std::vector<std::vector<SLaidSegment>>& group) {
        double w = 0;
        for (size_t i = 0; i < group.size(); ++i) {
            for (const auto& L : group[i])
                w += L.w;
            if (i + 1 < group.size())
                w += SPACING;
        }
        return w;
    };

    auto place = [&](std::vector<std::vector<SLaidSegment>>& group, double startX) {
        double x = startX;
        for (auto& mod : group) {
            for (auto& L : mod) {
                L.x = x;
                x += L.w;
            }
            x += SPACING;
        }
    };

    auto left   = measureGroup(g_barManager->m_layout.left);
    auto center = measureGroup(g_barManager->m_layout.center);
    auto right  = measureGroup(g_barManager->m_layout.right);

    const double LEFTW   = groupWidth(left);
    const double RIGHTW  = groupWidth(right);
    const double CENTERW = groupWidth(center);

    place(left, SPACING);
    place(right, BARLOCAL.w - SPACING - RIGHTW);

    // Center on the bar middle, but clamp into the gap between the left and
    // right groups so the center never overlaps them (a long now-playing title
    // used to bleed over the right modules). When the gap is too narrow the
    // center is pinned to the gap's left edge and clipped by the scissor below.
    const double GAPSTART = SPACING + LEFTW + SPACING;
    const double GAPEND   = BARLOCAL.w - SPACING - RIGHTW - SPACING;
    double       centerX  = (BARLOCAL.w - CENTERW) / 2.0;
    if (centerX + CENTERW > GAPEND)
        centerX = GAPEND - CENTERW;
    if (centerX < GAPSTART)
        centerX = GAPSTART;
    place(center, centerX);

    // ---- draw segments + record hit regions ----

    auto drawGroup = [&](std::vector<std::vector<SLaidSegment>>& group) {
        for (auto& mod : group) {
            if (mod.empty())
                continue;

            // ---- optional per-module pill (rounded background + border) ----
            // Drawn BEFORE the module's glyphs so text sits on top. The extent
            // spans from the first segment's left to the last segment's right
            // (logical px, monitor-local), inset vertically by box-margin.
            if (IModule* MODULE = mod.front().module) {
                const std::string BGSTR     = MODULE->opt("background");
                const std::string BORDERSTR = MODULE->opt("box-border");
                if (!BGSTR.empty() || !BORDERSTR.empty()) {
                    const double BOXMARGIN = (double)MODULE->optInt("box-margin", 4);
                    const int    BOXROUND  = (int)std::round((double)MODULE->optInt("box-rounding", (int64_t)g_cfg.rounding->value()) * SCALE);

                    const double PILLX0 = BARLOCAL.x + mod.front().x;
                    const double PILLX1 = BARLOCAL.x + mod.back().x + mod.back().w;
                    const CBox   PILL   = CBox{PILLX0, BARLOCAL.y + BOXMARGIN, PILLX1 - PILLX0, BARLOCAL.h - 2 * BOXMARGIN}.scale(SCALE).round();

                    if (PILL.w >= 1 && PILL.h >= 1) {
                        if (!BGSTR.empty())
                            if (const auto C = parseColorString(BGSTR); C.has_value())
                                g_pHyprOpenGL->renderRect(PILL, *C, {.round = BOXROUND, .roundingPower = 2.F});

                        if (!BORDERSTR.empty())
                            if (const auto C = parseColorString(BORDERSTR); C.has_value())
                                g_pHyprOpenGL->renderBorder(PILL, Config::CGradientValueData{*C},
                                                            {.round = BOXROUND, .roundingPower = 2.F, .borderSize = std::max(1, (int)std::round(SCALE))});
                    }
                }
            }

            for (auto& L : mod) {
                m_hitRegions.push_back(SHitRegion{
                    .box     = {BARLOCAL.x + L.x, BARLOCAL.y, L.w, BARLOCAL.h},
                    .module  = L.module,
                    .segment = L.seg,
                });

                const float  ALPHA = L.passive ? 0.5F : 1.F;
                const double PIXX  = std::round((BARLOCAL.x + L.x + L.pad) * SCALE);

                if (L.seg.icon) {
                    const double ICONPX = std::round(ICONSIZE * SCALE);
                    const CBox   BOX    = {PIXX, std::round(PIXBAR.y + (PIXBAR.h - ICONPX) / 2.0), ICONPX, ICONPX};
                    if (BOX.w >= 1 && BOX.h >= 1)
                        g_pHyprOpenGL->renderTexture(L.seg.icon, BOX, {.a = ALPHA});
                } else if (L.tex) {
                    // texture is already scale-baked: draw 1:1 in pixel space
                    const CBox BOX = {PIXX, std::round(PIXBAR.y + (PIXBAR.h - L.tex->m_size.y) / 2.0), L.tex->m_size.x, L.tex->m_size.y};
                    if (BOX.w >= 1 && BOX.h >= 1)
                        g_pHyprOpenGL->renderTexture(L.tex, BOX, {.a = ALPHA});
                }
            }
        }
    };

    // draw order must stay left -> center -> right so the hit regions line up
    // with the regionIdx assigned during measurement (hover highlight mapping).
    drawGroup(left);

    // clip the center group to the gap so an over-long center title is cut off
    // at the side groups instead of overlapping them.
    const double CLIPX0 = std::clamp(GAPSTART, 0.0, BARLOCAL.w);
    const double CLIPX1 = std::clamp(GAPEND, 0.0, BARLOCAL.w);
    if (CLIPX1 > CLIPX0) {
        const CBox CLIP = CBox{BARLOCAL.x + CLIPX0, BARLOCAL.y, CLIPX1 - CLIPX0, BARLOCAL.h}.scale(SCALE).round();
        g_pHyprOpenGL->scissor(CLIP);
        drawGroup(center);
        g_pHyprOpenGL->scissor(PIXBAR); // restore to the bar box for the right group
    } else
        drawGroup(center); // degenerate gap: draw unclipped so it stays clickable

    drawGroup(right);
    g_pHyprOpenGL->scissor(nullptr);

    // A relayout under a stationary cursor (a module widened, segments shifted)
    // moves segments out from under the index onMouseMove recorded, so the stale
    // m_hoveredIdx now points at a different segment -> the highlight above and
    // drawTooltip below attach to the wrong one. Re-resolve the hovered index by
    // hit-testing the cursor's current position against the freshly-built
    // regions so hover stays bound to the segment actually under the cursor.
    // Only when a hover is already active: starting one here would bypass the
    // hover-start timestamp/tooltip-timer setup owned by CBarManager::onMouseMove.
    if (m_hoveredIdx >= 0) {
        int resolvedIdx = -1;
        hitTest(g_pInputManager->getMouseCoordsInternal(), &resolvedIdx); // -1 if cursor covers no segment
        if (resolvedIdx != m_hoveredIdx) {
            m_hoveredIdx = resolvedIdx;
            damage(); // this frame already drew the stale highlight; force a corrective redraw
        }
    }
}

void CBar::drawTooltip() {
    const auto MON = m_monitor.lock();
    if (!MON || !g_barManager)
        return;

    if (!g_cfg.tooltips->value())
        return;

    if (m_hoveredIdx < 0 || (size_t)m_hoveredIdx >= m_hitRegions.size())
        return;

    if (steadyMs() < m_hoverStartMs + (uint64_t)g_cfg.tooltipDelayMs->value())
        return;

    const auto& REGION = m_hitRegions[m_hoveredIdx];
    if (!REGION.module)
        return;

    const auto TEXT = REGION.module->tooltip(REGION.segment);
    if (TEXT.empty())
        return;

    const float SCALE  = MON->m_scale;
    const int   FONTPX = (int)std::round(g_cfg.fontSize->value() * SCALE);

    // Markup tooltips (the clock's colored calendar) carry their own per-span
    // colors, so they go through the cairo+pango path and are drawn untinted.
    // Plain tooltips keep the single-color TextCache path with word wrapping.
    SP<Render::ITexture> TEX;
    if (REGION.module->tooltipIsMarkup())
        TEX = MarkupText::get(TEXT, FONTPX, g_cfg.fontFamily ? g_cfg.fontFamily->value() : std::string{"monospace"});
    else
        TEX = TextCache::get(TEXT, cfgColor(g_cfg.colTooltipFg), FONTPX, false, (int)std::round(600.0 * SCALE));
    if (!TEX || TEX->m_texID == 0)
        return;

    constexpr double PAD  = 8; // logical
    const double     BOXW = TEX->m_size.x / SCALE + 2 * PAD;
    const double     BOXH = TEX->m_size.y / SCALE + 2 * PAD;

    const CBox BARLOCAL = barBoxLocal();
    const bool BOTTOM   = g_cfg.position->value() == "bottom";

    // just below (top bar) / above (bottom bar) the hovered segment, clamped on-monitor
    double x = REGION.box.x + REGION.box.w / 2.0 - BOXW / 2.0;
    double y = BOTTOM ? BARLOCAL.y - BOXH - 4 : BARLOCAL.y + BARLOCAL.h + 4;
    x        = std::clamp(x, 0.0, std::max(0.0, MON->m_size.x - BOXW));
    y        = std::clamp(y, 0.0, std::max(0.0, MON->m_size.y - BOXH));

    const CBox PIXBOX = CBox{x, y, BOXW, BOXH}.scale(SCALE).round();
    if (PIXBOX.w < 1 || PIXBOX.h < 1)
        return;

    const int ROUNDING = (int)std::round(g_cfg.rounding->value() * SCALE);

    g_pHyprOpenGL->renderRect(PIXBOX, cfgColor(g_cfg.colTooltipBg), {.round = ROUNDING, .roundingPower = 2.F});
    g_pHyprOpenGL->renderBorder(PIXBOX, Config::CGradientValueData{cfgColor(g_cfg.colBorder)},
                                {.round = ROUNDING, .roundingPower = 2.F, .borderSize = std::max(1, (int)std::round(SCALE))});

    const CBox TEXBOX = {PIXBOX.x + std::round(PAD * SCALE), PIXBOX.y + std::round((PIXBOX.h - TEX->m_size.y) / 2.0), TEX->m_size.x, TEX->m_size.y};
    if (TEXBOX.w >= 1 && TEXBOX.h >= 1)
        g_pHyprOpenGL->renderTexture(TEX, TEXBOX, {.a = 1.F});
}

bool CBar::hitTest(const Vector2D& global, int* regionIdx) const {
    const auto MON = m_monitor.lock();
    if (!MON)
        return false;

    const Vector2D LOCAL = global - MON->m_position;

    for (size_t i = 0; i < m_hitRegions.size(); ++i) {
        if (m_hitRegions[i].box.containsPoint(LOCAL)) {
            if (regionIdx)
                *regionIdx = (int)i;
            return true;
        }
    }

    return false;
}

void CBar::damage() const {
    if (m_monitor.expired())
        return;

    g_pHyprRenderer->damageBox(barBoxGlobal());
}
