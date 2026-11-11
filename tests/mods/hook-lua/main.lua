-- Self-test mod: while its module is on it hooks Engine.Actor.CheckMaxEffectDistance before the call at priority 10
-- and turns CullDistance into CullDistance * 10 + 2 (after hook-native's * 10 + 1 at priority 20: 12 from 0). It
-- reads the pawn's Location.Z every tick, counts tick events and notes the map of each level load.
local hook

local m = mirage.module {
    name = "Hook (Lua)", tab = "Mods", description = "Self-test: hooks CheckMaxEffectDistance (priority 10)",
    on_enable = function()
        hook = mirage.hook("Engine.Actor.CheckMaxEffectDistance", function(call)
            local v = call.params.CullDistance * 10 + 2
            call.params.CullDistance = v
            Saw:set(v)
        end, { when = "before", priority = 10 })
    end,
    on_disable = function()
        if hook then hook:remove() end
        hook = nil
    end,
    on_tick = function()
        local pawn = mirage.pawn()
        if pawn then PawnZ:set(pawn.Location.Z) end
    end,
}

Saw = mirage.setting("saw")
PawnZ = mirage.setting("pawn_z")
LastMap = mirage.setting("last_map")
Ticks = m:slider_int("Ticks", 0, 0, 1 << 30)

mirage.on("tick", function(seconds) Ticks:set(Ticks:get() + 1) end)
mirage.on("level_load", function(finished, map) if finished then LastMap:set(map) end end)
