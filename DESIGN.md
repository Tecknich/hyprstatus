# hyprstatus — design

A status bar that lives *inside* Hyprland: a compositor plugin (`.so` loaded via
hyprpm/`hyprctl plugin load`), not a layer-shell client. First of its kind — every
existing bar (Waybar included) is an external Wayland client talking to Hyprland
over IPC.

Target: Hyprland **0.55.4** (commit `a0136d8c`). Plugins are ABI-locked to the
running compositor; hyprpm rebuilds per version.

## Why in-compositor beats Waybar

- **Zero-IPC state.** Workspaces, window titles, monitors, keyboard layout,
  submaps are read directly from compositor memory and updated by typed
  `Event::bus()` signals the same frame they change. No socket2 round-trips, no
  polling `hyprctl`, no stale state after reconnects.
- **One config, one reload.** Configured in `hyprland.conf` under
  `plugin:hyprstatus:*` + `hyprstatus-*` keywords; `hyprctl reload` re-themes the
  bar atomically with the rest of the desktop (halcyon can template one file).
- **Compositor-native rendering.** Drawn in Hyprland's own GL pass with its
  damage tracking: when nothing on the bar changes, the bar costs nothing. Real
  blur behind the bar via Hyprland's blur, exact monitor scale handling, no GTK.
- **First-class input.** Clicks/scrolls are consumed before any window sees
  them; workspace switching calls the compositor's own dispatchers.
- **Dispatchers + hyprctl.** `hyprstatus:toggle`, `hyprstatus:refresh` are
  bindable like any dispatcher; `hyprctl hyprstatus` dumps module state as JSON.

## Architecture

Everything compositor-facing runs on the main thread. Two carefully-fenced
exceptions (PulseAudio's threaded mainloop, nothing else) marshal back via an
eventfd registered on the compositor's wayland event loop.

```
main.cpp            PLUGIN_INIT/EXIT, config registration, dispatchers, hyprctl cmd
core/BarManager     module lifecycle, per-monitor CBar set, event listeners,
                    reserved-area management, input routing, render-stage hook
core/Bar            per-monitor layout (logical coords), hit regions, draw()
render/BarPassElement   IPassElement (EK_CUSTOM) queued at RENDER_POST_WINDOWS
render/TooltipPassElement   queued at RENDER_LAST_MOMENT on hovered monitor
render/TextCache    (text,font,px,weight,color,maxW) -> SP<ITexture>, LRU
config/ModuleConfig hyprstatus-module / hyprstatus-set keyword parsing (+ Lua)
services/MainThread eventfd waker: post(fn) from any thread -> runs on main
services/Exec       CProcess runAsync + wl_event_loop_add_fd (poll & streaming)
services/DBus       sd-bus session+system, fds pumped on the wl event loop
modules/*           one file per module, all implement IModule
```

### Rendering (the 0.55 pass system)

All drawing in 0.55 is deferred `IPassElement`s executed inside `endRender()`.
The plugin listens to `Event::bus()->m_events.render.stage`; at
`RENDER_POST_WINDOWS` it queues one `CBarPassElement` for the monitor being
rendered (`g_pHyprRenderer->m_renderData.pMonitor`). Insertion order = z-order:
above windows, below top/overlay layer-shell and the cursor. When a fullscreen
window is "solitary" the stage isn't emitted — the bar auto-hides over
fullscreen, which is the desired behavior.

Text goes through `g_pHyprRenderer->renderText()` (pango under the hood),
cached in TextCache keyed by content+style+scale. Boxes are computed in
monitor-local *logical* pixels and scaled by `monitor->m_scale` only at draw
time; `boundingBox()`/`opaqueRegion()` stay logical (the pass simplifier scales
them itself). Damage: any state change calls
`g_pHyprRenderer->damageBox(globalBarBox)` — that both schedules a frame on VFR
monitors and un-culls the element. No damage → `draw()` never runs → zero cost.

### Reserved space

`CMonitor::m_reservedArea` has typed dynamic slots; there is no plugin slot, so
hyprstatus uses `RESERVED_DYNAMIC_TYPE_ERROR_BAR` (the in-tree error overlay's
slot) with `resetType` + `addType` + `arrangeLayersForMonitor()`. The slot is
wiped by `applyMonitorRule` on every config reload / monitor change, so the
reservation is re-applied on `monitor.added`, `monitor.layoutChanged` and
`config.reloaded`. Known edge: while a config-error overlay is showing it owns
the same slot; the two fight cosmetically until the error clears (documented,
rare, harmless). Function-hooking `arrangeLayersForMonitor` would be bulletproof
but inline-patching compositor code is not worth the crash risk in v1.

Units are logical: reserve `(height + margin) / 1` in logical px (config values
are logical already).

### Input

`input.mouse.button` / `input.mouse.axis` (both cancellable, emitted before
keybinds and window delivery): if the cursor is inside a bar's box — and no
overlay layer-surface or seat grab claims that point — set
`info.cancelled = true` and route to the module segment under the cursor via
the layout's hit regions. `input.mouse.move` is only *observed* (never
cancelled) for hover styling and tooltips. Actions that mutate compositor state
(workspace switch) run via `g_pEventLoopManager->doLater` to avoid re-entrancy;
shell commands run via `Config::Supplementary::executor()->spawn()`.

### Module framework

`IModule` produces, per monitor, an ordered list of `SSegment`s (text or icon
texture, optional color/class, per-segment click identity). `CBar` lays out
left/center/right groups, records hit regions, draws background + border +
segments. Modules mark themselves dirty (`requestRedraw()`) — never draw.

