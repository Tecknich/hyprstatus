#include "Factories.hpp"

#include <functional>
#include <map>

#include "../config/ModuleConfig.hpp"

UP<IModule> createModuleByName(const SModuleConfig& cfg) {
    static const std::map<std::string, std::function<UP<IModule>(const SModuleConfig&)>> FACTORIES = {
        {"workspaces", makeWorkspacesModule},
        {"window", makeWindowModule},
        {"clock", makeClockModule},
        {"cpu", makeCpuModule},
        {"memory", makeMemoryModule},
        {"temperature", makeTemperatureModule},
        {"battery", makeBatteryModule},
        {"network", makeNetworkModule},
        {"pulseaudio", makePulseModule},
        {"power-profiles", makePowerProfilesModule},
        {"language", makeLanguageModule},
        {"submap", makeSubmapModule},
        {"tray", makeTrayModule},
    };

    if (const auto IT = FACTORIES.find(cfg.name); IT != FACTORIES.end())
        return IT->second(cfg);

    // declared customs and anything that looks like a custom module
    if (ModuleConfigStore::isDeclaredCustom(cfg.name) || cfg.opts.contains("exec") || cfg.opts.contains("format"))
        return makeCustomModule(cfg);

    return nullptr;
}
