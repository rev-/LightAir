-- ================================================================
-- LightAir game: Upkeep — two-team control-point game.
--
-- Port of the retired native C++ ruleset (deleted in the Lua
-- migration; the original is in git history).
--
-- CP totems attach to whichever team is alone nearby during a 2 s
-- beacon window and broadcast a point for the owning team every
-- 10 s of unchallenged control.  Players track both team totals
-- locally and the display shows "myPts/enemyPts" as a text var.
-- Team with the higher CP total wins; tie-break: fewer shone.
-- ================================================================

local std  = la.lib("std")
local proj = la.lib("projector")

-- The baseline profile reproduces what std.shiner did here: one
-- energy per beam, a full refill after the configured idle, both
-- read from this game's own config vars.
proj.define{ vars = { energy = "energy", spent = "energy_spent",
                      reload = "reload", reload_ms = "reload_ms" } }

local S   = { IN_GAME = 0, OUT_GAME = 1, GAME_END = 2 }
local MSG = la.msg
local R   = { TAKEN = 1, SHONE = 2, DOWN = 3, FRIEND = 4, IMMUNE = 5 }

local NEAR_CP_RSSI   = -65
local NEAR_BASE_RSSI = -57
local PICKUP_RSSI    = -57      -- ~2 m: BONUS/MALUS claim gate
local CP_NONE        = 0xFF

-- ---- Private state ------------------------------------------------
local my_team       = 0
local team_o_points = 0
local team_x_points = 0
local cp_ids        = {}
local cp_owner      = {}
local respawn_at    = 0
local can_respawn   = false
local shone_by      = nil       -- short name of whoever put us down
local imm           = std.immunity(3000)

local function is_opponent(id) return la.team_of(id) ~= my_team end
local function friendly(pkt)   return pkt.team == my_team and vars.friendly_fire == 0 end

local function cp_index(sender)
  for i, id in ipairs(cp_ids) do
    if id == sender then return i end
  end
  return nil
end

local function refresh_score_str(vars)
  local mine  = (my_team == 0) and team_o_points or team_x_points
  local other = (my_team == 0) and team_x_points or team_o_points
  vars.score_str   = string.format("%d/%d", mine, other)
  vars.team_points = mine                    -- winner var
end

-- CP beacon: track ownership (both states); presence reply IN_GAME only.
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
        la.show(string.format("CP %d->Team %s!", idx, la.team_short(owner)), 3000)
        la.ui(owner == my_team and "FlagReturn" or "FlagTaken")
      end
    end

    if send_presence and pkt.rssi >= NEAR_CP_RSSI then
      return my_team + 1        -- 1 = team O present, 2 = team X present
    end
  end
end

local function cp_score_handler(vars, pkt)
  local idx = cp_index(pkt.sender)
  if not idx or pkt.len < 1 then return end
  local team = pkt:byte(1)
  if team == 0 then     team_o_points = team_o_points + 1
  elseif team == 1 then team_x_points = team_x_points + 1
  else return end
  refresh_score_str(vars)
  if team == my_team then
    -- The point IS the game here, so it gets its own cue and names the
    -- hill that paid it: the score cell alone ticks by unnoticed.
    la.show(string.format("CP %d +1", idx), 2000)
    la.ui("ControlGain")
  else
    -- Worth knowing, not worth a cue every emission period: the tray
    -- line and the score cell carry the other side's points.
    la.show(string.format("CP %d -> %s", idx, la.team_short(team)), 2000)
  end
end

local function game_over()
  la.show("Game over!", 3000)
  la.ui("EndGame")
end

local function limit_reached(vars)
  return vars.end_points > 0
     and (team_o_points + team_x_points) >= vars.end_points
end

