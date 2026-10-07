-- In-game hold fixture for the host test: not a playable game.  It counts
-- what reaches it, so the test can tell what a held player still receives
-- (hold.accept narrows it to LIT), that the update body stops while held,
-- that the declarative clock does not, and that the hold hooks bracket it.
local S = { PLAY = 0, DONE = 1 }

-- An area hit is a LIT, so hold.accept = { LIT } lets it through too.
la.area_policy(50, { bands = { { -60, 1 } } })

return {
  api = 1, type_id = 0x7F06, name = "Hold",
  initial_state = S.PLAY,
  scoring_state = S.DONE,

  config = {},
  vars = {
    { id = "lits",     default = 0 },
    { id = "reports",  default = 0 },
    { id = "updates",  default = 0 },
    { id = "entered",  default = 0 },
    { id = "exited",   default = 0 },
    { id = "clock",    default = 100, countdown_in = { S.PLAY } },
  },
  monitor = {
    { var = "lits",     icon = "LIFE",  col = 0, row = 0, states = { S.PLAY } },
    { var = "reports",  icon = "LIFE",  col = 1, row = 0, states = { S.PLAY } },
    { var = "updates",  icon = "SCORE", col = 0, row = 1, states = { S.PLAY } },
    { var = "clock",    icon = "TIME",  col = 1, row = 1, states = { S.PLAY } },
    { var = "entered",  icon = "ROLE",  col = 0, row = 0, states = { S.DONE } },
    { var = "exited",   icon = "ROLE",  col = 1, row = 0, states = { S.DONE } },
  },
  winners = { { var = "lits", dir = "max" } },
  totem_slots = {}, teams = 0,

  on_message = {
    [S.PLAY] = {
      [la.msg.LIT]    = function(vars) vars.lits = vars.lits + 1; return 1 end,
      [la.msg.POINT_REPORT] = function(vars) vars.reports = vars.reports + 1 end,
    },
  },

  update = {
    [S.PLAY] = function(vars) vars.updates = vars.updates + 1 end,
  },

  hold = {
    accept   = { la.msg.LIT },
    on_enter = function(vars) vars.entered = vars.entered + 1 end,
    on_exit  = function(vars) vars.exited  = vars.exited + 1 end,
  },
}
