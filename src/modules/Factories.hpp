#pragma once
#define WLR_USE_UNSTABLE
#include <hyprland/src/helpers/memory/Memory.hpp>

#include "Module.hpp"

// Each module .cpp keeps its class file-local and exposes one factory.
UP<IModule> makeWorkspacesModule(const SModuleConfig&);
UP<IModule> makeWindowModule(const SModuleConfig&);
UP<IModule> makeClockModule(const SModuleConfig&);
UP<IModule> makeCpuModule(const SModuleConfig&);
UP<IModule> makeMemoryModule(const SModuleConfig&);
UP<IModule> makeTemperatureModule(const SModuleConfig&);
UP<IModule> makeBatteryModule(const SModuleConfig&);
UP<IModule> makeNetworkModule(const SModuleConfig&);
UP<IModule> makePulseModule(const SModuleConfig&);
UP<IModule> makePowerProfilesModule(const SModuleConfig&);
UP<IModule> makeLanguageModule(const SModuleConfig&);
UP<IModule> makeSubmapModule(const SModuleConfig&);
UP<IModule> makeTrayModule(const SModuleConfig&);
UP<IModule> makeCustomModule(const SModuleConfig&);

// Dispatch by module name: native names map to their factories; names declared
// via hyprstatus-module (or unknown names carrying an `exec`/`format` option)
// become custom modules. Returns nullptr for unresolvable names.
UP<IModule> createModuleByName(const SModuleConfig& cfg);
