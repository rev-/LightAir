-- ================================================================
-- LightAir game: Teams — two-team match, O vs X.
--
-- Port of src/rulesets/GameTeams.cpp.  Uses the standard library
-- (games/lib/std.lua) for the recurring idioms; compare with
-- games/freeforall.lua, which spells everything out.
--
-- Eliminating an opponent scores 1 point and broadcasts a point
-- report so teammates track the aggregate.  Respawn requires the
-- respawn timer AND proximity to an own-team (or teamless) BASE.
-- Team with most aggregate points wins; tie-break: fewest shone.
-- ================================================================

local std = la.lib("std")

local S   = { IN_GAME = 0, OUT_GAME = 1, GAME_END = 2 }
local MSG = la.msg
local R   = { TAKEN = 1, SHONE = 2, DOWN = 3, FRIEND = 4, IMMUNE = 5 }

local NEAR_BASE_RSSI = -57      -- ~2 m indoors: BASE respawn gate

-- ---- Private state ------------------------------------------------
local my_team      = 0          -- read from the runner in on_begin
local team_points  = 0          -- aggregate team points known locally
local respawn_at   = 0
local can_respawn  = false
local imm          = std.immunity(3000)
local shiner       = std.shiner{ energy = "energy", spent = "energy_spent",
                                 max = "start_energy", recharge = "recharge_secs" }

local function is_opponent(id)  return la.team_of(id) ~= my_team end

-- Reject same-team lits unless friendly fire is enabled.
local function friendly(pkt)
  return pkt.team == my_team and vars.friendly_fire == 0
end

return {
  api     = 1,
  type_id = 0x0002,               -- GameTypeId::TEAMS
  name    = "Teams",

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
    { id = "end_points",    name = "EndPoints",    min = 0,  max = 50,  step = 5,  default = 0   },
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

  -- Team aggregate: sum of points per team, tie-break fewest shone.
  on_score_announce = std.team_announce{ primary = "sum" },

  totem_slots = {
    { role = "BASE_O", min = 0, max = 4 },
    { role = "BASE_X", min = 0, max = 4 },
    { role = "BASE",   min = 0, max = 4 },
    { role = "BONUS",  min = 0, max = 16 },
    { role = "MALUS",  min = 0, max = 16 },
  },
  teams = 2,
  time_left_var = "time_left",

  on_begin = function(vars)
    vars.lives     = vars.start_lives
    vars.energy    = vars.start_energy
    vars.time_left = vars.game_time
    vars.points       = 0
    vars.energy_spent = 0
    vars.shone_times  = 0
    my_team      = la.my_team()
    team_points  = 0
    respawn_at   = 0
    can_respawn  = false
    imm.reset()
    shiner.reset()
    la.ui("GameStart")
  end,

  on_message = {
    [S.IN_GAME] = {
      [MSG.LIT] = std.lit_target{
        lives = "lives", immunity = imm, friendly = friendly,
        reply = { taken = R.TAKEN, shone = R.SHONE,
                  friend = R.FRIEND, immune = R.IMMUNE },
      },
      [MSG.POINT_REPORT] = function(vars, pkt)
        if la.team_of(pkt.sender) == my_team then
          team_points = team_points + 1
        end
      end,
    },
    [S.OUT_GAME] = {
      [MSG.LIT] = function() return R.DOWN end,
      [MSG.POINT_REPORT] = function(vars, pkt)
        if la.team_of(pkt.sender) == my_team then
          team_points = team_points + 1
        end
      end,
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
      [R.FRIEND] = function() la.ui("Friend") end,
      [R.IMMUNE] = function() la.ui("Immune") end,
      [R.SHONE]  = function(vars)
        vars.points = vars.points + 1
        team_points = team_points + 1
        la.broadcast_relay(MSG.POINT_REPORT)
        la.ui("Lit")
      end,
    },
  },

  rules = {
    { from = S.IN_GAME, to = S.GAME_END,
      when   = function(vars) return vars.time_left <= 0 end,
      action = function() la.show("Game over!", 3000); la.ui("EndGame") end },
    { from = S.IN_GAME, to = S.GAME_END,
      when   = function(vars) return vars.end_points > 0 and team_points >= vars.end_points end,
      action = function() la.show("Game over!", 3000); la.ui("EndGame") end },
    { from = S.IN_GAME, to = S.OUT_GAME,
      when   = function(vars) return vars.lives <= 0 end,
      action = function(vars)
        vars.shone_times = vars.shone_times + 1
        respawn_at  = la.now() + vars.respawn_secs * 1000
        can_respawn = false
        la.show("Shone!", 2000)
        la.ui("Down")
      end },
    { from = S.OUT_GAME, to = S.GAME_END,
      when   = function(vars) return vars.time_left <= 0 end,
      action = function() la.show("Game over!", 3000); la.ui("EndGame") end },
    { from = S.OUT_GAME, to = S.GAME_END,
      when   = function(vars) return vars.end_points > 0 and team_points >= vars.end_points end,
      action = function() la.show("Game over!", 3000); la.ui("EndGame") end },
    { from = S.OUT_GAME, to = S.IN_GAME,
      when   = function() return can_respawn end,
      action = function(vars)
        vars.lives  = vars.start_lives
        vars.energy = vars.start_energy
        can_respawn = false
        imm.reset()
        la.show("Back in game!", 1000)
        la.ui("Up")
      end },
  },

  update = {
    [S.IN_GAME] = function(vars)
      local target = la.shine_lit()
      if target and (is_opponent(target) or vars.friendly_fire == 1) then
        la.send(target, MSG.LIT)
      end
      shiner.tick(vars)
    end,
    -- OUT_GAME: respawn gating happens in on_message[BASE_BEACON];
    -- the countdown is declarative.  Nothing to do per tick.
  },

  totems = {
    BASE_O = std.totems.base(0),
    BASE_X = std.totems.base(1),
    BASE   = std.totems.base("any"),
    BONUS  = std.totems.bonus(),
    MALUS  = std.totems.malus(),
  },
}