return {
  api     = 1,
  type_id = 0x0005,               -- GameTypeId::UPKEEP
  name    = "Upkeep",

  initial_state = S.IN_GAME,
  scoring_state = S.GAME_END,
  score_msg     = MSG.SCORE_COLLECT,

  config = {
    { id = "start_lives",   name = "Lives",        min = 1,  max = 5,   step = 1,  default = 3   },
    { id = "respawn_secs",  name = "Respawn",      min = 5,  max = 120, step = 5,  default = 30  },
    { id = "start_energy",  name = "Energy",       min = 10, max = 100, step = 10, default = 50  },
    { id = "recharge_secs", name = "Recharge",     min = 0,  max = 20,  step = 5,  default = 10  },
    { id = "game_time",     name = "Time",         min = 60, max = 900, step = 60, default = 900 },
    { id = "friendly_fire", name = "FriendlyFire", min = 0,  max = 1,   step = 1,  default = 0   },
    { id = "end_points",    name = "EndPoints",    min = 0,  max = 500, step = 50, default = 150 },
  },

  vars = {
    { id = "lives",        default = 3  },
    { id = "energy",       default = 50 },
    { id = "time_left",    default = 900, countdown_in = { S.IN_GAME, S.OUT_GAME } },
    { id = "team_points",  default = 0  },              -- my team's CP total (winner var)
    { id = "energy_spent", default = 0  },
    -- The projector's reload clock, read by the energy cell's bar:
    -- reload = millis the wait began (0 = not waiting), reload_ms =
    -- how long it takes.  Both written by projector.lua.
    { id = "reload",       default = 0 },
    { id = "reload_ms",    default = 0 },
    { id = "shone_times",  default = 0  },
    -- Text slot: bound to the LCD like an int slot, but a char buffer.
    { id = "score_str",    text = true, len = 8, default = "0/0" },
  },

  monitor = {
    { var = "lives",        icon = "LIFE",   col = 0, row = 0, states = { S.IN_GAME } },
    -- Energy, and — while the pool is empty — a bar filling over the
    -- recharge.  The projector owns both the duration and the instant
    -- the wait began, because a refill starts at the trigger's RELEASE,
    -- not when the pool hit zero.
    { var = "energy",       icon = "ENERGY", col = 1, row = 0, states = { S.IN_GAME },
      bar = true, bar_at = 0, fill_var = "reload_ms", start_var = "reload" },
    { var = "time_left",    icon = "TIME",   col = 0, row = 1, states = { S.IN_GAME, S.OUT_GAME } },
    { var = "score_str",    icon = "SCORE",  col = 1, row = 1, states = { S.IN_GAME } },
    { var = "game_time",    icon = "TIME",   col = 0, row = 0, states = { S.GAME_END } },
    { var = "score_str",    icon = "SCORE",  col = 1, row = 0, states = { S.GAME_END } },
    { var = "energy_spent", icon = "ENERGY", col = 0, row = 1, states = { S.GAME_END } },
    { var = "shone_times",  icon = "LIFE",   col = 1, row = 1, states = { S.GAME_END } },
  },

  winners = {
    { var = "team_points", dir = "max" },
    { var = "shone_times", dir = "min" },
  },

  -- "max" aggregation: every player reports its local view of the team
  -- total; taking the max tolerates missed MSG.CP_SCORE broadcasts.
  on_score_announce = std.team_announce{ primary = "max" },

  totem_slots = {
    { role = "CP",     min = 3, max = 6 },
    { role = "BASE_O", min = 1, max = 3 },
    { role = "BASE_X", min = 1, max = 3 },
    { role = "BASE",   min = 0, max = 3 },
    { role = "BONUS",  min = 0, max = 16 },
    { role = "MALUS",  min = 0, max = 16 },
  },
  teams = 2,
  time_left_var = "time_left",

  on_begin = function(vars)
    vars.lives     = vars.start_lives
    vars.energy    = vars.start_energy
    vars.time_left = vars.game_time
    vars.team_points  = 0
    vars.energy_spent = 0
    vars.shone_times  = 0
    my_team       = la.my_team()
    team_o_points = 0
    team_x_points = 0
    respawn_at    = 0
    can_respawn   = false
    shone_by      = nil
    imm.reset()
    proj.reset(vars)
    cp_ids, cp_owner = {}, {}
    for i = 0, 5 do
      local id = la.totem_for_role("CP", i)
      if id == 0 then break end
      cp_ids[#cp_ids + 1]     = id
      cp_owner[#cp_owner + 1] = CP_NONE
    end
    refresh_score_str(vars)
    la.ui("GameStart")
  end,

  on_message = {
    [S.IN_GAME] = {
      -- A pickup totem gives itself to whoever answers, so only answer
      -- from arm's length: the claim has to mean "I am standing at it".
      [MSG.BONUS_BEACON] = std.pickup_claim{ rssi = PICKUP_RSSI },
      [MSG.MALUS_BEACON] = std.pickup_claim{ rssi = PICKUP_RSSI },
      [MSG.LIT] = std.lit_target{
        lives = "lives", immunity = imm, friendly = friendly,
        reply = { taken = R.TAKEN, shone = R.SHONE,
                  friend = R.FRIEND, immune = R.IMMUNE },
        on_shone = function(_, pkt) shone_by = la.player_short(pkt.sender) end,
      },
      [MSG.CP_BEACON] = cp_beacon_handler(true),
      [MSG.CP_SCORE]  = cp_score_handler,
    },
    [S.OUT_GAME] = {
      [MSG.LIT]       = function() return R.DOWN end,
      [MSG.CP_BEACON] = cp_beacon_handler(false),
      [MSG.CP_SCORE]  = cp_score_handler,
      [MSG.BASE_BEACON] = std.base_respawn{
        when     = function() return la.now() >= respawn_at end,
        team     = function() return my_team end,
        teamless = true,
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
      [R.FRIEND] = function() la.ui("Friend") end,
      [R.IMMUNE] = function() la.ui("Immune") end,
    },
  },

  rules = {
    { from = S.IN_GAME, to = S.GAME_END,
      when   = function(vars) return vars.time_left <= 0 end,
      action = game_over },
    { from = S.IN_GAME, to = S.GAME_END,
      when   = limit_reached,
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
      when   = limit_reached,
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
      local target = proj.result(vars)
      if target and (is_opponent(target) or vars.friendly_fire == 1) then
        la.send(target, MSG.LIT, proj.payload(vars))
      end
      proj.tick(vars)
    end,
  },

  totems = {
    CP     = std.totems.cp(),
    BASE_O = std.totems.base(0),
    BASE_X = std.totems.base(1),
    BASE   = std.totems.base("any"),
    BONUS  = std.totems.bonus(),
    MALUS  = std.totems.malus(),
  },
}
