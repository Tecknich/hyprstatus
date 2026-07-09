#define WLR_USE_UNSTABLE
#include "ModuleConfig.hpp"
#include "../globals.hpp"

#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprutils/string/String.hpp>

#include <cctype>
#include <set>

#if __has_include(<lua.h>) && __has_include(<lauxlib.h>)
extern "C" {
#include <lua.h>
#include <lauxlib.h>
}
#define HYPRSTATUS_HAS_LUA 1
#endif

namespace {
    std::map<std::string, SModuleConfig> store;
    std::set<std::string>                declaredCustoms;

    bool validName(const std::string& s) {
        if (s.empty())
            return false;
        for (const char C : s) {
            if (!std::isalnum((unsigned char)C) && C != '_' && C != '-')
                return false;
        }
        return true;
    }

    // literal backslash-n in keyword values -> real newline (tooltip formats;
    // hyprlang values are single-line so there is no other way to express one)
    std::string unescapeNewlines(std::string v) {
        size_t pos = 0;
        while ((pos = v.find("\\n", pos)) != std::string::npos) {
            v.replace(pos, 2, "\n");
            pos += 1;
        }
        return v;
    }

    // Newline-unescaping is presentation-only: it must NOT touch command values
    // (exec, exec-if, on-click*, on-scroll*, ...) where a literal \n is real shell
    // syntax (e.g. awk/sed programs) and rewriting it silently breaks the command.
    // Whitelist the keys where an embedded newline is the intended meaning: the
    // format family (format, format-alt, format-icons, ...) and any tooltip key
    // (tooltip, tooltip-format, ...). Everything else is stored VERBATIM.
    bool wantsNewlineUnescape(const std::string& key) {
        return key.starts_with("format") || key.find("tooltip") != std::string::npos;
    }

    std::string trimLeading(const std::string& s) {
        size_t i = 0;
        while (i < s.size() && std::isspace((unsigned char)s[i]))
            ++i;
        return s.substr(i);
    }

    SModuleConfig& ensureEntry(const std::string& name) {
        auto& entry = store[name];
        entry.name  = name;
        return entry;
    }
}

// legacy (hyprlang) backend: repeatable keywords ------------------------------

static Hyprlang::CParseResult onModuleKeyword(const char* COMMAND, const char* VALUE) {
    Hyprlang::CParseResult result;

    const auto NAME = Hyprutils::String::trim(std::string{VALUE ? VALUE : ""});
    if (!validName(NAME)) {
        result.setError("hyprstatus-module: invalid module name (allowed: [A-Za-z0-9_-]+)");
        return result;
    }

    declaredCustoms.insert(NAME);
    ensureEntry(NAME);
    return result;
}

static Hyprlang::CParseResult onSetKeyword(const char* COMMAND, const char* VALUE) {
    Hyprlang::CParseResult result;

    // split on the FIRST TWO commas only; the remainder is the raw value
    // (inner commas and spacing preserved, e.g. clock formats)
    const std::string RAW    = VALUE ? VALUE : "";
    const auto        FIRST  = RAW.find(',');
    const auto        SECOND = FIRST == std::string::npos ? std::string::npos : RAW.find(',', FIRST + 1);

    if (SECOND == std::string::npos) {
        result.setError("hyprstatus-set = <module>, <key>, <value>");
        return result;
    }

    const auto NAME = Hyprutils::String::trim(RAW.substr(0, FIRST));
    const auto KEY  = Hyprutils::String::trim(RAW.substr(FIRST + 1, SECOND - FIRST - 1));

    if (NAME.empty() || KEY.empty()) {
        result.setError("hyprstatus-set = <module>, <key>, <value>");
        return result;
    }

    const auto VAL = trimLeading(RAW.substr(SECOND + 1));
    ensureEntry(NAME).opts[KEY] = wantsNewlineUnescape(KEY) ? unescapeNewlines(VAL) : VAL;
    return result;
}

// lua backend -----------------------------------------------------------------

#ifdef HYPRSTATUS_HAS_LUA
static int luaModule(lua_State* L) {
    const auto NAME = Hyprutils::String::trim(luaL_checkstring(L, 1));

    if (!validName(NAME))
        return luaL_error(L, "hyprstatus.module: invalid module name (allowed: [A-Za-z0-9_-]+)");

    declaredCustoms.insert(NAME);
    ensureEntry(NAME);
    return 0;
}

static int luaSet(lua_State* L) {
    const auto  NAME  = Hyprutils::String::trim(luaL_checkstring(L, 1));
    const auto  KEY   = Hyprutils::String::trim(luaL_checkstring(L, 2));
    const char* VALUE = luaL_checkstring(L, 3);

    if (NAME.empty() || KEY.empty())
        return luaL_error(L, "hyprstatus.set(module, key, value): module and key must be non-empty");

    // lua string literals already carry real newlines; no unescaping
    ensureEntry(NAME).opts[KEY] = VALUE;
    return 0;
}
#endif

// public API -------------------------------------------------------------------

void ModuleConfigStore::registerKeywords() {
    const auto TYPE = Config::mgr()->type();

    if (TYPE == Config::CONFIG_LEGACY) {
        HyprlandAPI::addConfigKeyword(PHANDLE, "hyprstatus-module", ::onModuleKeyword, Hyprlang::SHandlerOptions{});
        HyprlandAPI::addConfigKeyword(PHANDLE, "hyprstatus-set", ::onSetKeyword, Hyprlang::SHandlerOptions{});
    } else if (TYPE == Config::CONFIG_LUA) {
#ifdef HYPRSTATUS_HAS_LUA
        HyprlandAPI::addLuaFunction(PHANDLE, "hyprstatus", "module", ::luaModule);
        HyprlandAPI::addLuaFunction(PHANDLE, "hyprstatus", "set", ::luaSet);
#else
        HyprlandAPI::addNotification(PHANDLE, "[hyprstatus] built without Lua headers: hl.plugin.hyprstatus.module/set are unavailable on the Lua config backend",
                                     CHyprColor{1.0, 0.7, 0.2, 1.0}, 8000);
#endif
    }
}

void ModuleConfigStore::clear() {
    store.clear();
    declaredCustoms.clear();
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
