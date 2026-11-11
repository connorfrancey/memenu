-- Higher Jumps: patches the game's own UnrealScript. TdMove_Jump.StartMove does
--
--     WantedJumpVelocity.Z = (heavy ? BaseJumpZHeavy : BaseJumpZ) * PawnOwner.GetMobilityMultiplier();
--
-- and this mod makes it `(...) * Multiplier`: two tokens added to the function's bytecode (a float constant, and a
-- float multiply taken from the one already in that line), the assignment pointed at them, the function assembled
-- again and put in place. No native detour: the game's VM runs the new code. Turned off, the original goes back.

local FUNCTION = "TdGame.TdMove_Jump.StartMove"

local patched = nil     -- The multiplier in place, nil if none

-- The statement `WantedJumpVelocity.Z = <rhs>`: its index, or nil
local function find_assignment(s)
    for i = 1, s:count() do
        local t = s:get(i)
        if t.kind == "Let" then
            local lhs = s:get(t.children[1])
            if lhs.kind == "StructMember" and lhs.object_name == "Z" then
                local base = s:get(lhs.children[1])
                if base.object_name == "WantedJumpVelocity" then return i, t end
            end
        end
    end
end

local function patch(multiplier)
    local s = mirage.script.open(FUNCTION)
    if not s then return false end
    local i, let = find_assignment(s)
    if not i then
        mirage.warn("WantedJumpVelocity.Z = ... wasn't found in " .. FUNCTION)
        s:close()
        return false
    end
    local rhs = s:get(let.children[2])     -- (...) * GetMobilityMultiplier(): a float multiply
    if rhs.kind ~= "NativeFunction" then
        mirage.warn("the jump speed isn't a multiplication: " .. rhs.kind)
        s:close()
        return false
    end
    local factor = s:add { kind = "FloatConst", float = multiplier }
    local times = s:add { kind = "NativeFunction", native = rhs.native, children = { let.children[2], factor } }
    s:set_children(i, { let.children[1], times })
    local ok = s:apply()
    s:close()
    return ok
end

local module = mirage.module {
    name = "Higher Jumps",
    tab = "Mods",
    description = "Jumps start faster upwards: a patch of the game's script (TdMove_Jump.StartMove)",
    order = 12,
}
local Multiplier = mirage.setting("multiplier")

-- Patched on the game thread, where scripts run: on (again, if the multiplier changed) or off, as the module is
mirage.on("tick", function()
    local want = module:enabled() and Multiplier:get() or nil
    if want == patched then return end
    if patched then mirage.script.restore(FUNCTION) end
    patched = nil
    if want and patch(want) then
        patched = want
        mirage.log(string.format("jumps x%.2f", want))
    end
end)
