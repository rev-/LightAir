-- Fault-injection fixture for the host test: a minimal valid game whose
-- update handler always errors and whose lit handler errors *after* a
-- partial mutation (the documented caveat: pre-error effects stand).
local S = { IN_GAME = 0, DONE = 1 }

return {
  api = 1, type_id = 0x7F01, name = "Faulty",
  initial_state = S.IN_GAME,
  scoring_state = S.DONE,

  config  = {},
  vars    = { { id = "lives", default = 3 } },
  monitor = { { var = "lives", icon = "LIFE", col = 0, row = 0, states = { S.IN_GAME } } },
  winners = { { var = "lives", dir = "max" } },
  totem_slots = {}, teams = 0,

  on_begin = function(vars) vars.lives = 3 end,

  on_message = {
    [S.IN_GAME] = {
      [la.msg.LIT] = function(vars, pkt)
        vars.lives = vars.lives - 1     -- partial effect, applied...
        error("boom in handler")        -- ...then the handler dies
      end,
    },
  },

  rules = {
    { from = S.IN_GAME, to = S.DONE,
      when = function(vars) return vars.lives <= 0 end },
  },

  update = {
    [S.IN_GAME] = function(vars)
      local x = nil
      return x.y                        -- errors every tick
    end,
  },
}
