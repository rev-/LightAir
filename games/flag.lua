-- ================================================================
-- LightAir game: Flag — capture the flag, team O vs team X.
--
-- Port of the retired native C++ ruleset (deleted in the Lua
-- migration; the original is in git history).
--
-- Pick up the enemy flag by approaching its FLAG totem (RSSI gate),
-- carry it to an own-team BASE while your own flag is home to score.
-- If the carrier is shone, the flag returns home.  Flag state is
-- synchronised between players (and the flag totems) with
-- MSG.FLAG_EVENT broadcasts: TAKEN / DROPPED / SCORED.
-- Team with most aggregate captures wins; tie-break: fewest shone.
-- ================================================================

local std  = la.lib("std")
local proj = la.lib("projector")

-- The baseline profile reproduces what std.shiner did here: one
-- energy per beam, a full refill after the configured idle, both
-- read from this game's own config vars.
proj.define{ vars = { energy = "energy", spent = "energy_spent" } }

local S   = { IN_GAME = 0, OUT_GAME = 1, GAME_END = 2 }
local MSG = la.msg
local FE  = la.flag_event                -- TAKEN / DROPPED / SCORED
local R   = { TAKEN = 1, SHONE = 2, DOWN = 3, FRIEND = 4, IMMUNE = 5 }

local NEAR_BASE_RSSI = -57               -- ~2 m: base proximity (respawn + scoring)
local FLAG_RSSI      = -62               -- ~3-4 m: flag pickup zone
local PICKUP_RSSI    = -57               -- ~2 m: BONUS/MALUS claim gate

-- Continuous carry alert: slow cyan pulse + gentle vibration, tone
-- 500 Hz above the LIT feedback so the two never sound alike.
local carry_bg = {
  priority = 1,
  steps = {
    { ms = 300, freq = 4500, vib = 25, rgb = { 0, 180, 255 } },
    { ms = 200,                        rgb = { 0,  30,  80 } },
  },
}

-- ---- Private state ------------------------------------------------
local my_team         = 0
local enemy_team      = 1
local team_points     = 0        -- aggregate team captures known locally
local has_flag        = false    -- carrying the enemy flag right now
local enemy_carrier   = nil      -- nil = enemy flag at its totem
local my_flag_carrier = nil      -- nil = our flag at home
local respawn_at      = 0
local can_respawn     = false
local shone_by        = nil      -- short name of whoever put us down
local imm             = std.immunity(3000)

local function is_opponent(id) return la.team_of(id) ~= my_team end
local function friendly(pkt)   return pkt.team == my_team and vars.friendly_fire == 0 end

local function drop_flag()
  la.broadcast_relay(MSG.FLAG_EVENT, FE.DROPPED, enemy_team)
  has_flag      = false
  enemy_carrier = nil
  la.ui("FlagTaken")             -- "FLAG LOST" feedback
  la.background()                -- clear the carry alert
  la.clear_tray()
end

-- Shared handler for player MSG.FLAG_EVENT broadcasts (both states).
local function on_flag_event(vars, pkt)
  if pkt.len < 2 then return end
  local sub, flag_team = pkt:byte(1), pkt:byte(2)

  if sub == FE.TAKEN then
    if flag_team == enemy_team then
      enemy_carrier = pkt.sender           -- someone got there first
    else
      my_flag_carrier = pkt.sender         -- enemy has OUR flag
      la.ui("FlagTaken")
      la.show(la.player_short(pkt.sender) .. " HAS YOUR FLAG", 0)
    end

  elseif sub == FE.DROPPED then
    if flag_team == enemy_team then
      enemy_carrier = nil
    else
      my_flag_carrier = nil                -- our flag is back home
      la.ui("FlagReturn")
      la.clear_tray()
    end

  elseif sub == FE.SCORED then
    if flag_team == enemy_team then
      enemy_carrier = nil                  -- a teammate captured it
      team_points   = team_points + 1
    else
      my_flag_carrier = nil                -- enemy scored with our flag
      la.ui("FlagReturn")
      la.clear_tray()
    end
  end
end

local function game_over()
  if has_flag then
    has_flag = false
    la.background()
  end
  la.show("Game over!", 3000)
  la.ui("EndGame")
end

