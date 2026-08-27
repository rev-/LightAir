-- ================================================================
-- LightAir game: King of Hill — FFA interactions plus CP totems.
--
-- Port of the retired native C++ ruleset (deleted in the Lua
-- migration; the original is in git history).
--
-- No host-assigned teams: every player is their own "slot"
-- (player id - 1), which maps one-to-one to the CP presence
-- sub-type and the CP score payload.  Hold a CP alone for 10 s and
-- it broadcasts a point for your slot.  Respawn at a teamless BASE.
-- Most CP points wins; tie-break: fewest shone.
-- ================================================================

local std = la.lib("std")

local S   = { IN_GAME = 0, OUT_GAME = 1, GAME_END = 2 }
local MSG = la.msg
local R   = { TAKEN = 1, SHONE = 2, DOWN = 3, IMMUNE = 4 }

local NEAR_CP_RSSI   = -65      -- ~3 m: CP presence gate
local NEAR_BASE_RSSI = -57      -- ~2 m: BASE respawn gate
local PICKUP_RSSI    = -57      -- ~2 m: BONUS/MALUS claim gate
local CP_NONE        = 0xFF

-- ---- Private state ------------------------------------------------
local my_slot     = 0           -- player id - 1; set in on_begin
local cp_ids      = {}          -- [i] = device id of the i-th CP totem
local cp_owner    = {}          -- [i] = last announced owner slot or CP_NONE
local respawn_at  = 0
local can_respawn = false
local shone_by    = nil         -- short name of whoever put us down
local imm         = std.immunity(3000)
local shiner      = std.shiner{ energy = "energy", spent = "energy_spent",
                                max = "start_energy", recharge = "recharge_secs" }

local function cp_index(sender)
  for i, id in ipairs(cp_ids) do
    if id == sender then return i end
  end
  return nil
end

-- CP beacon: track ownership changes (both states); declare presence
-- by returning slot+1 only when send_presence and close enough.
local function cp_beacon_handler(send_presence)
  return function(vars, pkt)
    local idx = cp_index(pkt.sender)
    if not idx or pkt.len < 1 then return end

    local owner = pkt:byte(1)
    if owner ~= cp_owner[idx] then
      cp_owner[idx] = owner
      if owner == CP_NONE then
        la.show(string.format("CP %d neutral", idx), 3000)
      else
        la.show(string.format("CP %d -> P%d!", idx, owner + 1), 3000)
        la.ui(owner == my_slot and "FlagReturn" or "FlagTaken")
      end
    end

    if send_presence and pkt.rssi >= NEAR_CP_RSSI then
      return my_slot + 1
    end
  end
end

local function cp_score_handler(vars, pkt)
  local idx = cp_index(pkt.sender)
  if not idx or pkt.len < 1 then return end
  if pkt:byte(1) == my_slot then
    vars.points = vars.points + 1
    -- The point IS the game here, so it gets its own cue and names the
    -- hill that paid it: the score cell alone ticks by unnoticed.
    la.show(string.format("CP %d +1", idx), 2000)
    la.ui("ControlGain")
  end
end

local function game_over()
  la.show("Game over!", 3000)
  la.ui("EndGame")
end

