-- hyprstatus - example configuration (Hyprland >= 0.57, Lua config)
--
-- A full, self-contained bar: floating + themed, native modules, two
-- Waybar-compatible script modules, notifications, and a tray. Load it from
-- ~/.config/hypr/hyprland.lua:
--     dofile(os.getenv("HOME") .. "/.config/hypr/hyprstatus.lua")
--
-- Hyprland 0.57 configures plugins only through Lua. Global options go in an
-- hl.config({ plugin = { hyprstatus = ... } }) table; per-module options use
-- hl.plugin.hyprstatus.set(module, key, value) with Waybar's option names
-- (format, format-icons.<key>, states.*, interval, on-click*, tooltip-format*,
-- ...). All three arguments are plain strings, so commas, quotes and real
-- newlines ("\n") inside a value need no special treatment.
--
-- Plugins load AFTER hyprland.lua first runs (hyprpm reload -n from your
-- autostart), and Hyprland re-runs the whole config once the plugin is in. Both
-- blocks below are therefore guarded so the first pass and --verify-config stay
-- error-free: unknown plugin keys are config errors, so probe a sentinel first.
--
-- Recommended companion: pyprland (https://github.com/hyprland-community/pyprland).
-- The `pypr toggle <name>` click actions below are scratchpad dropdowns defined in
-- example/pyprland.toml; a commented-out plain-app alternative sits under each
-- for pypr-less setups.

local function plugin_config_ready(sentinel)
  if not hl.get_config then return false end
  local _, err = hl.get_config(sentinel)
  return err == nil
end

if plugin_config_ready("plugin.hyprstatus.enabled") then
  hl.config({ plugin = { hyprstatus = {
    enabled            = true,
    position           = "top",
    height             = 38,
    margin             = 4,     -- float the bar with the same gap as tiled windows
    spacing            = 4,
    rounding           = 6,
    border_size        = 1,
    blur               = false,
    hide_on_fullscreen = true,  -- hide the bar while a window is fullscreen
    hide_on_overview   = true,  -- hide the bar while a gloview overview is open
    font_family        = "JetBrainsMono Nerd Font",
    font_size          = 13,

    modules_left   = "identity workspaces",
    modules_center = "nowplaying",
    modules_right  = "tray updates cpu memory gpu network pulseaudio power-profiles battery notifications clock",

    -- ---- palette (named slots; every module resolves its colours here) ----
    col = {
      background        = "rgba(06051aa6)",  -- bar background (translucent)
      foreground        = "rgba(8b80b0ff)",  -- normal text
      foreground_bright = "rgba(e8e3ffff)",  -- emphasised text (bold identity, ...)
      border            = "rgba(2a2150ff)",
      accent            = "rgba(ff6ec7ff)",  -- active workspace, clock, highlights
      accent_dim        = "rgba(8f3f74ff)",
      ok                = "rgba(9ece6aff)",  -- charging / healthy states
      warn              = "rgba(e0af68ff)",  -- warning states
      err               = "rgba(f7768eff)",  -- critical states
      tooltip_bg        = "rgba(151129f0)",
      tooltip_fg        = "rgba(cdc7e8ff)",
    },
  } } })
end

if hl.plugin and hl.plugin.hyprstatus and hl.plugin.hyprstatus.set and hl.plugin.hyprstatus.module then
  local module = hl.plugin.hyprstatus.module
  local set    = hl.plugin.hyprstatus.set

  -- ---- static text module -------------------------------------------------
  module("identity")
  set("identity", "format", "hyprstatus")
  set("identity", "tooltip", "false")
  set("identity", "color", "accent")
  set("identity", "padding", "14")
  set("identity", "bold", "true")

  -- ---- native modules -----------------------------------------------------
  set("workspaces", "persistent", "1 2 3 4 5")
  set("workspaces", "padding", "10")

  set("clock", "format", "%a, %b %d   %I:%M %p")
  set("clock", "interval", "1")
  set("clock", "color", "accent")
  set("clock", "padding", "12")
  -- pill background + border (a "clock pill" like Waybar), matching the theme
  set("clock", "background", "rgba(ff6ec71a)")
  set("clock", "box-border", "rgba(8f3f74ff)")
  set("clock", "box-rounding", "6")
  -- hover shows a month calendar; right-click the clock to toggle a full-year view

  set("cpu", "format", "CPU {usage:>2}%")
  set("cpu", "interval", "2")
  set("memory", "format", "MEM {percentage:>2}%")
  set("memory", "interval", "2")

  set("gpu", "format", "GPU {usage:>2}%")
  set("gpu", "interval", "2")
  -- Hover shows a panel with a VRAM bar, utilization, clocks, power, temp and fan.
  -- Auto-detect prefers a discrete GPU (NVIDIA via NVML, else the largest-VRAM
  -- amdgpu card). Pin one explicitly by vendor, DRM card, PCI slot or name:
  -- set("gpu", "device", "nvidia")
  -- set("gpu", "device", "0000:01:00.0")
  -- Colour the segment once the GPU is busy (uses the warning/critical classes):
  -- set("gpu", "warning-threshold", "70")
  -- set("gpu", "critical-threshold", "90")
  -- A custom tooltip-format replaces the panel with plain text:
  -- set("gpu", "tooltip-format", "{name}\n{vramUsed} / {vramTotal} GiB ({vramPercentage}%)")

  set("network", "format-wifi", "{essid} {signalStrength}%")
  set("network", "format-ethernet", "{ifname}")
  set("network", "format-disconnected", "NET x")
  set("network", "interval", "2")
  set("network", "tooltip-format-wifi", "{ifname} - {essid}\nIP: {ipaddr}/{cidr}\nStrength: {signalStrength}%\nDown {bandwidthDownBits}  Up {bandwidthUpBits}")
  -- dropdown via a pyprland scratchpad (see example/pyprland.toml)
  set("network", "on-click", "pypr toggle nmtui")
  -- without pyprland:
  -- set("network", "on-click", "nm-connection-editor")

  set("pulseaudio", "format", "{icon} {volume}%")
  set("pulseaudio", "format-bluetooth", "{volume}%")
  set("pulseaudio", "format-muted", "muted")
  set("pulseaudio", "format-icons.speaker", "")
  set("pulseaudio", "format-icons.headphone", "")
  set("pulseaudio", "format-icons.hdmi", "")
  set("pulseaudio", "format-icons.default", "")
  set("pulseaudio", "scroll-step", "5")
  set("pulseaudio", "max-volume", "153")
  -- dropdown mixer via a pyprland scratchpad
  set("pulseaudio", "on-click", "pypr toggle audio")
  -- without pyprland:
  -- set("pulseaudio", "on-click", "pavucontrol")

  set("power-profiles", "format", "{icon}")
  set("power-profiles", "format-icons.power-saver", "ECO")
  set("power-profiles", "format-icons.balanced", "BAL")
  set("power-profiles", "format-icons.performance", "PERF")
  set("power-profiles", "format-icons.default", "PWR")

  set("battery", "interval", "30")
  set("battery", "states.warning", "20")
  set("battery", "states.critical", "10")
  set("battery", "format", "{icon} {capacity}%")
  set("battery", "format-charging", " {capacity}%")
  set("battery", "format-plugged", " {capacity}%")
  set("battery", "format-icons", "    ")
  set("battery", "tooltip-format", "{timeTo}  ({power}W)\nHealth: {health}%")

  -- notifications: built-in SwayNotificationCenter module. Bell + unread count;
  -- left-click opens the panel, right-click toggles do-not-disturb.
  set("notifications", "format", "{icon} {text}")
  set("notifications", "format-icons.notification", "●")
  set("notifications", "format-icons.none", "")
  set("notifications", "format-icons.dnd-notification", "●")
  set("notifications", "format-icons.dnd-none", "")
  set("notifications", "on-click", "swaync-client -t -sw")
  set("notifications", "on-click-right", "swaync-client -d -sw")
  set("notifications", "color.notification", "accent")

  -- ---- custom exec modules (Waybar-compatible) -----------------------------
  -- now-playing: current track via playerctl (plain text, polled). A json script
  -- can return {text,alt,tooltip,class,percentage} instead - see the README.
  module("nowplaying")
  set("nowplaying", "exec", "playerctl metadata --format '{{artist}} - {{title}}' 2>/dev/null")
  set("nowplaying", "exec-if", "command -v playerctl")
  set("nowplaying", "interval", "2")
  set("nowplaying", "max-length", "60")
  set("nowplaying", "hide-when-empty", "true")
  set("nowplaying", "on-click", "playerctl play-pause")
  set("nowplaying", "on-click-right", "playerctl next")
  set("nowplaying", "on-scroll-up", "playerctl volume 0.05+")
  set("nowplaying", "on-scroll-down", "playerctl volume 0.05-")
  set("nowplaying", "padding", "14")

  -- pending updates count (pacman-contrib's checkupdates; swap for your distro).
  module("updates")
  set("updates", "exec", "echo \"$(checkupdates 2>/dev/null | wc -l) upd\"")
  set("updates", "exec-if", "command -v checkupdates")
  set("updates", "interval", "600")
  set("updates", "on-click", "kitty -e sh -c 'checkupdates; read x'")

  -- ---- tray (StatusNotifierItem host) --------------------------------------
  -- hide         : space/comma tokens; hide an item whose Id/Title contains one
  -- hide-passive : hide Passive items entirely instead of dimming them
  -- hide-if-bt-disconnected : ";"-separated "<item>=<device>" rules - hide the
  --   matched item unless a BlueZ device whose Alias/Name/Address contains
  --   <device> is Connected right now (watched live over the system bus). Use it
  --   for apps that keep their tray item registered after their bluetooth device
  --   drops - e.g. librepods leaves its AirPods item (with stale battery) behind:
  set("tray", "hide-if-bt-disconnected", "librepods=AirPods")
end

-- Toggle the whole bar / refresh a module. Plugin dispatchers are gone in
-- Hyprland 0.57 (`hyprctl dispatch hyprstatus:toggle` no longer exists), so
-- these are Lua functions:
--   hl.bind("SUPER + B", function() hl.plugin.hyprstatus.toggle() end)
--   hyprctl eval 'hl.plugin.hyprstatus.refresh("updates")'   -- no argument = all
-- Scripts can also refresh a module with `pkill -RTMIN+N Hyprland` after
-- set("<module>", "signal", "N").
