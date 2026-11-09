#define WLR_USE_UNSTABLE
#include "ModuleConfig.hpp"
#include "../globals.hpp"

#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprutils/string/String.hpp>

#include <cctype>
#include <set>

// Hyprland 0.57 removed the hyprlang (.conf) provider: Lua is the ONLY way
// module options arrive. Fail the build loudly instead of shipping a plugin
// that loads fine but silently ignores every hyprstatus.module/set call.
#if !__has_include(<lua.h>) || !__has_include(<lauxlib.h>)
#error "hyprstatus needs the Lua headers (lua.h/lauxlib.h): Hyprland >= 0.57 configures plugins only through Lua"
#endif
extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

namespace {
    std::map<std::string, SModuleConfig> store;
    std::set<std::string>                declaredCustoms;

    // parked by beginReload() so a reload that never ran can be undone
    std::map<std::string, SModuleConfig> parkedStore;
    std::set<std::string>                parkedCustoms;
    bool                                 luaCalledThisReload = false;

    bool validName(const std::string& s) {
        if (s.empty())
            return false;
        for (const char C : s) {
            if (!std::isalnum((unsigned char)C) && C != '_' && C != '-')
                return false;
        }
        return true;
    }

    SModuleConfig& ensureEntry(const std::string& name) {
        auto& entry = store[name];
        entry.name  = name;
        return entry;
    }
}

// lua backend -----------------------------------------------------------------

// luaL_error / luaL_check* longjmp out of this C function (Lua is built as C),
// skipping C++ destructors. So no std::string may be alive when one fires:
// read every argument with the raw C API first, then do the C++ work in a
// separate scope and report failure with a static message afterwards.
static int luaModule(lua_State* L) {
    const char* RAW = luaL_checkstring(L, 1); // may longjmp: nothing C++ alive yet
    bool        ok  = false;
    {
        const auto NAME = Hyprutils::String::trim(std::string{RAW});
        if (validName(NAME)) {
            declaredCustoms.insert(NAME);
            ensureEntry(NAME);
            ModuleConfigStore::noteLuaCall();
            ok = true;
        }
    }
    if (!ok)
        return luaL_error(L, "hyprstatus.module: invalid module name (allowed: [A-Za-z0-9_-]+)");
    return 0;
}

static int luaSet(lua_State* L) {
    const char* RAWNAME = luaL_checkstring(L, 1);
    const char* RAWKEY  = luaL_checkstring(L, 2);
    const char* VALUE   = luaL_checkstring(L, 3);
    bool        ok      = false;
    {
        const auto NAME = Hyprutils::String::trim(std::string{RAWNAME});
        const auto KEY  = Hyprutils::String::trim(std::string{RAWKEY});
        if (!NAME.empty() && !KEY.empty()) {
            // lua string literals already carry real newlines; no unescaping
            ensureEntry(NAME).opts[KEY] = VALUE;
            ModuleConfigStore::noteLuaCall();
            ok = true;
        }
    }
    if (!ok)
        return luaL_error(L, "hyprstatus.set(module, key, value): module and key must be non-empty");
    return 0;
}

// public API -------------------------------------------------------------------

void ModuleConfigStore::registerKeywords() {
    // 0.57 configures plugins only through Lua: hl.plugin.hyprstatus.module /
    // hl.plugin.hyprstatus.set (the hyprstatus-module / hyprstatus-set keywords
    // went away with the hyprlang provider).
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprstatus", "module", ::luaModule);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprstatus", "set", ::luaSet);
}

void ModuleConfigStore::beginReload() {
    parkedStore         = std::move(store);
    parkedCustoms       = std::move(declaredCustoms);
    store               = {};
    declaredCustoms     = {};
    luaCalledThisReload = false;
}

void ModuleConfigStore::noteLuaCall() {
    luaCalledThisReload = true;
}

void ModuleConfigStore::endReload(bool verificationPassed) {
    if (!verificationPassed && !luaCalledThisReload) {
        store           = std::move(parkedStore);
        declaredCustoms = std::move(parkedCustoms);
    }
    parkedStore.clear();
    parkedCustoms.clear();
}

const std::map<std::string, SModuleConfig>& ModuleConfigStore::all() {
    return store;
}

SModuleConfig ModuleConfigStore::get(const std::string& name) {
    if (const auto IT = store.find(name); IT != store.end()) {
        auto copy = IT->second;
        copy.name = name;
        return copy;
    }
    return SModuleConfig{.name = name};
}

bool ModuleConfigStore::isDeclaredCustom(const std::string& name) {
    return declaredCustoms.contains(name);
}