Update sources per module type:
- event-driven (workspaces, window, layout, submap): event-bus listeners
- polled (cpu, memory, temperature, battery, network, custom exec): one
  `CEventLoopTimer` each, re-armed in the callback; sysfs/procfs reads are
  non-blocking and stay on the main thread
- external stacks: PulseAudio (own `pa_threaded_mainloop` thread →
  MainThread::post), sd-bus for power-profiles (system bus) and tray (session
  bus) pumped fd-driven on the wl event loop — no extra threads
- exec modules: `CProcess::runAsync` + pipe fd on the wl loop (AsyncDialogBox
  pattern) — polling *and* streaming (no `interval` = persistent child, one
  JSON line per update, à la `swaync-client -swb`). Child exit codes are
  unreliable inside Hyprland (`SA_NOCLDWAIT`), so `exec-if` is judged by a
  sentinel echo, not exit status.

### Configuration

Global options are typed `addConfigValueV2` values (`plugin:hyprstatus:height`,
`...:col.accent`, `...:modules_left`, …) — introspectable, validated, live on
`hyprctl reload`. Per-module options use repeatable keywords (the hyprbars
mechanism), value-as-rest so commas in formats survive:

```
hyprstatus-module = nowplaying                      # declare a custom module
hyprstatus-set = nowplaying, exec, ~/.config/halcyon/scripts/halcyon-nowplaying.sh
hyprstatus-set = nowplaying, interval, 2
hyprstatus-set = clock, format, %a, %b %d   %I:%M %p
```

Keyword state is cleared on `config.preReload` and modules are rebuilt on
`config.reloaded`. On the Lua config backend the same two operations are exposed
as `hl.plugin.hyprstatus.module{...}` / `hl.plugin.hyprstatus.set{...}`.

Waybar-compatible module option names (`format`, `interval`, `on-click`,
`format-icons.<alt>`, `states.warning`, …) so migration is mechanical.

## Modules (v1)

| module | source | parity notes |
|---|---|---|
| workspaces | compositor state + workspace/monitor events | per-monitor, `persistent` list, active/urgent colors, click=switch, scroll=cycle |
| window | focusState + window.title/active events | active title per monitor, max-length |
| clock | strftime, boundary-aligned timer | tooltip = month calendar (text grid) |
| cpu / memory / temperature | /proc/stat, /proc/meminfo, hwmon | waybar tokens |
| battery | /sys/class/power_supply (energy_* and charge_*) | {icon}{capacity}{timeTo}{power}{health}, states warning/critical, charging/plugged formats |
| network | route table + sysfs + /proc/net/wireless + SIOCGIWESSID + getifaddrs | wifi/ethernet/disconnected formats, {essid}{signalStrength}{ipaddr}{cidr}{bandwidth*Bits} |
| pulseaudio | libpulse threaded mainloop (pipewire-pulse) | {icon}{volume}, port/bluetooth icon selection, muted format, scroll ±step clamped to max-volume, middle=mute default |
| power-profiles | sd-bus system, org.freedesktop.UPower.PowerProfiles | {icon}{profile}{driver}, click cycles |
| language / submap | keyboard.layout + keybinds.submap events | hyprland/language + hyprland/submap parity |
| tray | SNI: plugin owns org.kde.StatusNotifierWatcher + host (session bus) | icons via IconName theme lookup (hicolor+configured theme, PNG+SVG via librsvg) with IconPixmap fallback; Activate/ContextMenu/Scroll; Passive=dim, NeedsAttention=highlight. **No DBusMenu renderer in v1** — menu-only items (appindicator ports) won't pop menus yet; documented, Phase 2 |
| custom | exec engine | exec / exec-if / interval / streaming / return-type json ({text,alt,tooltip,class,percentage}) / format-icons by alt / signal SIGRTMIN+N (signalfd) / max-length / hide-when-empty / 5 pointer actions |

Not in v1 (roadmap): DBusMenu popups, native MPRIS, per-module bar instances,
sliding animations, `hyprstatus-rule` per-monitor overrides.

## Class → color

Modules attach a class (`warning`, `critical`, `charging`, `muted`,
`has-updates`, `active`, …). Resolution: per-module `color.<class>` option →
built-in semantic map (`warning→col.warn`, `critical→col.err`, `charging→col.ok`,
`active→col.accent`, …) → module `color` option → global `col.foreground`.

## Lifecycle / safety rules (non-negotiable)

- Every `CHyprSignalListener` and `SP<CEventLoopTimer>` lives in owned state;
  PLUGIN_EXIT order: stop threads/join → remove fd sources → close fds → remove
  timers → reset listeners → `m_renderPass.removeAllOfType(...)` → drop textures
  → clear reservations + `arrangeLayersForMonitor` + damage monitors.
- No blocking calls on the main thread, ever (`execAndGet`, `runSync`, popen are
  banned; procfs/sysfs reads are the only sanctioned "I/O").
- Worker threads touch only their own mutex-guarded staging structs and
  `MainThread::post()`.
- Throwing from PLUGIN_INIT is the sanctioned load-failure path (after the
  `__hyprland_api_get_hash` check).

## Packaging

`hyprpm.toml` (`[hyprstatus]`, `output = "build/hyprstatus.so"`, CMake build
steps), CMake with `PREFIX ""`, `-fno-gnu-unique` on g++, C++23, deps:
`hyprland` (headers-only, symbols resolve at dlopen — no `--no-undefined`),
`pangocairo`, `libpulse`, `libsystemd`, `librsvg-2.0`, `cairo`. Install:
`hyprpm add <repo>` → `hyprpm enable hyprstatus` → `hyprpm reload`.
