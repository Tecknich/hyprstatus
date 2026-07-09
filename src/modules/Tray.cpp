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
#include <string>
#include <vector>

#define WLR_USE_UNSTABLE
#include <hyprland/src/helpers/Monitor.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/render/Renderer.hpp>

#include "../globals.hpp"
#include "../services/DBus.hpp"

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

        std::string              status, iconName, iconThemePath, title, menuPath;
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

        void onClick(uint32_t button, const SSegment& seg, PHLMONITOR) override {
            if (!m_bus || seg.id >= m_items.size())
                return;
            auto* const ITEM   = m_items[seg.id].get();
            const char* METHOD = button == BTN_RIGHT ? "ContextMenu" :
                button == BTN_MIDDLE                 ? "SecondaryActivate" :
                ITEM->itemIsMenu                     ? "ContextMenu" :
                                                       "Activate";
            const auto  POS    = g_pInputManager->getMouseCoordsInternal();
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

        void addItem(const std::string& busName, const std::string& path) {
            const std::string SERVICE = busName + path;
            for (auto& it : m_items) {
                if (it->service == SERVICE) {
                    requestGetAll(it.get()); // re-registration -> refresh
                    return;
                }
            }

            auto item        = makeUnique<STrayItem>();
            item->owner      = this;
            item->busName    = busName;
            item->objectPath = path;
            item->service    = SERVICE;

            // member = nullptr -> all signals on the item iface (NewIcon, NewStatus, ...)
            sd_bus_match_signal(m_bus, &item->slotSignals, busName.c_str(), path.c_str(), ITEM_IFACE, nullptr, onItemSignal, item.get());
            const std::string MATCH = "type='signal',sender='org.freedesktop.DBus',path='/org/freedesktop/DBus',"
                                      "interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='" +
                busName + "'";
            sd_bus_add_match(m_bus, &item->slotOwnerChanged, MATCH.c_str(), onItemOwnerChanged, item.get());

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

                if (KEY == "Status" || KEY == "IconName" || KEY == "IconThemePath" || KEY == "Title") {
                    if (sd_bus_message_enter_container(m, SD_BUS_TYPE_VARIANT, "s") > 0) {
                        const char* val = nullptr;
                        sd_bus_message_read(m, "s", &val);
                        sd_bus_message_exit_container(m);
                        const std::string V = val ? val : "";
                        if (KEY == "Status")
                            item->status = V;
                        else if (KEY == "IconName")
                            item->iconName = V;
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
                            while (sd_bus_message_enter_container(m, SD_BUS_TYPE_STRUCT, "iiay") > 0) {
                                int32_t w = 0, h = 0;
                                sd_bus_message_read(m, "ii", &w, &h);
                                const void* ptr = nullptr;
                                size_t      sz  = 0;
                                sd_bus_message_read_array(m, 'y', &ptr, &sz);
                                sd_bus_message_exit_container(m);
                                // NordVPN publishes a 0x0 dummy pixmap — skip degenerates
                                if (w > 0 && h > 0 && ptr && sz == (size_t)w * h * 4) {
                                    STrayPixmap pm;
                                    pm.width  = w;
                                    pm.height = h;
                                    pm.data.assign((const uint8_t*)ptr, (const uint8_t*)ptr + sz);
                                    item->pixmaps.emplace_back(std::move(pm));
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

            item->texByPx.clear(); // icon may have changed
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
        SP<Render::ITexture> iconFor(STrayItem* item, int px) {
            if (const auto IT = item->texByPx.find(px); IT != item->texByPx.end())
                return IT->second;
            auto tex = buildIcon(item, px); // one-time I/O per (item, px); cached even on failure
            item->texByPx[px] = tex;
            return tex;
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

        // full index.theme parsing deliberately skipped in v1: fixed-size dirs
        // + scalable/apps cover hicolor app icons in practice
        std::string findIconFile(const STrayItem* item, int px) {
            if (item->iconName.empty())
                return "";
            const auto& NAME   = item->iconName;
            const auto  EXISTS = [](const std::string& p) {
                std::error_code ec;
                return std::filesystem::is_regular_file(p, ec);
            };

            static const char* EXTS[] = {".png", ".svg"};

            if (!item->iconThemePath.empty()) {
                for (const auto* EXT : EXTS) {
                    if (const auto P = item->iconThemePath + "/" + NAME + EXT; EXISTS(P))
                        return P;
                }
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

            const int SIZES[] = {px, 22, 24, 32, 48, 64, 128};
            for (const auto& THEME : themes) {
                for (const auto& BASE : bases) {
                    const auto ROOT = BASE + "/" + THEME + "/";
                    for (const int S : SIZES) {
                        const auto DIR = ROOT + std::to_string(S) + "x" + std::to_string(S) + "/apps/";
                        for (const auto* EXT : EXTS) {
                            if (const auto P = DIR + NAME + EXT; EXISTS(P))
                                return P;
                        }
                    }
                    if (const auto P = ROOT + "scalable/apps/" + NAME + ".svg"; EXISTS(P))
                        return P;
                }
            }

            for (const auto* EXT : EXTS) {
                if (const auto P = std::string{"/usr/share/pixmaps/"} + NAME + EXT; EXISTS(P))
                    return P;
            }
            return "";
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
        if (!busName.empty())
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

} // namespace

UP<IModule> makeTrayModule(const SModuleConfig& cfg) {
    return makeUnique<CTrayModule>(cfg);
}
