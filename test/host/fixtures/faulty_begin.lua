-- Fixture: on_begin fails -> the game must refuse to play
-- (forced into scoring_state).
return {
  api = 1, type_id = 0x7F02, name = "FaultyBegin",
  initial_state = 0,
  scoring_state = 1,
  config = {}, vars = { { id = "x", default = 0 } },
  monitor = {}, winners = {}, totem_slots = {}, teams = 0,
  on_begin = function(vars) error("cannot start") end,
  rules = {}, update = {},
}
