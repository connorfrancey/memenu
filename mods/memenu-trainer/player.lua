-- The player's movement, put back to plain walking (the menu's player_state::ResetMovement, in Lua)
local M = {}

M.MOVE_Walking = 1
M.MOVE_RumpSlide = 38
M.MOVE_FallingUncontrolled = 72

-- Out of whatever move the player is in (a wallrun, a grab, an uncontrolled fall), walking with the controls back
function M.reset_movement(pawn, pc)
    local state = pawn.MovementState
    pawn.InitialState = "Walking"
    mirage.call(pawn, "SetInitialState")
    pc.InitialState = "PlayerWalking"
    mirage.call(pc, "SetInitialState")
    mirage.call(pawn, "StopSlideEffect")
    mirage.call(pawn, "StopAllCustomAnimations", { BlendOutTime = 0 })
    mirage.call(pawn, "SetMove", { NewMove = M.MOVE_Walking })
    pc.bIgnoreLookInput = false
    pc.bIgnoreMoveInput = false
    pc.bIgnoreButtonInput = false
    if state == M.MOVE_RumpSlide then
        pawn.WalkableFloorZ = 0.71
        pawn.GravityModifier = 1.0
        pawn.StreakEffectOverride = 0
    end
end

return M
