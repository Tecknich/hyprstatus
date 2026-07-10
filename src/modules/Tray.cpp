#include "Factories.hpp"

#include <endian.h>
#include <linux/input-event-codes.h>
#include <systemd/sd-bus.h>

#include <cairo/cairo.h>
#include <librsvg/rsvg.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

#define WLR_USE_UNSTABLE
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/config/shared/complex/ComplexDataTypes.hpp>
#include <hyprland/src/helpers/Monitor.hpp>
#include <hyprland/src/helpers/math/Math.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>

#include "../globals.hpp"
#include "../render/TextCache.hpp"
#include "../services/DBus.hpp"
#include "../services/MainThread.hpp"

using namespace Render::GL;

// StatusNotifierItem tray: the plugin owns org.kde.StatusNotifierWatcher +
// a StatusNotifierHost on the session bus. If another watcher (waybar) is
// alive, we fall back to consuming its registry and re-attempt ownership when
// the name frees. All sd-bus callbacks arrive on the compositor main thread
// (DBus::session() is pumped on the wl event loop).
// DBusMenu rendering is out of scope in v1 — menu-only appindicator items may
// ignore Activate/ContextMenu (documented in DESIGN.md).

namespace {

    constexpr const char* WATCHER_NAME  = "org.kde.StatusNotifierWatcher";
    constexpr const char* WATCHER_PATH  = "/StatusNotifierWatcher";
    constexpr const char* WATCHER_IFACE = "org.kde.StatusNotifierWatcher";
    constexpr const char* ITEM_IFACE    = "org.kde.StatusNotifierItem";
    constexpr const char* HOST_NAME     = "org.kde.StatusNotifierHost-hyprstatus";
    constexpr const char* PROPS_IFACE   = "org.freedesktop.DBus.Properties";
    constexpr const char* MENU_IFACE    = "com.canonical.dbusmenu";

    class CTrayModule;

    struct STrayPixmap {
        int32_t              width = 0, height = 0;
        std::vector<uint8_t> data; // raw wire bytes: ARGB32, network order, NON-premultiplied
    };

    struct STrayItem {
        ~STrayItem() {
            // userdata of these slots is this item — they must die with it
            if (slotSignals)
                sd_bus_slot_unref(slotSignals);
            if (slotOwnerChanged)
                sd_bus_slot_unref(slotOwnerChanged);
            if (slotGetAll)
                sd_bus_slot_unref(slotGetAll);
        }

        CTrayModule* owner = nullptr;
        std::string  busName, objectPath, service; // service = busName + objectPath (registry form)

        std::string              status, iconName, attentionIconName, iconThemePath, title, menuPath;
        std::string              tooltipTitle, tooltipBody;
        bool                     itemIsMenu = false;
        std::vector<STrayPixmap> pixmaps;

        bool         pendingGetAll = false;
        sd_bus_slot* slotSignals      = nullptr; // all org.kde.StatusNotifierItem signals from this item
        sd_bus_slot* slotOwnerChanged = nullptr; // NameOwnerChanged arg0=busName
        sd_bus_slot* slotGetAll       = nullptr; // in-flight Properties.GetAll

        // px -> texture; an entry with nullptr = lookup attempted and failed
        std::map<int, SP<Render::ITexture>> texByPx;
    };

    // service registry strings come in two forms: "busname" (path defaults to
    // /StatusNotifierItem) or "busname/obj/path"
    std::pair<std::string, std::string> parseService(const std::string& s) {
        const auto POS = s.find('/');
        if (POS == std::string::npos)
            return {s, "/StatusNotifierItem"};
        if (POS == 0)
            return {"", s}; // bare path: cannot resolve a bus name from a registry string
        return {s.substr(0, POS), s.substr(POS)};
    }

    // A D-Bus bus name (unique ":1.23" or well-known "org.kde.Foo") is a restricted
    // charset. We concatenate busName into an sd_bus_add_match rule string, so any
    // char outside this set (notably a quote/comma) could break out of the
    // arg0='...' clause and inject match conditions. Reject anything malformed
    // before it ever reaches addItem / the match rule (fix 3).
    bool isValidBusName(const std::string& n) {
        if (n.empty())
            return false;
        for (const unsigned char C : n) {
            const bool OK = (C >= 'A' && C <= 'Z') || (C >= 'a' && C <= 'z') || (C >= '0' && C <= '9') || C == '_' || C == '.' || C == ':' || C == '-';
            if (!OK)
                return false;
        }
        return true;
    }

    // ---- com.canonical.dbusmenu model ----

    struct SMenuEntry {
        int32_t                 id          = 0;
        std::string             label;                 // '_' accelerator markers stripped
        bool                    enabled     = true;
        bool                    visible     = true;
        std::string             type;                  // "" (standard) | "separator"
        std::string             toggleType;            // "" | "checkmark" | "radio"
        int32_t                 toggleState = -1;      // 0 off, 1 on, -1 indeterminate
        bool                    hasSubmenu  = false;   // children-display == "submenu"
        std::vector<SMenuEntry> children;
    };

    // hit box for one popup entry (monitor-local LOGICAL), tagged by column
    struct SMenuHit {
        CBox box;
        int  column = 0; // 0 = top level, 1 = expanded submenu
        int  index  = 0; // index into that column's entry vector
    };

    // GTK menus mark the mnemonic with a single '_'; '__' is a literal underscore.
    std::string stripAccel(const std::string& in) {
        std::string out;
        out.reserve(in.size());
        bool removed = false;
        for (size_t i = 0; i < in.size(); ++i) {
            if (in[i] == '_') {
                if (i + 1 < in.size() && in[i + 1] == '_') { // "__" -> "_"
                    out += '_';
                    ++i;
                    continue;
                }
                if (!removed) { // drop the first standalone mnemonic marker
                    removed = true;
                    continue;
                }
            }
            out += in[i];
        }
        return out;
    }

    // read one property variant of a known basic type; on type mismatch skip it
    void readMenuStr(sd_bus_message* m, std::string& out, bool strip) {
        if (sd_bus_message_enter_container(m, SD_BUS_TYPE_VARIANT, "s") > 0) {
            const char* v = nullptr;
            sd_bus_message_read(m, "s", &v);
            sd_bus_message_exit_container(m);
            out = v ? v : "";
            if (strip)
                out = stripAccel(out);
        } else
            sd_bus_message_skip(m, "v");
    }
    void readMenuBool(sd_bus_message* m, bool& out) {
        if (sd_bus_message_enter_container(m, SD_BUS_TYPE_VARIANT, "b") > 0) {
            int b = 0;
            sd_bus_message_read(m, "b", &b);
            sd_bus_message_exit_container(m);
            out = b != 0;
        } else
            sd_bus_message_skip(m, "v");
    }
    void readMenuInt(sd_bus_message* m, int32_t& out) {
        if (sd_bus_message_enter_container(m, SD_BUS_TYPE_VARIANT, "i") > 0) {
            int v = 0;
            sd_bus_message_read(m, "i", &v);
            sd_bus_message_exit_container(m);
            out = v;
        } else
            sd_bus_message_skip(m, "v");
    }

    // Defense-in-depth cap on dbusmenu nesting: a malicious client could ship a
    // pathologically deep layout to blow our stack. Beyond this we stop
    // descending (still consuming the wire data) rather than recurse (fix 4).
    constexpr int MAX_MENU_DEPTH = 16;

    // parse one (ia{sv}av) node; recursion covers the whole tree from GetLayout
    // with depth -1. Returns false only if the struct itself could not be read;
    // partial property/child failures degrade gracefully (never throw/crash).
    bool parseMenuNode(sd_bus_message* m, SMenuEntry& out, int depth = 0) {
        if (sd_bus_message_enter_container(m, SD_BUS_TYPE_STRUCT, "ia{sv}av") <= 0)
            return false;

        sd_bus_message_read(m, "i", &out.id); // id (default 0 on failure)

        if (sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, "{sv}") > 0) {
            while (sd_bus_message_enter_container(m, SD_BUS_TYPE_DICT_ENTRY, "sv") > 0) {
                const char* key = nullptr;
                if (sd_bus_message_read(m, "s", &key) < 0) {
                    sd_bus_message_exit_container(m);
                    break;
                }
                const std::string K = key ? key : "";
                if (K == "label")
                    readMenuStr(m, out.label, true);
                else if (K == "type")
                    readMenuStr(m, out.type, false);
                else if (K == "toggle-type")
                    readMenuStr(m, out.toggleType, false);
                else if (K == "children-display") {
                    std::string cd;
                    readMenuStr(m, cd, false);
                    out.hasSubmenu = cd == "submenu";
                } else if (K == "enabled")
                    readMenuBool(m, out.enabled);
                else if (K == "visible")
                    readMenuBool(m, out.visible);
                else if (K == "toggle-state")
                    readMenuInt(m, out.toggleState);
                else
                    sd_bus_message_skip(m, "v");
                sd_bus_message_exit_container(m); // dict entry
            }
            sd_bus_message_exit_container(m); // a{sv}
        }

