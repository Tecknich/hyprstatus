cmake_minimum_required(VERSION 3.27)
project(hyprstatus
    DESCRIPTION "Compositor-rendered status bar plugin for Hyprland"
    VERSION 0.1.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

file(GLOB_RECURSE SRC CONFIGURE_DEPENDS "src/*.cpp")

add_library(hyprstatus SHARED ${SRC})
# hyprpm expects build/hyprstatus.so, not libhyprstatus.so
set_target_properties(hyprstatus PROPERTIES PREFIX "")

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # keep dlclose()/plugin unload functional (no STB_GNU_UNIQUE symbols)
    target_compile_options(hyprstatus PRIVATE -fno-gnu-unique)
endif()
# addConfigKeyword and a few friends are [[deprecated]] but still the only
# supported mechanism for repeatable plugin keywords in 0.55
target_compile_options(hyprstatus PRIVATE -Wno-deprecated-declarations -Wno-c++11-narrowing)

find_package(PkgConfig REQUIRED)
pkg_check_modules(deps REQUIRED IMPORTED_TARGET
    hyprland          # headers only; Hyprland symbols resolve at dlopen
    libdrm
    pixman-1
    wayland-server
    xkbcommon
    pangocairo        # text measurement helpers (renderText does the heavy lifting)
    cairo             # tray icon surfaces
    libpulse          # pulseaudio module
    libsystemd        # sd-bus: power-profiles + tray (SNI)
    librsvg-2.0       # tray SVG icons
)
target_link_libraries(hyprstatus PRIVATE rt PkgConfig::deps)
# Deliberately NO -Wl,--no-undefined: compositor symbols stay undefined.
