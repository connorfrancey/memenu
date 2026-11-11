-- Hello (Lua): the native Hello example as a Lua mod. Copy this folder into the menu's mods folder
-- (Binaries\memenu\mods\hello-lua) and it shows up in the Mods column, no build needed.

local greet = require("greeting")    -- greeting.lua, next to this file

local hello
hello = mirage.module {
    name = "Hello (Lua)",
    tab = "Mods",
    description = "A Lua example mod: draws a greeting while on",
    order = 11,

    on_enable = function()
        local times = tonumber(mirage.config.get("times_enabled", "0")) + 1
        mirage.config.set("times_enabled", times)
        Status:set("on (" .. times .. " times so far)")
        mirage.log("enabled,", times, "times so far")
    end,

    on_disable = function()
        Status:set("off")
    end,

    on_tick = function()
        Ticks:set(Ticks:get() + 1)
    end,

    on_render = function()
        local w, h = mirage.draw.screen_size()
        local text = greet(Greeting:get(), Ticks:get())
        local tw, th = mirage.draw.text_size(text)
        local x, y = w * 0.5, h * Height:get()
        mirage.draw.rect_filled(x - 6, y - 4, x + tw + 6, y + th + 4, mirage.rgba(0, 0, 0, 140))
        mirage.draw.text(x, y, mirage.rgba(255, 200, 120), text)
    end,
}

Greeting = hello:text("Greeting", "Hello from a Lua mod", "What it draws")
Height = hello:slider("Height", 0.25, 0.0, 0.95, "%.2f", "Where on the screen, from the top")
Ticks = hello:slider_int("Ticks", 0, 0, 1 << 30, "Game ticks counted while on")
Status = hello:text("Status", "off", "Set by the mod")