        if (depth >= MAX_MENU_DEPTH) {
            // at the cap: consume the children array without descending
            sd_bus_message_skip(m, "av");
        } else if (sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, "v") > 0) {
            while (sd_bus_message_enter_container(m, SD_BUS_TYPE_VARIANT, "(ia{sv}av)") > 0) {
                SMenuEntry child;
                if (parseMenuNode(m, child, depth + 1))
                    out.children.emplace_back(std::move(child));
                sd_bus_message_exit_container(m); // variant
            }
            sd_bus_message_exit_container(m); // av
        }

        sd_bus_message_exit_container(m); // struct
        return true;
    }

    // ---- icon rasterization (cairo, main thread) ----

    cairo_surface_t* scaleOnto(cairo_surface_t* src, double w, double h, int px) {
        cairo_surface_t* out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, px, px);
        if (cairo_surface_status(out) != CAIRO_STATUS_SUCCESS) {
            cairo_surface_destroy(out);
            cairo_surface_destroy(src);
            return nullptr;
        }
        cairo_t*   cr    = cairo_create(out);
        const auto SCALE = std::min(px / w, px / h);
        cairo_translate(cr, (px - w * SCALE) / 2.0, (px - h * SCALE) / 2.0);
        cairo_scale(cr, SCALE, SCALE);
        cairo_set_source_surface(cr, src, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
        cairo_paint(cr);
        cairo_destroy(cr);
        cairo_surface_destroy(src);
        return out;
    }

    cairo_surface_t* surfaceFromPNG(const std::string& path, int px) {
        cairo_surface_t* src = cairo_image_surface_create_from_png(path.c_str());
        if (!src)
            return nullptr;
        if (cairo_surface_status(src) != CAIRO_STATUS_SUCCESS) {
            cairo_surface_destroy(src);
            return nullptr;
        }
        const double W = cairo_image_surface_get_width(src);
        const double H = cairo_image_surface_get_height(src);
        if (W < 1 || H < 1) {
            cairo_surface_destroy(src);
            return nullptr;
        }
        return scaleOnto(src, W, H, px);
    }

    cairo_surface_t* surfaceFromSVG(const std::string& path, int px) {
        RsvgHandle* handle = rsvg_handle_new_from_file(path.c_str(), nullptr);
        if (!handle)
            return nullptr;
        cairo_surface_t* out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, px, px);
        if (cairo_surface_status(out) != CAIRO_STATUS_SUCCESS) {
            cairo_surface_destroy(out);
            g_object_unref(handle);
            return nullptr;
        }
        cairo_t*      cr       = cairo_create(out);
        RsvgRectangle viewport = {0, 0, (double)px, (double)px};
        const bool    OK       = rsvg_handle_render_document(handle, cr, &viewport, nullptr);
        cairo_destroy(cr);
        g_object_unref(handle);
        if (!OK) {
            cairo_surface_destroy(out);
            return nullptr;
        }
        return out;
    }

    cairo_surface_t* surfaceFromPixmap(const STrayPixmap& pm, int px) {
        cairo_surface_t* src = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pm.width, pm.height);
        if (cairo_surface_status(src) != CAIRO_STATUS_SUCCESS) {
            cairo_surface_destroy(src);
            return nullptr;
        }
        cairo_surface_flush(src);
        uint8_t*   dst    = cairo_image_surface_get_data(src);
        const auto STRIDE = cairo_image_surface_get_stride(src);
        for (int32_t y = 0; y < pm.height; ++y) {
            auto* row = reinterpret_cast<uint32_t*>(dst + (size_t)y * STRIDE);
            for (int32_t x = 0; x < pm.width; ++x) {
                uint32_t v = 0;
                std::memcpy(&v, pm.data.data() + 4 * ((size_t)y * pm.width + x), 4);
                // SNI pixmaps are network-order non-premultiplied ARGB;
                // CAIRO_FORMAT_ARGB32 wants native-endian premultiplied
                v                = be32toh(v);
                const uint32_t A = v >> 24, R = (v >> 16) & 0xff, G = (v >> 8) & 0xff, B = v & 0xff;
                row[x]           = (A << 24) | ((R * A / 255) << 16) | ((G * A / 255) << 8) | (B * A / 255);
            }
        }
        cairo_surface_mark_dirty(src);
        if (pm.width == px && pm.height == px)
            return src;
        return scaleOnto(src, pm.width, pm.height, px);
    }

    // ---- sd-bus callbacks (all main thread) ----

    int onRegisterItem(sd_bus_message* m, void* userdata, sd_bus_error* retError);
    int onRegisterHost(sd_bus_message* m, void* userdata, sd_bus_error* retError);
    int propItems(sd_bus* bus, const char* path, const char* iface, const char* prop, sd_bus_message* reply, void* userdata, sd_bus_error* retError);
    int propHostRegistered(sd_bus* bus, const char* path, const char* iface, const char* prop, sd_bus_message* reply, void* userdata, sd_bus_error* retError);
    int propProtocolVersion(sd_bus* bus, const char* path, const char* iface, const char* prop, sd_bus_message* reply, void* userdata, sd_bus_error* retError);
    int onItemSignal(sd_bus_message* m, void* userdata, sd_bus_error* retError);
    int onItemOwnerChanged(sd_bus_message* m, void* userdata, sd_bus_error* retError);
    int onGetAllReply(sd_bus_message* m, void* userdata, sd_bus_error* retError);
    int onMenuLayout(sd_bus_message* m, void* userdata, sd_bus_error* retError);
    int onClientItemRegistered(sd_bus_message* m, void* userdata, sd_bus_error* retError);
    int onClientItemUnregistered(sd_bus_message* m, void* userdata, sd_bus_error* retError);
    int onClientItemsReply(sd_bus_message* m, void* userdata, sd_bus_error* retError);
    int onWatcherOwnerChanged(sd_bus_message* m, void* userdata, sd_bus_error* retError);

    const sd_bus_vtable WATCHER_VTABLE[] = {
        SD_BUS_VTABLE_START(0),
        SD_BUS_METHOD_WITH_NAMES("RegisterStatusNotifierItem", "s", SD_BUS_PARAM(service), "", "", onRegisterItem, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD_WITH_NAMES("RegisterStatusNotifierHost", "s", SD_BUS_PARAM(service), "", "", onRegisterHost, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_PROPERTY("RegisteredStatusNotifierItems", "as", propItems, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
        SD_BUS_PROPERTY("IsStatusNotifierHostRegistered", "b", propHostRegistered, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
        SD_BUS_PROPERTY("ProtocolVersion", "i", propProtocolVersion, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_SIGNAL_WITH_NAMES("StatusNotifierItemRegistered", "s", SD_BUS_PARAM(service), 0),
        SD_BUS_SIGNAL_WITH_NAMES("StatusNotifierItemUnregistered", "s", SD_BUS_PARAM(service), 0),
        SD_BUS_SIGNAL("StatusNotifierHostRegistered", "", 0),
        SD_BUS_SIGNAL("StatusNotifierHostUnregistered", "", 0),
        SD_BUS_VTABLE_END,
    };

    class CTrayModule : public IModule {
      public:
        explicit CTrayModule(const SModuleConfig& cfg) : IModule(cfg) {}

        ~CTrayModule() override {
            // slots first: every callback userdata is a raw pointer into us
            if (m_slotMenuLayout) {
                sd_bus_slot_unref(m_slotMenuLayout);
                m_slotMenuLayout = nullptr;
            }
            m_popupOpen = false;
            m_items.clear();
            dropClientSlots();
            if (m_slotVtable) {
                sd_bus_slot_unref(m_slotVtable);
                m_slotVtable = nullptr;
            }
            if (m_bus) {
                if (m_ownsHostName)
                    sd_bus_release_name(m_bus, HOST_NAME);
                if (m_isWatcher)
                    sd_bus_release_name(m_bus, WATCHER_NAME);
                DBus::flush(m_bus);
            }
            // Drop our owned ref LAST — after every slot unref and release_name
            // above. We hold an sd_bus_ref (taken in init) so the bus object
            // stays alive through teardown even if DBus already dropped its own
            // ref (e.g. after an auto-disconnect on bus error). Otherwise the
            // final slot unref could free the bus and release_name/flush would
            // touch freed memory. sd_bus_unref(nullptr) is a safe no-op.
            sd_bus_unref(m_bus);
        }

        void init() override {
            sd_bus* bus = DBus::session();
            if (!bus)
                return; // no bus: stay hidden, own no ref
            m_bus = sd_bus_ref(bus); // owned ref, released last in the dtor
            if (!tryBecomeWatcher())
                enterClientMode();

            // Self-heal: when the session-bus connection drops and DBus reopens
            // it, the old bus object + every slot we added on it are dead. Re-
            // attach to the fresh connection. Guarded by m_alive so a reload-
            // orphaned copy of this callback is a safe no-op (fix 6).
            DBus::onReconnect(false /*session*/, "tray", [this, weak = WP<bool>(m_alive)] {
                if (!weak.lock())
                    return; // module was destroyed (e.g. config reload); do not touch it
                reattach();
            });
        }

        void update() override {
            for (auto& item : m_items)
                requestGetAll(item.get());
        }

        std::vector<SSegment> segments(PHLMONITOR mon) override {
            std::vector<SSegment> out;
            if (!mon)
                return out;
            const int PX = std::max(1, (int)std::lround((double)g_cfg.trayIconSize->value() * mon->m_scale));
            out.reserve(m_items.size());
            for (size_t i = 0; i < m_items.size(); ++i) {
                auto* const ITEM = m_items[i].get();
                SSegment    seg;
                seg.id   = i;
                seg.icon = iconFor(ITEM, PX);
                if (!seg.icon)
                    seg.text = "?";
                if (ITEM->status == "Passive")
                    seg.cls = "passive";
                else if (ITEM->status == "NeedsAttention")
                    seg.cls = "needs-attention";
                seg.tooltip = ITEM->tooltipTitle.empty() ? ITEM->title :
                    ITEM->tooltipBody.empty()            ? ITEM->tooltipTitle :
                                                           ITEM->tooltipTitle + "\n" + ITEM->tooltipBody;
                out.emplace_back(std::move(seg));
            }
            return out;
        }

        bool hidden(PHLMONITOR) override {
            return m_items.empty();
        }

        // every tray item is clickable (Activate / ContextMenu / native menu)
        bool clickable(const SSegment&) const override { return true; }

        void onClick(uint32_t button, const SSegment& seg, PHLMONITOR mon) override {
            if (!m_bus || seg.id >= m_items.size())
                return;
            auto* const ITEM = m_items[seg.id].get();
            const auto  POS  = g_pInputManager->getMouseCoordsInternal();

            // Menu items (the common appindicator case): left/right-click renders
            // the item's com.canonical.dbusmenu natively instead of firing
            // Activate/ContextMenu, which these clients ignore.
            if ((button == BTN_LEFT || button == BTN_RIGHT) && !ITEM->menuPath.empty()) {
                openMenu(seg.id, mon, POS.x);
                return;
            }

            const char* METHOD = button == BTN_RIGHT ? "ContextMenu" :
                button == BTN_MIDDLE                 ? "SecondaryActivate" :
                ITEM->itemIsMenu                     ? "ContextMenu" :
                                                       "Activate";
            sd_bus_call_method_async(m_bus, nullptr, ITEM->busName.c_str(), ITEM->objectPath.c_str(), ITEM_IFACE, METHOD, nullptr, nullptr, "ii", (int)POS.x, (int)POS.y);
            DBus::flush(m_bus);
        }

        void onScroll(double delta, const SSegment& seg, PHLMONITOR) override {
            if (!m_bus || seg.id >= m_items.size())
                return;
            auto* const ITEM = m_items[seg.id].get();
            int         d    = (int)delta;
            if (d == 0) // sub-1.0 axis deltas would send a useless Scroll(0)
                d = delta < 0 ? -1 : 1;
            sd_bus_call_method_async(m_bus, nullptr, ITEM->busName.c_str(), ITEM->objectPath.c_str(), ITEM_IFACE, "Scroll", nullptr, nullptr, "is", d, "vertical");
            DBus::flush(m_bus);
        }

        // ---- native dbusmenu popup ----

        bool          popupOpen() const override { return m_popupOpen; }
        PHLMONITORREF popupMonitor() const override { return m_popupMon; }

        void closePopup() override {
            const auto MON = m_popupMon.lock();
            if (m_slotMenuLayout) {
                sd_bus_slot_unref(m_slotMenuLayout);
                m_slotMenuLayout = nullptr;
            }
            m_popupOpen        = false;
            m_popupEntries.clear();
            m_popupHits.clear();
            m_popupHoveredTop  = -1;
            m_popupExpandedTop = -1;
            m_popupHoveredSub  = -1;
            m_popupBusName.clear();
            m_popupMenuPath.clear();
            m_popupMon     = {};
            m_popupAnchorX = 0;
            if (MON)
                g_pHyprRenderer->damageMonitor(MON); // erase the popup
        }

        void popupHandleMotion(const Vector2D& global) override {
            if (!m_popupOpen)
                return;
            const auto MON = m_popupMon.lock();
            if (!MON)
                return;
            const Vector2D L = global - MON->m_position;
            int            newTop = -1, newSub = -1;
            for (const auto& H : m_popupHits) {
                if (!H.box.containsPoint(L))
                    continue;
                if (H.column == 0)
                    newTop = H.index;
                else
                    newSub = H.index;
            }
            if (newTop != m_popupHoveredTop || newSub != m_popupHoveredSub) {
                m_popupHoveredTop = newTop;
                m_popupHoveredSub = newSub;
                g_pHyprRenderer->damageMonitor(MON);
            }
        }

        bool popupHandleButton(uint32_t button, const Vector2D& global) override {
            if (!m_popupOpen)
                return false;
            const auto MON = m_popupMon.lock();
            if (!MON)
                return false;
            const Vector2D L = global - MON->m_position;

            for (const auto& H : m_popupHits) {
                if (!H.box.containsPoint(L))
                    continue;

                const std::vector<SMenuEntry>* entries = nullptr;
                if (H.column == 0)
                    entries = &m_popupEntries;
                else if (m_popupExpandedTop >= 0 && (size_t)m_popupExpandedTop < m_popupEntries.size())
                    entries = &m_popupEntries[m_popupExpandedTop].children;

                if (!entries || (size_t)H.index >= entries->size())
                    return true; // inside the popup but stale: consume, do nothing

                const auto& E = (*entries)[H.index];
                if (!E.visible || E.type == "separator" || !E.enabled)
                    return true; // consume, ignore

                // top-level submenu parent: expand/collapse the second column
                if (E.hasSubmenu && H.column == 0) {
                    m_popupExpandedTop = (m_popupExpandedTop == H.index) ? -1 : (int)H.index;
                    m_popupHoveredSub  = -1;
                    g_pHyprRenderer->damageMonitor(MON);
                    return true;
                }

                // leaf entry: fire Event(clicked) and dismiss
                sendMenuEvent(E.id);
                closePopup();
                return true;
            }
            return false; // click was not inside the popup
        }

        void drawPopup(PHLMONITOR mon) override;

        // called from the file-local GetLayout reply callback
        void handleMenuLayout(sd_bus_message* m) {
            if (m_slotMenuLayout) {
                sd_bus_slot_unref(m_slotMenuLayout);
                m_slotMenuLayout = nullptr;
            }
            if (sd_bus_message_is_method_error(m, nullptr)) {
                fallbackContextMenu();
                return;
            }
            uint32_t rev = 0;
            if (sd_bus_message_read(m, "u", &rev) < 0) {
                fallbackContextMenu();
                return;
            }
            SMenuEntry root;
            if (!parseMenuNode(m, root) || root.children.empty()) {
                fallbackContextMenu();
                return;
            }
            m_popupEntries     = std::move(root.children);
            m_popupHoveredTop  = -1;
            m_popupExpandedTop = -1;
            m_popupHoveredSub  = -1;
            m_popupOpen        = true;
            if (const auto MON = m_popupMon.lock())
                g_pHyprRenderer->damageMonitor(MON);
        }

        // ---- watcher / client plumbing (public: called from file-local sd-bus callbacks) ----

        bool tryBecomeWatcher() {
            if (m_isWatcher)
                return true;
            if (sd_bus_add_object_vtable(m_bus, &m_slotVtable, WATCHER_PATH, WATCHER_IFACE, WATCHER_VTABLE, this) < 0)
                return false;
            if (sd_bus_request_name(m_bus, WATCHER_NAME, 0) < 0) {
                sd_bus_slot_unref(m_slotVtable);
                m_slotVtable = nullptr;
                return false;
            }
            m_isWatcher = true;
            dropClientSlots();
            // be our own host so IsStatusNotifierHostRegistered is honest;
            // register through the bus so the normal code path runs
            if (sd_bus_request_name(m_bus, HOST_NAME, 0) >= 0)
                m_ownsHostName = true;
            sd_bus_call_method_async(m_bus, nullptr, WATCHER_NAME, WATCHER_PATH, WATCHER_IFACE, "RegisterStatusNotifierHost", nullptr, nullptr, "s", HOST_NAME);
            DBus::flush(m_bus);
            return true;
        }

        void enterClientMode() {
            sd_bus_match_signal(m_bus, &m_slotClientItemReg, WATCHER_NAME, WATCHER_PATH, WATCHER_IFACE, "StatusNotifierItemRegistered", onClientItemRegistered, this);
            sd_bus_match_signal(m_bus, &m_slotClientItemUnreg, WATCHER_NAME, WATCHER_PATH, WATCHER_IFACE, "StatusNotifierItemUnregistered", onClientItemUnregistered, this);
            const std::string MATCH = std::string{"type='signal',sender='org.freedesktop.DBus',path='/org/freedesktop/DBus',"
                                                  "interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='"} +
                WATCHER_NAME + "'";
            sd_bus_add_match(m_bus, &m_slotClientWatcherOwner, MATCH.c_str(), onWatcherOwnerChanged, this);
            sd_bus_call_method_async(m_bus, &m_slotClientGetItems, WATCHER_NAME, WATCHER_PATH, PROPS_IFACE, "Get", onClientItemsReply, this, "ss", WATCHER_IFACE,
                                     "RegisteredStatusNotifierItems");
            DBus::flush(m_bus);
        }

        void dropClientSlots() {
            for (auto* slot : {&m_slotClientItemReg, &m_slotClientItemUnreg, &m_slotClientWatcherOwner, &m_slotClientGetItems}) {
                if (*slot) {
                    sd_bus_slot_unref(*slot);
                    *slot = nullptr;
                }
            }
        }

        // (re)install the per-item signal + NameOwnerChanged matches on m_bus.
        // busName is validated (isValidBusName) before an item is ever created,
        // so its use in the match-rule string here is injection-safe.
        void armItemMatches(STrayItem* item) {
            // member = nullptr -> all signals on the item iface (NewIcon, NewStatus, ...)
            sd_bus_match_signal(m_bus, &item->slotSignals, item->busName.c_str(), item->objectPath.c_str(), ITEM_IFACE, nullptr, onItemSignal, item);
            const std::string MATCH = "type='signal',sender='org.freedesktop.DBus',path='/org/freedesktop/DBus',"
                                      "interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='" +
                item->busName + "'";
            sd_bus_add_match(m_bus, &item->slotOwnerChanged, MATCH.c_str(), onItemOwnerChanged, item);
        }

        void addItem(const std::string& busName, const std::string& path) {
            // fix 3: a malformed bus name could break out of the match-rule
            // arg0='...' clause; drop it before it reaches any match string.
            if (!isValidBusName(busName))
                return;

            const std::string SERVICE = busName + path;
            for (auto& it : m_items) {
                if (it->service == SERVICE) {
                    requestGetAll(it.get()); // re-registration -> refresh
                    return;
                }
            }

            // fix 1: cap tracked items so a malicious client can't OOM us by
            // spamming registrations. Existing items still refresh (loop above).
            constexpr size_t MAX_ITEMS = 256;
            if (m_items.size() >= MAX_ITEMS)
                return;

            auto item        = makeUnique<STrayItem>();
            item->owner      = this;
            item->busName    = busName;
            item->objectPath = path;
            item->service    = SERVICE;

            armItemMatches(item.get());

            auto* const RAW = item.get();
            m_items.emplace_back(std::move(item));
            requestGetAll(RAW);

            if (m_isWatcher) {
                sd_bus_emit_signal(m_bus, WATCHER_PATH, WATCHER_IFACE, "StatusNotifierItemRegistered", "s", SERVICE.c_str());
                sd_bus_emit_properties_changed(m_bus, WATCHER_PATH, WATCHER_IFACE, "RegisteredStatusNotifierItems", NULL);
                DBus::flush(m_bus);
            }
            requestRedraw();
        }

        void removeItem(STrayItem* item) {
            const auto IT = std::find_if(m_items.begin(), m_items.end(), [item](const auto& e) { return e.get() == item; });
            if (IT == m_items.end())
                return;
            const std::string SERVICE = item->service;
            m_items.erase(IT); // slots die with the item
            if (m_isWatcher) {
                sd_bus_emit_signal(m_bus, WATCHER_PATH, WATCHER_IFACE, "StatusNotifierItemUnregistered", "s", SERVICE.c_str());
                sd_bus_emit_properties_changed(m_bus, WATCHER_PATH, WATCHER_IFACE, "RegisteredStatusNotifierItems", NULL);
                DBus::flush(m_bus);
            }
            requestRedraw();
        }

        void removeItemByService(const std::string& service) {
            for (auto& it : m_items) {
                if (it->service == service) {
                    removeItem(it.get());
                    return;
                }
            }
        }

        void requestGetAll(STrayItem* item) {
            if (item->slotGetAll) { // coalesce bursts of New* signals
                item->pendingGetAll = true;
                return;
            }
            if (sd_bus_call_method_async(m_bus, &item->slotGetAll, item->busName.c_str(), item->objectPath.c_str(), PROPS_IFACE, "GetAll", onGetAllReply, item, "s", ITEM_IFACE) < 0)
                item->slotGetAll = nullptr;
            DBus::flush(m_bus);
        }

        void applyProps(STrayItem* item, sd_bus_message* m) {
            item->status.clear();
            item->iconName.clear();
            item->attentionIconName.clear();
            item->iconThemePath.clear();
            item->title.clear();
            item->menuPath.clear();
            item->tooltipTitle.clear();
            item->tooltipBody.clear();
            item->itemIsMenu = false;
            item->pixmaps.clear();

            if (sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, "{sv}") <= 0)
                return;
            while (sd_bus_message_enter_container(m, SD_BUS_TYPE_DICT_ENTRY, "sv") > 0) {
                const char* key = nullptr;
                if (sd_bus_message_read(m, "s", &key) < 0)
                    break;
                const std::string KEY = key ? key : "";

                if (KEY == "Status" || KEY == "IconName" || KEY == "AttentionIconName" || KEY == "IconThemePath" || KEY == "Title") {
                    if (sd_bus_message_enter_container(m, SD_BUS_TYPE_VARIANT, "s") > 0) {
                        const char* val = nullptr;
                        sd_bus_message_read(m, "s", &val);
                        sd_bus_message_exit_container(m);
                        const std::string V = val ? val : "";
                        if (KEY == "Status")
                            item->status = V;
                        else if (KEY == "IconName")
                            item->iconName = V;
                        else if (KEY == "AttentionIconName")
                            item->attentionIconName = V;
                        else if (KEY == "IconThemePath")
                            item->iconThemePath = V;
                        else
                            item->title = V;
                    } else
                        sd_bus_message_skip(m, "v");
                } else if (KEY == "Menu") {
                    if (sd_bus_message_enter_container(m, SD_BUS_TYPE_VARIANT, "o") > 0) {
                        const char* val = nullptr;
                        sd_bus_message_read(m, "o", &val);
                        sd_bus_message_exit_container(m);
                        item->menuPath = val ? val : "";
                    } else
                        sd_bus_message_skip(m, "v");
                } else if (KEY == "ItemIsMenu") {
                    if (sd_bus_message_enter_container(m, SD_BUS_TYPE_VARIANT, "b") > 0) {
                        int b = 0;
                        sd_bus_message_read(m, "b", &b);
                        sd_bus_message_exit_container(m);
                        item->itemIsMenu = b != 0;
                    } else
                        sd_bus_message_skip(m, "v");
                } else if (KEY == "ToolTip") {
                    if (sd_bus_message_enter_container(m, SD_BUS_TYPE_VARIANT, "(sa(iiay)ss)") > 0) {
                        if (sd_bus_message_enter_container(m, SD_BUS_TYPE_STRUCT, "sa(iiay)ss") > 0) {
                            const char* iconName = nullptr;
                            sd_bus_message_read(m, "s", &iconName);
                            sd_bus_message_skip(m, "a(iiay)");
                            const char *t = nullptr, *b = nullptr;
                            sd_bus_message_read(m, "ss", &t, &b);
                            sd_bus_message_exit_container(m);
                            item->tooltipTitle = t ? t : "";
                            item->tooltipBody  = b ? b : "";
                        }
                        sd_bus_message_exit_container(m);
                    } else
                        sd_bus_message_skip(m, "v");
                } else if (KEY == "IconPixmap") {
                    if (sd_bus_message_enter_container(m, SD_BUS_TYPE_VARIANT, "a(iiay)") > 0) {
                        if (sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, "(iiay)") > 0) {
                            // fix 1: reject absurd dimensions and cap total retained
                            // pixmap bytes per item so a hostile client cannot pin
                            // hundreds of MB via many/huge pixmaps.
                            constexpr int32_t MAX_PIXMAP_DIM   = 512;
                            constexpr size_t  MAX_PIXMAP_BYTES = 4u * 1024 * 1024;
                            size_t            pixmapBytes      = 0;
                            while (sd_bus_message_enter_container(m, SD_BUS_TYPE_STRUCT, "iiay") > 0) {
                                int32_t w = 0, h = 0;
                                sd_bus_message_read(m, "ii", &w, &h);
                                const void* ptr = nullptr;
                                size_t      sz  = 0;
                                sd_bus_message_read_array(m, 'y', &ptr, &sz);
                                sd_bus_message_exit_container(m);
                                // NordVPN publishes a 0x0 dummy pixmap — skip degenerates.
                                // Keep the exact sz == w*h*4 check; add dim + byte caps.
                                if (w > 0 && h > 0 && w <= MAX_PIXMAP_DIM && h <= MAX_PIXMAP_DIM && ptr && sz == (size_t)w * h * 4 && pixmapBytes + sz <= MAX_PIXMAP_BYTES) {
                                    STrayPixmap pm;
                                    pm.width  = w;
                                    pm.height = h;
                                    pm.data.assign((const uint8_t*)ptr, (const uint8_t*)ptr + sz);
                                    item->pixmaps.emplace_back(std::move(pm));
                                    pixmapBytes += sz;
                                }
                            }
                            sd_bus_message_exit_container(m);
                        }
                        sd_bus_message_exit_container(m);
                    } else
                        sd_bus_message_skip(m, "v");
                } else
                    sd_bus_message_skip(m, "v");

                sd_bus_message_exit_container(m); // dict entry
            }
            sd_bus_message_exit_container(m); // array

            // fix 2: build the icon texture(s) HERE (we are on a D-Bus reply, not
            // in the render pass, so filesystem I/O + createTexture are safe).
            // segments()/iconFor() then become a pure cached lookup.
            rebuildTextures(item);
            requestRedraw();
        }

        void dropClientGetItemsSlot() {
            if (m_slotClientGetItems) {
                sd_bus_slot_unref(m_slotClientGetItems);
                m_slotClientGetItems = nullptr;
            }
        }

        std::vector<UP<STrayItem>> m_items;

      private:
        // fix 2: PURE cached lookup — MUST NOT touch the filesystem or build a
        // texture (segments() calls this every frame). On a miss (a monitor
        // scale that wasn't built in applyProps, e.g. hotplug/rescale) show no
        // icon and schedule a deferred rebuild off the render path.
        SP<Render::ITexture> iconFor(STrayItem* item, int px) {
            if (const auto IT = item->texByPx.find(px); IT != item->texByPx.end())
                return IT->second;
            scheduleTextureRebuild();
            return nullptr;
        }

        // the set of icon px sizes the live monitors actually need (trayIconSize
        // times each monitor's scale). Falls back to the unscaled size if there
        // are no monitors yet, so we never build an empty set.
        std::set<int> neededPxSet() const {
            std::set<int> pxs;
            const double  SIZE = (double)g_cfg.trayIconSize->value();
            for (const auto& MON : g_pCompositor->m_monitors) {
                if (MON)
                    pxs.insert(std::max(1, (int)std::lround(SIZE * MON->m_scale)));
            }
            if (pxs.empty())
                pxs.insert(std::max(1, (int)std::lround(SIZE)));
            return pxs;
        }

        // build+cache any missing px in `pxs` for one item (cached even on
        // failure, so a bad icon is not retried every frame). Never called from
        // the render pass.
        void buildTexturesFor(STrayItem* item, const std::set<int>& pxs) {
            for (const int PX : pxs) {
                if (item->texByPx.find(PX) == item->texByPx.end())
                    item->texByPx[PX] = buildIcon(item, PX);
            }
        }

        // icon changed (applyProps): drop stale textures and rebuild for the
        // sizes currently on screen.
        void rebuildTextures(STrayItem* item) {
            item->texByPx.clear();
            buildTexturesFor(item, neededPxSet());
        }

        // deferred, coalesced rebuild for a render-time cache miss. Runs on the
        // main thread OUTSIDE the render pass (via MainThread::post), guarded by
        // m_alive so a destroyed module is a safe no-op.
        void scheduleTextureRebuild() {
            if (m_rebuildPosted)
                return;
            m_rebuildPosted = true;
            MainThread::post([this, weak = WP<bool>(m_alive)] {
                if (!weak.lock())
                    return;
                m_rebuildPosted = false;
                const auto PXS  = neededPxSet();
                for (auto& item : m_items)
                    buildTexturesFor(item.get(), PXS);
                requestRedraw();
            });
        }

        SP<Render::ITexture> buildIcon(STrayItem* item, int px) {
            if (px < 1)
                return nullptr;
            cairo_surface_t* surf = nullptr;
            if (const auto FILE = findIconFile(item, px); !FILE.empty())
                surf = FILE.ends_with(".svg") ? surfaceFromSVG(FILE, px) : surfaceFromPNG(FILE, px);
            if (!surf && !item->pixmaps.empty()) {
                const STrayPixmap* best = nullptr;
                for (const auto& PM : item->pixmaps) {
                    if (!best || std::abs(PM.width - px) < std::abs(best->width - px))
                        best = &PM;
                }
                surf = surfaceFromPixmap(*best, px);
            }
            if (!surf)
                return nullptr;
            cairo_surface_flush(surf);
            auto tex = g_pHyprRenderer->createTexture(surf); // safe outside the render pass
            cairo_surface_destroy(surf);
            return tex;
        }

        // full index.theme parsing deliberately skipped: fixed-size + scalable
        // dirs across the common freedesktop contexts cover real tray icons
        // (app icons live in apps/, bluetooth/network status icons in status/,
        // etc.), plus the -symbolic variant many status icons ship only as.
        std::string findIconFile(const STrayItem* item, int px) {
            // NeedsAttention prefers the attention icon when the item provides one
            std::string name = item->iconName;
            if (item->status == "NeedsAttention" && !item->attentionIconName.empty())
                name = item->attentionIconName;
            if (name.empty())
                return "";

            const auto EXISTS = [](const std::string& p) {
                std::error_code ec;
                return std::filesystem::is_regular_file(p, ec);
            };

            // absolute path: some items publish a full file path as the icon name
            if (name[0] == '/') {
                if (EXISTS(name))
                    return name;
                return "";
            }

            // fix 5: a RELATIVE icon name must be a bare basename. It is
            // wire-supplied (IconName) and gets concatenated onto trusted icon
            // directory prefixes below; a name like "../../etc/shadow" or one
            // containing '/' could otherwise steer us to open files outside the
            // icon dirs. Constrain it to a plain filename.
            if (name.find('/') != std::string::npos || name.find("..") != std::string::npos)
                return "";

            static const char* EXTS[] = {".png", ".svg"};
            static const char* CATS[] = {"apps", "status", "devices", "panel", "actions"};

            // try the name as given, then its -symbolic variant
            std::vector<std::string> names;
            names.emplace_back(name);
            if (!name.ends_with("-symbolic"))
                names.emplace_back(name + "-symbolic");

            // 1) the item's own IconThemePath (a flat dir of icon files)
            if (!item->iconThemePath.empty()) {
                for (const auto& N : names)
                    for (const auto* EXT : EXTS)
                        if (const auto P = item->iconThemePath + "/" + N + EXT; EXISTS(P))
                            return P;
            }

            std::vector<std::string> themes;
            if (const auto CFG = g_cfg.iconTheme->value(); !CFG.empty())
                themes.emplace_back(CFG);
            themes.emplace_back("hicolor");

            std::vector<std::string> bases;
            if (const char* HOME = getenv("HOME"); HOME && *HOME) {
                bases.emplace_back(std::string{HOME} + "/.icons");
                bases.emplace_back(std::string{HOME} + "/.local/share/icons");
            }
            bases.emplace_back("/usr/share/icons");

            const int SIZES[] = {px, 22, 24, 32, 48, 64, 128, 16, 256};
            // prefer the exact name across everything before falling back to
            // -symbolic (outer loop over names)
            for (const auto& N : names) {
                for (const auto& THEME : themes) {
                    for (const auto& BASE : bases) {
                        const auto ROOT = BASE + "/" + THEME + "/";
                        for (const int S : SIZES) {
                            const auto SD = ROOT + std::to_string(S) + "x" + std::to_string(S) + "/";
                            for (const auto* CAT : CATS) {
                                for (const auto* EXT : EXTS)
                                    if (const auto P = SD + CAT + "/" + N + EXT; EXISTS(P))
                                        return P;
                            }
                        }
                        // scalable/ and symbolic/ size-dirs (GNOME ships symbolic
                        // status icons, e.g. bluetooth-active-symbolic, under symbolic/)
                        for (const auto* SIZEDIR : {"scalable", "symbolic"})
                            for (const auto* CAT : CATS)
                                if (const auto P = ROOT + SIZEDIR + "/" + CAT + "/" + N + ".svg"; EXISTS(P))
                                    return P;
                    }
                }
            }

            for (const auto& N : names)
                for (const auto* EXT : EXTS)
                    if (const auto P = std::string{"/usr/share/pixmaps/"} + N + EXT; EXISTS(P))
                        return P;

            return "";
        }

        // ---- popup helpers (private) ----

        // fetch the item's dbusmenu layout and (on reply) open the popup
        void openMenu(size_t idx, PHLMONITOR mon, double globalX) {
            if (!m_bus || idx >= m_items.size() || !mon)
                return;
            auto* const ITEM = m_items[idx].get();
            if (ITEM->menuPath.empty())
                return;

            closePopup(); // drop any prior popup + its in-flight slot

            m_popupBusName  = ITEM->busName;
            m_popupMenuPath = ITEM->menuPath;
            m_popupMon      = mon;
            m_popupAnchorX  = globalX;

            // AboutToShow(0): fire-and-forget; lets the client populate the menu
            sd_bus_call_method_async(m_bus, nullptr, m_popupBusName.c_str(), m_popupMenuPath.c_str(), MENU_IFACE, "AboutToShow", nullptr, nullptr, "i", 0);

            // GetLayout(parentId=0, depth=-1, props=[...]) -> u(ia{sv}av)
            sd_bus_message* msg = nullptr;
            if (sd_bus_message_new_method_call(m_bus, &msg, m_popupBusName.c_str(), m_popupMenuPath.c_str(), MENU_IFACE, "GetLayout") < 0) {
                fallbackContextMenu();
                return;
            }
            bool ok = sd_bus_message_append(msg, "ii", 0, -1) >= 0;
            ok      = ok && sd_bus_message_open_container(msg, SD_BUS_TYPE_ARRAY, "s") >= 0;
            for (const char* P : {"label", "enabled", "visible", "type", "toggle-type", "toggle-state", "children-display"})
                ok = ok && sd_bus_message_append(msg, "s", P) >= 0;
            ok = ok && sd_bus_message_close_container(msg) >= 0;
            if (!ok || sd_bus_call_async(m_bus, &m_slotMenuLayout, msg, onMenuLayout, this, 0) < 0) {
                sd_bus_message_unref(msg);
                fallbackContextMenu();
                return;
            }
            sd_bus_message_unref(msg);
            DBus::flush(m_bus);
        }

        // Event(i id, s "clicked", v data, u timestamp) on the dbusmenu iface
        void sendMenuEvent(int32_t id) {
            if (!m_bus || m_popupBusName.empty() || m_popupMenuPath.empty())
                return;
            sd_bus_message* msg = nullptr;
            if (sd_bus_message_new_method_call(m_bus, &msg, m_popupBusName.c_str(), m_popupMenuPath.c_str(), MENU_IFACE, "Event") < 0)
                return;
            bool ok = sd_bus_message_append(msg, "is", id, "clicked") >= 0;
            ok      = ok && sd_bus_message_open_container(msg, SD_BUS_TYPE_VARIANT, "y") >= 0;
            ok      = ok && sd_bus_message_append(msg, "y", (uint8_t)0) >= 0;
            ok      = ok && sd_bus_message_close_container(msg) >= 0;
            ok      = ok && sd_bus_message_append(msg, "u", (uint32_t)0) >= 0;
            if (ok)
                sd_bus_call_async(m_bus, nullptr, msg, nullptr, nullptr, 0);
            sd_bus_message_unref(msg);
            DBus::flush(m_bus);
        }

        // any menu failure: dismiss the popup and send the old ContextMenu so a
        // click is never simply swallowed with nothing happening
        void fallbackContextMenu() {
            const std::string BUS = m_popupBusName;
            closePopup(); // clears the popup members (incl. m_popupBusName)
            if (!m_bus || BUS.empty())
                return;
            for (auto& it : m_items) {
                if (it->busName != BUS)
                    continue;
                const auto POS = g_pInputManager->getMouseCoordsInternal();
                sd_bus_call_method_async(m_bus, nullptr, it->busName.c_str(), it->objectPath.c_str(), ITEM_IFACE, "ContextMenu", nullptr, nullptr, "ii", (int)POS.x, (int)POS.y);
                DBus::flush(m_bus);
                return;
            }
        }

        // fix 6: the session bus reconnected. The old bus object and EVERY slot
        // we added on it are dead; rebuild all of them on the fresh connection.
        // We keep the item list (state we still want) and re-arm its matches.
        void reattach() {
            sd_bus* bus = DBus::session();
            if (!bus)
                return; // bus still down; the next reconnect notify will retry

            // Drop every slot bound to the old connection. Our owned m_bus ref
            // still keeps the old bus object alive, so these unrefs are safe.
            closePopup(); // also unrefs any in-flight GetLayout slot
            dropClientSlots();
            if (m_slotVtable)
                m_slotVtable = sd_bus_slot_unref(m_slotVtable);
            for (auto& item : m_items) {
                item->slotSignals      = sd_bus_slot_unref(item->slotSignals);
                item->slotOwnerChanged = sd_bus_slot_unref(item->slotOwnerChanged);
                item->slotGetAll       = sd_bus_slot_unref(item->slotGetAll);
                item->pendingGetAll    = false;
            }

            // Swap our owned ref to the fresh connection (release the old one).
            sd_bus_unref(m_bus);
            m_bus = sd_bus_ref(bus);

            // Re-establish our role on the new bus. The old name grants died with
            // the old connection; reset the flags and re-request from scratch.
            m_isWatcher    = false;
            m_ownsHostName = false;
            if (!tryBecomeWatcher())
                enterClientMode();

            // Re-arm each known item's matches + refetch its props on the new bus.
            for (auto& item : m_items) {
                armItemMatches(item.get());
                requestGetAll(item.get());
            }
            DBus::flush(m_bus);
        }

        sd_bus* m_bus          = nullptr;
        bool    m_isWatcher    = false;
        bool    m_ownsHostName = false;

        sd_bus_slot* m_slotVtable            = nullptr;
        // client mode (another watcher owns the name)
        sd_bus_slot* m_slotClientItemReg     = nullptr;
        sd_bus_slot* m_slotClientItemUnreg   = nullptr;
        sd_bus_slot* m_slotClientWatcherOwner = nullptr;
        sd_bus_slot* m_slotClientGetItems    = nullptr;

        // ---- popup state ----
        bool                    m_popupOpen = false;
        PHLMONITORREF           m_popupMon;
        double                  m_popupAnchorX = 0; // global logical x of the clicked icon
        std::string             m_popupBusName;     // captured so item churn can't dangle it
        std::string             m_popupMenuPath;
        std::vector<SMenuEntry> m_popupEntries;     // top-level menu items
        std::vector<SMenuHit>   m_popupHits;        // rebuilt each drawPopup()
        int                     m_popupHoveredTop  = -1;
        int                     m_popupExpandedTop = -1; // index of the expanded top entry
        int                     m_popupHoveredSub  = -1;
        sd_bus_slot*            m_slotMenuLayout   = nullptr; // in-flight GetLayout

        // fix 2: coalesces render-miss texture rebuilds posted to the main thread.
        bool     m_rebuildPosted = false;

        // fix 6: liveness guard for the DBus reconnect callback + any posted
        // texture rebuild. Both live beyond a single call; this module is
        // destroyed+rebuilt on config reload, so they weak-capture this guard and
        // no-op once the module (and this SP) are gone.
        SP<bool> m_alive = makeShared<bool>(true);
    };

    // ---- callback bodies ----

    int onRegisterItem(sd_bus_message* m, void* userdata, sd_bus_error*) {
        auto* const self = static_cast<CTrayModule*>(userdata);
        const char* arg  = nullptr;
        if (sd_bus_message_read(m, "s", &arg) < 0)
            return -EINVAL;

        std::string busName, path;
        if (arg && arg[0] == '/') {
            // path form: the item's bus name is the caller
            const char* SENDER = sd_bus_message_get_sender(m);
            busName            = SENDER ? SENDER : "";
            path               = arg;
        } else if (arg && arg[0]) {
            busName = arg;
            path    = "/StatusNotifierItem";
        }
        // fix 3: validate here too so a bad name never reaches addItem / a match
        // rule (addItem re-checks; this keeps the entry point honest).
        if (isValidBusName(busName))
            self->addItem(busName, path);
        return sd_bus_reply_method_return(m, NULL);
    }

    int onRegisterHost(sd_bus_message* m, void*, sd_bus_error*) {
        // hosts are not tracked individually; IsStatusNotifierHostRegistered is
        // hardcoded true (we are always our own host)
        const char* arg = nullptr;
        sd_bus_message_read(m, "s", &arg);
        sd_bus_emit_signal(sd_bus_message_get_bus(m), WATCHER_PATH, WATCHER_IFACE, "StatusNotifierHostRegistered", "");
        return sd_bus_reply_method_return(m, NULL);
    }

    int propItems(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void* userdata, sd_bus_error*) {
        auto* const self = static_cast<CTrayModule*>(userdata);
        if (const auto R = sd_bus_message_open_container(reply, 'a', "s"); R < 0)
            return R;
        for (const auto& IT : self->m_items)
            sd_bus_message_append(reply, "s", IT->service.c_str());
        return sd_bus_message_close_container(reply);
    }

    int propHostRegistered(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void*, sd_bus_error*) {
        return sd_bus_message_append(reply, "b", 1);
    }

    int propProtocolVersion(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void*, sd_bus_error*) {
        return sd_bus_message_append(reply, "i", 0);
    }

    int onItemSignal(sd_bus_message* m, void* userdata, sd_bus_error*) {
        auto* const item   = static_cast<STrayItem*>(userdata);
        const char* MEMBER = sd_bus_message_get_member(m);
        // NewIcon / NewStatus / NewToolTip / NewTitle / NewIconThemePath / ...
        if (MEMBER && std::strncmp(MEMBER, "New", 3) == 0)
            item->owner->requestGetAll(item);
        return 0;
    }

    int onItemOwnerChanged(sd_bus_message* m, void* userdata, sd_bus_error*) {
        auto* const item = static_cast<STrayItem*>(userdata);
        const char *name = nullptr, *oldOwner = nullptr, *newOwner = nullptr;
        if (sd_bus_message_read(m, "sss", &name, &oldOwner, &newOwner) < 0)
            return 0;
        if (!newOwner || !*newOwner)
            item->owner->removeItem(item); // destroys this slot: safe, sd-bus refs it during dispatch
        return 0;
    }

    int onGetAllReply(sd_bus_message* m, void* userdata, sd_bus_error*) {
        auto* const item = static_cast<STrayItem*>(userdata);
        auto* const self = item->owner;
        if (item->slotGetAll) {
            sd_bus_slot_unref(item->slotGetAll);
            item->slotGetAll = nullptr;
        }
        if (!sd_bus_message_is_method_error(m, nullptr))
            self->applyProps(item, m);
        if (item->pendingGetAll) {
            item->pendingGetAll = false;
            self->requestGetAll(item);
        }
        return 0;
    }

    int onClientItemRegistered(sd_bus_message* m, void* userdata, sd_bus_error*) {
        auto* const self = static_cast<CTrayModule*>(userdata);
        const char* svc  = nullptr;
        if (sd_bus_message_read(m, "s", &svc) < 0 || !svc)
            return 0;
        const auto [BUS, PATH] = parseService(svc);
        if (!BUS.empty())
            self->addItem(BUS, PATH);
        return 0;
    }

    int onClientItemUnregistered(sd_bus_message* m, void* userdata, sd_bus_error*) {
        auto* const self = static_cast<CTrayModule*>(userdata);
        const char* svc  = nullptr;
        if (sd_bus_message_read(m, "s", &svc) < 0 || !svc)
            return 0;
        const auto [BUS, PATH] = parseService(svc);
        self->removeItemByService(BUS + PATH);
        self->removeItemByService(svc); // in case the watcher emitted a raw registry string
        return 0;
    }

    int onClientItemsReply(sd_bus_message* m, void* userdata, sd_bus_error*) {
        auto* const self = static_cast<CTrayModule*>(userdata);
        self->dropClientGetItemsSlot();
        if (sd_bus_message_is_method_error(m, nullptr))
            return 0;
        if (sd_bus_message_enter_container(m, SD_BUS_TYPE_VARIANT, "as") <= 0)
            return 0;
        if (sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, "s") <= 0)
            return 0;
        const char* svc = nullptr;
        while (sd_bus_message_read(m, "s", &svc) > 0) {
            const auto [BUS, PATH] = parseService(svc ? svc : "");
            if (!BUS.empty())
                self->addItem(BUS, PATH);
        }
        sd_bus_message_exit_container(m);
        sd_bus_message_exit_container(m);
        return 0;
    }

    int onWatcherOwnerChanged(sd_bus_message* m, void* userdata, sd_bus_error*) {
        auto* const self = static_cast<CTrayModule*>(userdata);
        const char *name = nullptr, *oldOwner = nullptr, *newOwner = nullptr;
        if (sd_bus_message_read(m, "sss", &name, &oldOwner, &newOwner) < 0)
            return 0;
        if (!newOwner || !*newOwner)
            self->tryBecomeWatcher(); // frees the client slots (incl. this one) on success
        return 0;
    }

    int onMenuLayout(sd_bus_message* m, void* userdata, sd_bus_error*) {
        static_cast<CTrayModule*>(userdata)->handleMenuLayout(m);
        return 0;
    }

    // ---- popup rendering (GL current; called from CPopupPassElement::draw) ----

    void CTrayModule::drawPopup(PHLMONITOR mon) {
        if (!mon || !m_popupOpen || m_popupEntries.empty())
            return;
        m_popupHits.clear();

        const double SCALE       = mon->m_scale;
        const double fontLogical = (double)g_cfg.fontSize->value();
        const int    FONTPX      = std::max(1, (int)std::round(fontLogical * SCALE));
        const double PAD_X = 12, PAD_Y = 6, GUTTER = 18, ARROWW = 14;
        const double ROW_H    = std::max(fontLogical + 10.0, 22.0);
        const double SEP_H    = 7;
        const int    ROUNDING = (int)std::round(g_cfg.rounding->value() * SCALE);

        const double MARGIN  = g_cfg.margin->value();
        const double HEIGHT  = g_cfg.height->value();
        const bool   BOTTOM  = g_cfg.position->value() == "bottom";
        const double BAR_TOP = BOTTOM ? mon->m_size.y - MARGIN - HEIGHT : MARGIN;
        const double GAP     = 4;

        const CHyprColor BG   = cfgColor(g_cfg.colTooltipBg);
        const CHyprColor BORD = cfgColor(g_cfg.colBorder);
        const CHyprColor FG   = cfgColor(g_cfg.colTooltipFg);
        const CHyprColor FGB  = cfgColor(g_cfg.colForegroundBright);
        const CHyprColor HL   = cfgColor(g_cfg.colAccent).modifyA(0.30F);

        // measure a column: fill base textures + row heights, return content width
        auto measure = [&](const std::vector<SMenuEntry>& entries, std::vector<SP<Render::ITexture>>& tex, std::vector<double>& rowH) -> double {
            tex.assign(entries.size(), nullptr);
            rowH.assign(entries.size(), 0.0);
            double maxc = 0;
            for (size_t i = 0; i < entries.size(); ++i) {
                const auto& E = entries[i];
                if (!E.visible)
                    continue;
                if (E.type == "separator") {
                    rowH[i] = SEP_H;
                    continue;
                }
                rowH[i]            = ROW_H;
                // base color is always FG (disabled dimmed via texture alpha at
                // draw); keeps enabled/disabled labels sharing one cache entry
                auto         t  = TextCache::get(E.label.empty() ? std::string(" ") : E.label, FG, FONTPX, false);
                tex[i]         = t;
                const double tw = (t && t->m_texID) ? t->m_size.x / SCALE : 0.0;
                maxc           = std::max(maxc, GUTTER + tw + ARROWW);
            }
            return maxc;
        };

        // draw a column at (ox,oy) size (w,h); record hit boxes tagged `column`
        auto drawColumn = [&](const std::vector<SMenuEntry>& entries, const std::vector<SP<Render::ITexture>>& tex, const std::vector<double>& rowH, double ox, double oy,
                              double w, double h, int column, int hovered) {
            const CBox PIX = CBox{ox, oy, w, h}.scale(SCALE).round();
            if (PIX.w < 1 || PIX.h < 1)
                return;
            g_pHyprOpenGL->renderRect(PIX, BG, {.round = ROUNDING, .roundingPower = 2.F});
            g_pHyprOpenGL->renderBorder(PIX, Config::CGradientValueData{BORD}, {.round = ROUNDING, .roundingPower = 2.F, .borderSize = std::max(1, (int)std::round(SCALE))});

            double ry = oy + PAD_Y;
            for (size_t i = 0; i < entries.size(); ++i) {
                const auto& E = entries[i];
                if (!E.visible || rowH[i] <= 0)
                    continue;
                const double RH = rowH[i];

                if (E.type == "separator") {
                    const CBox line = CBox{ox + PAD_X, ry + RH / 2.0, w - 2 * PAD_X, 1.0}.scale(SCALE).round();
                    if (line.w >= 1 && line.h >= 1)
                        g_pHyprOpenGL->renderRect(line, BORD, {});
                    ry += RH;
                    continue;
                }

                m_popupHits.push_back(SMenuHit{CBox{ox, ry, w, RH}, column, (int)i});

                const bool HOV = hovered == (int)i && E.enabled;
                if (HOV) {
                    const CBox hl = CBox{ox + 2, ry, w - 4, RH}.scale(SCALE).round();
                    if (hl.w >= 1 && hl.h >= 1)
                        g_pHyprOpenGL->renderRect(hl, HL, {.round = std::max(0, ROUNDING / 2), .roundingPower = 2.F});
                }

                // toggle state marker in the left gutter
                if ((E.toggleType == "checkmark" || E.toggleType == "radio") && E.toggleState == 1) {
                    const double CS = std::max(4.0, fontLogical * 0.45);
                    const double mx = ox + PAD_X + (GUTTER - CS) / 2.0;
                    const double my = ry + (RH - CS) / 2.0;
                    const CBox   mk = CBox{mx, my, CS, CS}.scale(SCALE).round();
                    const int    mr = E.toggleType == "radio" ? (int)std::round(CS * SCALE / 2.0) : std::max(1, (int)std::round(2 * SCALE));
                    if (mk.w >= 1 && mk.h >= 1)
                        g_pHyprOpenGL->renderRect(mk, E.enabled ? FGB : FG, {.round = mr, .roundingPower = 2.F});
                }

                // label (bright when hovered)
                auto t = HOV ? TextCache::get(E.label.empty() ? std::string(" ") : E.label, FGB, FONTPX, false) : tex[i];
                if (t && t->m_texID) {
                    const double tx = ox + PAD_X + GUTTER;
                    const CBox   tb = {std::round(tx * SCALE), std::round((ry + (RH - t->m_size.y / SCALE) / 2.0) * SCALE), (double)t->m_size.x, (double)t->m_size.y};
                    if (tb.w >= 1 && tb.h >= 1)
                        g_pHyprOpenGL->renderTexture(t, tb, {.a = E.enabled ? 1.F : 0.5F}); // dim disabled
                }

                // submenu arrow
                if (E.hasSubmenu) {
                    auto a = TextCache::get(std::string("›"), HOV ? FGB : FG, FONTPX, false);
                    if (a && a->m_texID) {
                        const double ax = ox + w - PAD_X - a->m_size.x / SCALE;
                        const CBox   ab = {std::round(ax * SCALE), std::round((ry + (RH - a->m_size.y / SCALE) / 2.0) * SCALE), (double)a->m_size.x, (double)a->m_size.y};
                        if (ab.w >= 1 && ab.h >= 1)
                            g_pHyprOpenGL->renderTexture(a, ab, {.a = E.enabled ? 1.F : 0.5F});
                    }
                }

                ry += RH;
            }
        };

        // top-level column
        std::vector<SP<Render::ITexture>> topTex;
        std::vector<double>               topRowH;
        const double                      topContent = measure(m_popupEntries, topTex, topRowH);
        const double                      topW       = std::clamp(topContent + 2 * PAD_X, 150.0, 460.0);
        double                            topH       = 2 * PAD_Y;
        for (double rh : topRowH)
            topH += rh;

        const double localAnchorX = m_popupAnchorX - mon->m_position.x;
        const double topX         = std::clamp(localAnchorX - topW / 2.0, 0.0, std::max(0.0, mon->m_size.x - topW));
        double       topY         = BOTTOM ? BAR_TOP - GAP - topH : BAR_TOP + HEIGHT + GAP;
        topY                      = std::clamp(topY, 0.0, std::max(0.0, mon->m_size.y - topH));

        drawColumn(m_popupEntries, topTex, topRowH, topX, topY, topW, topH, 0, m_popupHoveredTop);

        // one submenu column
        if (m_popupExpandedTop >= 0 && (size_t)m_popupExpandedTop < m_popupEntries.size()) {
            const auto& PE = m_popupEntries[m_popupExpandedTop];
            if (PE.hasSubmenu && !PE.children.empty()) {
                std::vector<SP<Render::ITexture>> subTex;
                std::vector<double>               subRowH;
                const double                      subContent = measure(PE.children, subTex, subRowH);
                const double                      subW       = std::clamp(subContent + 2 * PAD_X, 150.0, 460.0);
                double                            subH       = 2 * PAD_Y;
                for (double rh : subRowH)
                    subH += rh;

                const double subX = (topX + topW + subW <= mon->m_size.x) ? topX + topW : std::max(0.0, topX - subW);
                double       ry   = topY + PAD_Y;
                for (int i = 0; i < m_popupExpandedTop; ++i)
                    ry += topRowH[i];
                const double subY = std::clamp(ry, 0.0, std::max(0.0, mon->m_size.y - subH));

                drawColumn(PE.children, subTex, subRowH, subX, subY, subW, subH, 1, m_popupHoveredSub);
            }
        }
    }

} // namespace

UP<IModule> makeTrayModule(const SModuleConfig& cfg) {
    return makeUnique<CTrayModule>(cfg);
}