return {
  api     = 1,
  type_id = 0x0006,               -- GameTypeId::KING_OF_HILL
  name    = "King of Hill",

  initial_state = S.IN_GAME,
  scoring_state = S.GAME_END,
  score_msg     = MSG.SCORE_COLLECT,

  config = {
    { id = "start_lives",   name = "Lives",     min = 1,  max = 5,   step = 1,  default = 3   },
    { id = "respawn_secs",  name = "Respawn",   min = 5,  max = 120, step = 5,  default = 30  },
    { id = "start_energy",  name = "Energy",    min = 10, max = 100, step = 10, default = 50  },
    { id = "recharge_secs", name = "Recharge",  min = 0,  max = 20,  step = 5,  default = 10  },
    { id = "game_time",     name = "Time",      min = 60, max = 900, step = 60, default = 900 },
    { id = "end_points",    name = "EndPoints", min = 0,  max = 100, step = 20, default = 0   },
  },

  vars = {
    { id = "lives",        default = 3  },
    { id = "energy",       default = 50 },
    { id = "time_left",    default = 900, countdown_in = { S.IN_GAME, S.OUT_GAME } },
    { id = "points",       default = 0  },
    { id = "energy_spent", default = 0  },
    { id = "shone_times",  default = 0  },
  },

  monitor = {
    { var = "lives",        icon = "LIFE",   col = 0, row = 0, states = { S.IN_GAME } },
    { var = "energy",       icon = "ENERGY", col = 1, row = 0, states = { S.IN_GAME } },
    { var = "time_left",    icon = "TIME",   col = 0, row = 1, states = { S.IN_GAME, S.OUT_GAME } },
    { var = "points",       icon = "SCORE",  col = 1, row = 1, states = { S.IN_GAME } },
    { var = "game_time",    icon = "TIME",   col = 0, row = 0, states = { S.GAME_END } },
    { var = "points",       icon = "SCORE",  col = 1, row = 0, states = { S.GAME_END } },
    { var = "energy_spent", icon = "ENERGY", col = 0, row = 1, states = { S.GAME_END } },
    { var = "shone_times",  icon = "LIFE",   col = 1, row = 1, states = { S.GAME_END } },
  },

  winners = {
    { var = "points",      dir = "max" },
    { var = "shone_times", dir = "min" },
  },

  totem_slots = {
    { role = "CP",    min = 1, max = 6 },      -- at least one hill
    { role = "BASE",  min = 1, max = 4 },      -- teamless respawn base
    { role = "BONUS", min = 0, max = 16 },
    { role = "MALUS", min = 0, max = 16 },
  },
  teams = 0,
  time_left_var = "time_left",

  on_begin = function(vars)
    vars.lives     = vars.start_lives
    vars.energy    = vars.start_energy
    vars.time_left = vars.game_time
    vars.points       = 0
    vars.energy_spent = 0
    vars.shone_times  = 0
    my_slot     = la.my_id() - 1
    respawn_at  = 0
    can_respawn = false
    shone_by    = nil
    imm.reset()
    shiner.reset()
    cp_ids, cp_owner = {}, {}
    for i = 0, 5 do
      local id = la.totem_for_role("CP", i)
      if id == 0 then break end
      cp_ids[#cp_ids + 1]   = id
      cp_owner[#cp_owner + 1] = CP_NONE
    end
    la.ui("GameStart")
  end,

  on_message = {
    [S.IN_GAME] = {
      -- A pickup totem gives itself to whoever answers, so only answer
      -- from arm's length: the claim has to mean "I am standing at it".
      [MSG.BONUS_BEACON] = std.pickup_claim{ rssi = PICKUP_RSSI },
      [MSG.MALUS_BEACON] = std.pickup_claim{ rssi = PICKUP_RSSI },
      [MSG.LIT] = std.lit_target{
        lives = "lives", immunity = imm,
        reply = { taken = R.TAKEN, shone = R.SHONE, immune = R.IMMUNE },
        on_shone = function(_, pkt) shone_by = la.player_short(pkt.sender) end,
      },
      [MSG.CP_BEACON] = cp_beacon_handler(true),
      [MSG.CP_SCORE]  = cp_score_handler,
    },
    [S.OUT_GAME] = {
      [MSG.LIT]       = function() return R.DOWN end,
      [MSG.CP_BEACON] = cp_beacon_handler(false),   -- track only; can't capture
      [MSG.CP_SCORE]  = cp_score_handler,
      [MSG.BASE_BEACON] = std.base_respawn{
        when     = function() return la.now() >= respawn_at end,
        teamless = true,                            -- teamless bases only
        rssi     = NEAR_BASE_RSSI,
        on_ready = function() can_respawn = true end,
      },
    },
  },

  on_reply = {
    [MSG.LIT] = {
      [R.TAKEN]  = function() la.ui("Taken")  end,
      [R.SHONE]  = function(vars, reply)
        la.show(la.player_short(reply.sender) .. " SHONE!", 3000)
        la.ui("Lit")
      end,
      [R.IMMUNE] = function() la.ui("Immune") end,
    },
  },

  rules = {
    { from = S.IN_GAME, to = S.GAME_END,
      when   = function(vars) return vars.time_left <= 0 end,
      action = game_over },
    { from = S.IN_GAME, to = S.GAME_END,
      when   = function(vars) return vars.end_points > 0 and vars.points >= vars.end_points end,
      action = game_over },
    { from = S.IN_GAME, to = S.OUT_GAME,
      when   = function(vars) return vars.lives <= 0 end,
      action = function(vars)
        vars.shone_times = vars.shone_times + 1
        respawn_at  = la.now() + vars.respawn_secs * 1000
        can_respawn = false
        -- Two persistent lines for the whole wait, credit on top: who put
        -- us down, and what to do about it.  The "Down" cue is the moment
        -- feedback, so no transient line competes for the tray.
        la.show("Go to base", 0)
        la.show("LIT by " .. (shone_by or "?"), 0)
        la.ui("Down")
      end },
    { from = S.OUT_GAME, to = S.GAME_END,
      when   = function(vars) return vars.time_left <= 0 end,
      action = game_over },
    { from = S.OUT_GAME, to = S.GAME_END,
      when   = function(vars) return vars.end_points > 0 and vars.points >= vars.end_points end,
      action = game_over },
    { from = S.OUT_GAME, to = S.IN_GAME,
      when   = function() return can_respawn end,
      action = function(vars)
        vars.lives  = vars.start_lives
        vars.energy = vars.start_energy
        can_respawn = false
        shone_by    = nil
        imm.reset()
        la.clear_tray()             -- drop the credit and the instruction
        la.show("Back in game!", 1000)
        la.ui("Up")
      end },
  },

  update = {
    [S.IN_GAME] = function(vars)
      -- Everyone is a valid target: no team check needed.
      local target = la.shine_lit()
      if target then la.send(target, MSG.LIT) end
      shiner.tick(vars)
    end,
  },

  totems = {
    CP    = std.totems.cp(),
    BASE  = std.totems.base("any"),
    BONUS = std.totems.bonus(),
    MALUS = std.totems.malus(),
  },
}
