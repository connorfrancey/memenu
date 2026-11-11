-- Self-test mod: hot reload. The self-test rewrites VERSION and reloads; Status shows which code runs, Kept (a
-- mod.toml setting) and the module's on/off state must survive every reload.
local VERSION = 0
local m = mirage.module { name = "Reload (Lua)", tab = "Mods", description = "Hot reload test",
    on_tick = function() Ticks:set(Ticks:get() + 1) end }
Status = m:text("Status", "")
Ticks = m:slider_int("Ticks", 0, 0, 1 << 30)
Status:set("v" .. VERSION)