return {
  api     = 1,
  type_id = 0x0003,               -- GameTypeId::FLAG
  name    = "Flag",

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
    { id = "end_points",    name = "EndPoints",    min = 0,  max = 10,  step = 1,  default = 0   },
  },

  vars = {
    { id = "lives",        default = 3  },
    { id = "energy",       default = 50 },
    { id = "time_left",    default = 900, countdown_in = { S.IN_GAME, S.OUT_GAME } },
    { id = "flags",        default = 0  },      -- personal captures
    { id = "energy_spent", default = 0  },
    { id = "shone_times",  default = 0  },
  },

  monitor = {
    { var = "lives",        icon = "LIFE",   col = 0, row = 0, states = { S.IN_GAME } },
    { var = "energy",       icon = "ENERGY", col = 1, row = 0, states = { S.IN_GAME } },
    { var = "time_left",    icon = "TIME",   col = 0, row = 1, states = { S.IN_GAME, S.OUT_GAME } },
    { var = "flags",        icon = "FLAG",   col = 1, row = 1, states = { S.IN_GAME } },
    { var = "game_time",    icon = "TIME",   col = 0, row = 0, states = { S.GAME_END } },
    { var = "flags",        icon = "FLAG",   col = 1, row = 0, states = { S.GAME_END } },
    { var = "energy_spent", icon = "ENERGY", col = 0, row = 1, states = { S.GAME_END } },
    { var = "shone_times",  icon = "LIFE",   col = 1, row = 1, states = { S.GAME_END } },
  },

  winners = {
    { var = "flags",       dir = "max" },
    { var = "shone_times", dir = "min" },
  },

  on_score_announce = std.team_announce{ primary = "sum" },

  totem_slots = {
    { role = "FLAG_O", min = 1, max = 1 },     -- exactly one flag per team
    { role = "FLAG_X", min = 1, max = 1 },
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
    vars.flags        = 0
    vars.energy_spent = 0
    vars.shone_times  = 0
    my_team         = la.my_team()
    enemy_team      = my_team ~ 1
    team_points     = 0
    has_flag        = false
    enemy_carrier   = nil
    my_flag_carrier = nil
    respawn_at      = 0
    can_respawn     = false
    shone_by        = nil
    imm.reset()
    proj.reset(vars)
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

      [MSG.FLAG_EVENT] = on_flag_event,

      -- Flag pickup: FLAG totem beacon, state IN, enemy's flag, close by.
      [MSG.FLAG_BEACON] = function(vars, pkt)
        if has_flag                     then return end
        if pkt.len < 2                  then return end
        if pkt:byte(1) ~= 0             then return end   -- 0 = FLAG_IN
        if pkt:byte(2) ~= enemy_team    then return end
        if pkt.rssi < FLAG_RSSI         then return end
        if enemy_carrier ~= nil         then return end
        has_flag      = true
        enemy_carrier = la.my_id()
        la.broadcast_relay(MSG.FLAG_EVENT, FE.TAKEN, enemy_team)
        la.ui("FlagGain")
        la.background(carry_bg)
        la.show("YOU HAVE FLAG", 0)
      end,

      -- Flag scoring: own-team BASE beacon while carrying — but only
      -- while our own flag is home (returning it while ours is away
      -- must not score).
      [MSG.BASE_BEACON] = function(vars, pkt)
        if not has_flag                 then return end
        if pkt.len < 1                  then return end
        if pkt:byte(1) ~= my_team       then return end
        if pkt.rssi < NEAR_BASE_RSSI    then return end
        if my_flag_carrier ~= nil       then return end
        vars.flags    = vars.flags + 1
        team_points   = team_points + 1
        has_flag      = false
        enemy_carrier = nil
        la.broadcast_relay(MSG.FLAG_EVENT, FE.SCORED, enemy_team)
        la.ui("FlagGain")
        la.background()
        la.clear_tray()
        la.show("FLAG SCORED!", 2000)
      end,
    },

    [S.OUT_GAME] = {
      [MSG.LIT]        = function() return R.DOWN end,
      [MSG.FLAG_EVENT] = on_flag_event,
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
      -- No points for shining in Flag, but the tray still names who went
      -- down: that is the only way a carrier's escort knows it worked.
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
      when   = function(vars) return vars.end_points > 0 and team_points >= vars.end_points end,
      action = game_over },
    { from = S.IN_GAME, to = S.OUT_GAME,
      when   = function(vars) return vars.lives <= 0 end,
      action = function(vars)
        if has_flag then drop_flag() end   -- carrier shone: flag returns home
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
      when   = function(vars) return vars.end_points > 0 and team_points >= vars.end_points end,
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
    FLAG_O = std.totems.flag(0),
    FLAG_X = std.totems.flag(1),
    BASE_O = std.totems.base(0),
    BASE_X = std.totems.base(1),
    BASE   = std.totems.base("any"),
    BONUS  = std.totems.bonus(),
    MALUS  = std.totems.malus(),
  },
}
