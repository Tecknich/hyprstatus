# hyprstatus

A status bar that lives **inside** the compositor. hyprstatus is a Hyprland
plugin (`.so` loaded by hyprpm) that replaces Waybar with a bar rendered by
Hyprland itself — the first in-compositor status bar for Hyprland.

> [!WARNING]
> This software is 99% vibe coded with Claude Fable 5, however it has been externally reviewed.
> Use at your own discretion


## Why not just Waybar?

| | Waybar | hyprstatus |
|---|---|---|
| process | separate GTK client | inside Hyprland |
| hyprland state (workspaces, titles, submaps) | polled/streamed over IPC sockets | read directly from compositor memory, updated by compositor events the same frame |
| config | JSONC + GTK CSS, own reload | `hyprland.conf` (`plugin:hyprstatus:*`), atomically re-themed by `hyprctl reload` |
| rendering | GTK, own damage | Hyprland's GL render pass + damage tracking — costs nothing while nothing changes; native blur |
| clicks | layer-shell surface | consumed pre-keybind by the compositor; workspace clicks call dispatchers directly |
| control | signals / waybar-msg | `hyprctl dispatch hyprstatus:toggle`, `hyprstatus:refresh [module]`, `hyprctl hyprstatus` (JSON state) |

## Install

```sh
hyprpm add https://github.com/Tecknich/hyprstatus   # or a local repo path
hyprpm enable hyprstatus
hyprpm reload
```

Add `exec-once = hyprpm reload -n` to `hyprland.conf` so it loads on start.
Build deps beyond Hyprland headers: `pangocairo`, `libpulse`, `libsystemd`
(sd-bus), `librsvg`, `cairo` — all present on any Arch/Hyprland box.

For development: `cmake -B build -S . && cmake --build build` then
`hyprctl plugin load "$PWD/build/hyprstatus.so"` (absolute path required).

## Configure

Everything lives in `hyprland.conf`. See [`example/hyprstatus.conf`](example/hyprstatus.conf)
for a full setup (floating themed bar, custom script modules, tray).

```ini
plugin {
    hyprstatus {
        height = 38
        margin = 4
        rounding = 6
        font_family = JetBrainsMono Nerd Font
        font_size = 13
        modules_left = workspaces window
        modules_center = clock
        modules_right = tray network pulseaudio battery
        col.background = rgba(06051aa6)
        col.accent = rgba(ff6ec7ff)
        # col.foreground, col.border, col.ok, col.warn, col.err, ...
    }
}
```

Per-module options use Waybar's option names, one per line, commas in values
allowed (only the first two commas split):

```ini
hyprstatus-set = clock, format, %a, %b %d   %I:%M %p
hyprstatus-set = battery, states.warning, 20
hyprstatus-set = network, on-click-right, pypr toggle nmtui
```

Custom script modules are Waybar-compatible (`return-type json` with
`{text, alt, tooltip, class, percentage}`, `exec-if`, `interval`, streaming
mode when `interval` is omitted, `signal N`, `max-length`, click/scroll
actions):

```ini
hyprstatus-module = nowplaying
hyprstatus-set = nowplaying, exec, ~/.config/scripts/nowplaying.sh
hyprstatus-set = nowplaying, return-type, json
hyprstatus-set = nowplaying, interval, 2
hyprstatus-set = nowplaying, on-click, playerctl play-pause
```

To refresh a module instantly from a script (Waybar's `pkill -RTMIN+N waybar`):
either `pkill -RTMIN+9 Hyprland` with `hyprstatus-set = mymod, signal, 9`, or
`hyprctl dispatch hyprstatus:refresh mymod`.

## Modules

`workspaces` (persistent lists, urgent/active states, click/scroll switching) ·
`window` · `clock` (calendar tooltip) · `cpu` · `memory` · `temperature` ·
`battery` · `network` · `pulseaudio` (PipeWire via pipewire-pulse) ·
`power-profiles` · `language` · `submap` · `tray` (StatusNotifierItem host) ·
custom exec modules.

Waybar option names (`format`, `format-icons.<key>`, `states.*`, `interval`,
`on-click*`, `tooltip-format*`, `max-length`, ...) work as you expect.

## Known limitations (v1)

- **Tray menus**: items are shown, clicked, scrolled; `ContextMenu` is
  forwarded, but hyprstatus does not render DBusMenu popups yet — pure
  appindicator items (nm-applet & friends) won't pop their menus. Planned.
- The bar auto-hides over fullscreen windows (compositor skips the render
  stage) — by design.
- While a Hyprland config-error banner is visible it shares the reserved-area
  slot with the bar; layout normalizes as soon as the error is fixed.
- One bar; per-monitor different layouts not yet supported (`monitors = `
  restricts which outputs show the bar).

## Compatibility

Built and tested against Hyprland **0.55.4**. Plugins are ABI-locked to the
exact running compositor build — always install through `hyprpm`, which builds
against your running version's headers.
