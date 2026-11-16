-- Library-inbox fixture for the host test: not a playable game.
--
-- A library message (the 0x20 block: MSG.LOCK, MSG.TRACE) that no ruleset
-- handler takes is kept by the runner for the library that owns it, which
-- reads it with la.inbox() on its tick.  This ruleset stands in for such a
-- library: its update drains the inbox into vars the test can read.  In
-- OWN it declares a handler for MSG.LOCK of its own — and a declared
-- handler wins, so that message never reaches the inbox there.
--
-- It also carries two owned bars, to show la.bar_mark puts a wait's marker
-- on the bar that times THAT wait and on no other.
local S = { PLAY = 0, OWN = 1, DONE = 2, PARK = 3 }

local function drain(vars)
  la.bar_mark("wait_from", vars.mark_cmd)
  if vars.drain == 0 then return end
  local msg, sender, team, rssi, b1 = la.inbox()
  while msg do
    vars.got         = vars.got + 1
    vars.last_msg    = msg
    vars.last_sender = sender
    vars.last_rssi   = rssi
    vars.last_b1     = b1
    msg, sender, team, rssi, b1 = la.inbox()
  end
end

return {
  api = 1, type_id = 0x7F0C, name = "Inbox",
  initial_state = S.PLAY,
  scoring_state = S.DONE,

  config = {},
  vars = {
    { id = "got",         default = 0 },
    { id = "last_msg",    default = 0 },
    { id = "last_sender", default = 0 },
    { id = "last_rssi",   default = 0 },
    { id = "last_b1",     default = 0 },
    { id = "handled",     default = 0 },
    { id = "drain",       default = 1 },
    { id = "go",          default = 0 },
    { id = "mark_cmd",    default = 0 },
    { id = "zero",        default = 0 },
    { id = "wait_from",   default = 0 },
    { id = "other_from",  default = 0 },
    { id = "wait_ms",     default = 5000 },
  },
  monitor = {
    { var = "zero", icon = "TIME", col = 0, row = 0, states = { S.PLAY, S.OWN },
      bar = true, bar_at = 0, fill_var = "wait_ms", start_var = "wait_from" },
    { var = "zero", icon = "TIME", col = 1, row = 0, states = { S.PLAY, S.OWN },
      bar = true, bar_at = 0, fill_var = "wait_ms", start_var = "other_from" },
    -- Parked on screens nobody sees, only so the host test can reach the slots.
    { var = "got",         icon = "SCORE", col = 0, row = 0, states = { S.DONE } },
    { var = "last_msg",    icon = "SCORE", col = 1, row = 0, states = { S.DONE } },
    { var = "last_sender", icon = "SCORE", col = 0, row = 1, states = { S.DONE } },
    { var = "last_rssi",   icon = "SCORE", col = 1, row = 1, states = { S.DONE } },
    { var = "last_b1",     icon = "SCORE", col = 0, row = 0, states = { S.PARK } },
    { var = "handled",     icon = "SCORE", col = 1, row = 0, states = { S.PARK } },
    { var = "drain",       icon = "SCORE", col = 0, row = 1, states = { S.PARK } },
    { var = "go",          icon = "SCORE", col = 1, row = 1, states = { S.PARK } },
    { var = "mark_cmd",    icon = "SCORE", col = 0, row = 0, states = { S.PARK } },
    { var = "wait_from",   icon = "SCORE", col = 1, row = 0, states = { S.PARK } },
    { var = "other_from",  icon = "SCORE", col = 0, row = 1, states = { S.PARK } },
  },
  winners = { { var = "got", dir = "max" } },
  totem_slots = {}, teams = 0,

  on_begin = function(vars) end,

  on_message = {
    [S.OWN] = {
      [la.msg.LOCK] = function(vars, pkt) vars.handled = vars.handled + 1 end,
    },
  },

  rules = {
    { from = S.PLAY, to = S.OWN, when = function(vars) return vars.go == 1 end },
  },
  update = {
    [S.PLAY] = drain,
    [S.OWN]  = drain,
  },
}
