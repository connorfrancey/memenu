-- Trainer: features the menu had as native code, now a mod using only the public API (and the example to copy).
--
--   God Mode            the controller's bGodMode, full health and no fall deaths every tick
--   Teleport to Spawn   back to the start spot the game itself picks (GameInfo.FindPlayerStart), facing its way
--   Game Speed          the world's TimeDilation and the game's GameSpeed

local player = require("player")

-- What a module undoes when switched off is done on the game thread, in a tick event: on_disable is called from the
-- menu's thread, and a tick already under way could otherwise set things again right after it.
local undo = {}
mirage.on("tick", function()
    for name, fn in pairs(undo) do
        undo[name] = nil
        fn()
    end
end)

-- ---- God Mode ------------------------------------------------------------------------------------------------------
local god
god = mirage.module {
    name = "God Mode",
    tab = "Player",
    description = "Take no damage",
    order = 10,

    on_enable = function()
        -- Switched on during a death fall, it catches you (like the trainer's God tool)
        local pawn, pc = mirage.pawn(), mirage.controller()
        if pawn and pc and pawn.Health > 0 and pawn.MovementState == player.MOVE_FallingUncontrolled then
            player.reset_movement(pawn, pc)
        end
    end,

    on_tick = function()
        local pawn, pc = mirage.pawn(), mirage.controller()
        if not pawn or not pc or not god:enabled() then return end   -- A tick still under way when switched off
        pc.bGodMode = true                  -- The engine's own "God" flag
        pawn.Health = pawn.MaxHealth
        pawn.EnterFallingHeight = -1e30     -- No fall deaths
    end,

    on_disable = function()
        undo.god = function()
            local pc = mirage.controller()
            if pc then pc.bGodMode = false end
        end
    end,
}

-- ---- Teleport to Spawn ------------------------------------------------------------------------------------------------
mirage.module {
    name = "Teleport to Spawn",
    tab = "Player",
    description = "Move the player back to the level start",
    order = 40,
    action = true,

    on_trigger = function()
        local pawn, pc, world = mirage.pawn(), mirage.controller(), mirage.world()
        local game = world and world.Game
        if not pawn or not pc or not game then
            mirage.notify("Teleport to Spawn: no player")
            return
        end
        local start = mirage.call(game, "FindPlayerStart", { Player = pc, InTeam = 0 })   -- IncomingName stays ""
        if type(start) ~= "userdata" then
            mirage.notify("Teleport to Spawn: this level has no start spot")
            return
        end
        player.reset_movement(pawn, pc)     -- Out of any move (wallrun, grab, ...) first
        pawn.Velocity.X, pawn.Velocity.Y, pawn.Velocity.Z = 0, 0, 0
        pawn.Acceleration.X, pawn.Acceleration.Y, pawn.Acceleration.Z = 0, 0, 0
        pawn.Location.X, pawn.Location.Y, pawn.Location.Z = start.Location.X, start.Location.Y, start.Location.Z
        pc.Rotation.Yaw = start.Rotation.Yaw    -- The view, and the body with it
        pawn.Rotation.Yaw = start.Rotation.Yaw
        mirage.log(string.format("at %s (%.0f, %.0f, %.0f)", mirage.name(start), start.Location.X, start.Location.Y,
            start.Location.Z))
        mirage.notify("Teleported to the level start")
    end,
}

-- ---- Game Speed -------------------------------------------------------------------------------------------------------
local function set_speed(speed)
    local world = mirage.world()
    if not world then return end
    world.TimeDilation = speed
    if world.Game then world.Game.GameSpeed = speed end
end

local Speed
local speed
speed = mirage.module {
    name = "Game Speed",
    tab = "World",
    description = "Change the world time dilation",
    order = 10,
    on_tick = function()
        if speed:enabled() then set_speed(Speed:get()) end    -- A tick still under way when switched off: nothing
    end,
    on_disable = function() undo.speed = function() set_speed(1.0) end end,
}
Speed = mirage.setting("game_speed")
