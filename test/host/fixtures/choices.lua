-- Config-choices fixture for the host test: not a playable game, just a
-- ruleset whose config menu lists values with labels, beside an ordinary
-- numeric entry, so the test can assert the descriptor, the menu stepping
-- and the config blob.
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
  vars = {},
  monitor = {
    { var = "lives", icon = "LIFE", col = 0, row = 0, states = { S.IN_GAME } },
  },
  winners = { { var = "lives", dir = "max" } },
  totem_slots = {}, teams = 0,

  on_begin = function(vars) end,
  rules = {},
  update = {},
}
