-- Input-surface fixture for the host test: not a playable game, just a
-- ruleset that mirrors everything the keypad verbs report into vars so
-- the test can assert on it.  It names no key layout of its own beyond
-- the two it asks about by label — the count and the first entry come
-- from whatever the InputReport happens to hold.
local S = { WATCH = 0, DONE = 1 }

return {
  api = 1, type_id = 0x7F04, name = "Keys",
  initial_state = S.WATCH,
  scoring_state = S.DONE,

  config = {},
  vars = {
    { id = "active",  default = 0 },                            -- keys the report holds
    { id = "a_down",  default = 0 },                            -- la.key_down("A")
    { id = "pad",     default = -1 },                           -- keypad id of entry 1
    { id = "first",   text = true, len = 16, default = "-" },   -- "<key>:<state>" of entry 1
    { id = "b_state", text = true, len = 16, default = "-" },   -- la.key_state("B")
  },
  monitor = {
    { var = "active",  icon = "LIFE", col = 0, row = 0, states = { S.WATCH } },
    { var = "a_down",  icon = "LIFE", col = 1, row = 0, states = { S.WATCH } },
    { var = "first",   icon = "ROLE", col = 0, row = 1, states = { S.WATCH } },
    { var = "b_state", icon = "ROLE", col = 1, row = 1, states = { S.WATCH } },
    -- Parked on the other screen only so the host test can reach the slot.
    { var = "pad",     icon = "SCORE", col = 0, row = 0, states = { S.DONE } },
  },
  winners = { { var = "active", dir = "max" } },
  totem_slots = {}, teams = 0,

  on_begin = function(vars) vars.active = 0 end,

  rules = {
    -- A chord on keys this ruleset picked, on any keypad.
    { from = S.WATCH, to = S.DONE,
      when = function() return la.key_down("<") and la.key_down(">") end },
  },

  update = {
    [S.WATCH] = function(vars)
      local n = 0
      while la.key_at(n + 1) do n = n + 1 end
      vars.active  = n
      vars.a_down  = la.key_down("A") and 1 or 0
      vars.b_state = la.key_state("B")
      local key, state, pad = la.key_at(1)
      vars.first = key and (key .. ":" .. state) or "-"
      vars.pad   = pad or -1
    end,
  },
}
