-- Display-binding fixture for the host test: not a playable game, just a
-- ruleset that declares every shape of `bar` monitor row so the test can
-- assert the binding reaches DisplayCtrl with the right pointers.
--
-- The point of the shape is that a bar's TIMING is not the display's: the
-- fill duration and the instant the wait began are both ordinary game
-- vars, kept current by whoever owns the wait.  For a projector's energy
-- that matters, because a "refill" recharge starts when the trigger comes
-- up, not when the pool hit zero.
local S = { IN_GAME = 0, DONE = 1 }

return {
  api = 1, type_id = 0x7F05, name = "Bar",
  initial_state = S.IN_GAME,
  scoring_state = S.DONE,

  config = {},
  vars = {
    { id = "energy",       default = 5 },   -- the number, and the bar's value
    { id = "reload",       default = 0 },   -- millis the wait began; 0 = idle
    { id = "reload_ms",  default = 10000 }, -- how long the wait takes, in ms
    { id = "respawn_zero", default = 0 },   -- pinned at the trigger
    { id = "respawn_ms", default = 30000 },
  },
  monitor = {
    -- Owner-timed: the projector shape.  Number until energy hits 0, then a
    -- bar filling over reload_ms from the instant named by reload.
    { var = "energy", icon = "ENERGY", col = 0, row = 0, states = { S.IN_GAME },
      bar = true, bar_at = 0, fill_var = "reload_ms", start_var = "reload" },
    -- Self-timed: no start_var, so the display starts its own clock when the
    -- value arrives at the trigger.  The respawn-wait shape.
    { var = "respawn_zero", icon = "TIME", col = 1, row = 0, states = { S.IN_GAME },
      bar = true, bar_at = 0, fill_var = "respawn_ms", width = 30 },
    -- An ordinary row alongside them, to prove the branch is per-row.
    { var = "energy", icon = "ENERGY", col = 0, row = 0, states = { S.DONE } },
    -- Parked on the other screen only so the host test can reach the slots
    -- the bar's two pointers address.
    { var = "reload",      icon = "TIME", col = 1, row = 0, states = { S.DONE } },
    { var = "reload_ms", icon = "TIME", col = 0, row = 1, states = { S.DONE } },
  },
  winners = { { var = "energy", dir = "max" } },
  totem_slots = {}, teams = 0,

  on_begin = function(vars) vars.energy = 5 end,
  rules = {},
  update = {},
}
