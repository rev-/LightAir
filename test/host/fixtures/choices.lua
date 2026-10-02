-- Config-choices fixture for the host test: not a playable game, just a
-- ruleset whose config menu lists values with labels, beside an ordinary
-- numeric entry, so the test can assert the descriptor, the menu stepping
-- and the config blob.  on_begin also copies what la.roster() and
-- la.session_seed() report into vars, so the test can read them back.
local S = { IN_GAME = 0, DONE = 1 }

return {
  api = 1, type_id = 0x7F07, name = "Choices",
  initial_state = S.IN_GAME,
  scoring_state = S.DONE,

  config = {
    { id = "friendly_fire", name = "FriendlyFire", default = 0,
      choices = { { 0, "OFF" }, { 1, "ON" } } },
    -- Not contiguous, not sorted: the list order is the menu order.
    { id = "game_time", name = "Time", default = 600,
      choices = { { 600, "10 min" }, { 300, "5 min" }, { 1800, "30 min" } } },
    { id = "lives", name = "Lives", min = 1, max = 5, step = 2, default = 3 },
  },
  vars = {
    { id = "roster_n",     default = -1 },
    { id = "roster_first", default = -1 },
    { id = "roster_last",  default = -1 },
    { id = "seed",         default = -1 },
  },
  monitor = {
    { var = "lives", icon = "LIFE", col = 0, row = 0, states = { S.IN_GAME } },
    -- Parked on the other screen only so the host test can reach the slots.
    { var = "roster_n",     icon = "TIME", col = 0, row = 0, states = { S.DONE } },
    { var = "roster_first", icon = "TIME", col = 1, row = 0, states = { S.DONE } },
    { var = "roster_last",  icon = "TIME", col = 0, row = 1, states = { S.DONE } },
    { var = "seed",         icon = "TIME", col = 1, row = 1, states = { S.DONE } },
  },
  winners = { { var = "lives", dir = "max" } },
  totem_slots = {}, teams = 0,

  on_begin = function(vars)
    local ids = la.roster()
    vars.roster_n     = #ids
    vars.roster_first = ids[1] or 0
    vars.roster_last  = ids[#ids] or 0
    vars.seed         = la.session_seed()
  end,
  rules = {},
  update = {},
}
