-- Area fixture for the host test: not a playable game.  It declares an
-- area effect no projector triggers and starts it from its own LIT
-- handler, the way a ruleset bursts on its own terms, then counts what
-- came of it: a real hit emits, an area hit is refused (an area effect
-- never starts another), and a declaration after the load is refused.
-- An inner-band hit knocks out, so the service sends a credit; on_reply
-- keys on the credit's type to catch its acknowledgement or timeout
-- leaking into the ruleset.  Its hold takes no LIT, so no area hit.
local S = { PLAY = 0, DONE = 1 }

la.area_policy(40, { bands = { { -55, 2 }, { -60, 1 } } })
-- Teammates never, the originator itself yes, and no credit.
la.area_policy(42, { bands = { { -55, 2 } }, friendly = "never", self = true, credit = false })

return {
  api = 1, type_id = 0x7F0B, name = "Area",
  initial_state = S.PLAY,
  scoring_state = S.DONE,

  config = {},
  vars = {
    { id = "hits",      default = 0 },
    { id = "area_hits", default = 0 },
    { id = "emitted",   default = 0 },
    { id = "refused",   default = 0 },
    { id = "late",      default = 0 },
    { id = "leaks",     default = 0 },
  },
  monitor = {
    { var = "hits",      icon = "LIFE",  col = 0, row = 0, states = { S.PLAY } },
    { var = "area_hits", icon = "LIFE",  col = 1, row = 0, states = { S.PLAY } },
    { var = "emitted",   icon = "SCORE", col = 0, row = 1, states = { S.PLAY } },
    { var = "refused",   icon = "SCORE", col = 1, row = 1, states = { S.PLAY } },
    { var = "late",      icon = "ROLE",  col = 0, row = 0, states = { S.DONE } },
    { var = "leaks",     icon = "ROLE",  col = 1, row = 0, states = { S.DONE } },
  },
  winners = { { var = "hits", dir = "max" } },
  totem_slots = {}, teams = 0,

  on_begin = function(vars)
    local ok = pcall(la.area_policy, 41, { bands = { { -60, 1 } } })
    vars.late = ok and 0 or 1
  end,

  on_message = {
    [S.PLAY] = {
      [la.msg.LIT] = function(vars, pkt)
        vars.hits = vars.hits + 1
        if pkt.area then vars.area_hits = vars.area_hits + 1 end
        if la.area_emit(40) then vars.emitted = vars.emitted + 1
        else vars.refused = vars.refused + 1 end
        if pkt.area and pkt:byte(1) >= 2 then return la.hit.SHONE end
        return la.hit.TAKEN
      end,
    },
  },

  -- Held, this ruleset takes no LIT, so no area hit either.
  hold = { accept = { la.msg.POINT_REPORT } },

  on_reply = {
    [0x1A] = {                                  -- MSG_AREA_CREDIT
      [0]     = function(vars) vars.leaks = vars.leaks + 1 end,
      timeout = function(vars) vars.leaks = vars.leaks + 1 end,
    },
  },
}
