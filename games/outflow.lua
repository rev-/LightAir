-- ================================================================
-- LightAir game: Outflow — energy-only FFA; no lives.
--
-- Port of the retired native C++ ruleset (deleted in the Lua
-- migration; the original is in git history).
--
-- Energy is simultaneously ammo and life total: shining costs 1,
-- being lit costs lit_cost, and a passive drain eats 1 energy every
-- (10 s / drain_rate).  Reaching 0 puts you out until the timed
-- respawn.  Eliminating another player refills you by start_energy
-- (uncapped) and grants a point; draining yourself to 0 costs one.
-- Most points wins; tie-break: fewest times shone.
--
-- Note: the C++ file's config menu edited the *live* energy value
-- and onBegin then overwrote it with a fixed 100; this port wires
-- the config entry to start_energy, which is what the menu label
-- always meant.
-- ================================================================

local std = la.lib("std")

local S   = { IN_GAME = 0, OUT_GAME = 1, GAME_END = 2 }
local MSG = la.msg
local R   = { TAKEN = 1, SHONE = 2, DOWN = 3 }

local PICKUP_RSSI = -57         -- ~2 m: BONUS/MALUS claim gate

-- ---- Private state ------------------------------------------------
local pending_shone    = false   -- fatal lit received this cycle
local shone_by         = nil     -- short name of whoever put us down
local pending_depleted = false   -- drain zeroed energy this cycle
local respawn_at       = 0
local last_drain       = 0
local drain_interval   = 1000

local function game_over()
  la.show("Game over!", 3000)
  la.ui("EndGame")
end

return {
  api     = 1,
  type_id = 0x0004,               -- GameTypeId::OUTFLOW
  name    = "Outflow",

  initial_state = S.IN_GAME,
  scoring_state = S.GAME_END,
  score_msg     = MSG.SCORE_COLLECT,

  config = {
    { id = "start_energy", name = "Energy",    min = 50, max = 200, step = 25, default = 100 },
    { id = "lit_cost",     name = "LitCost",   min = 25, max = 200, step = 25, default = 50  },
    { id = "drain_rate",   name = "DrainRate", min = 2,  max = 20,  step = 2,  default = 10  },
    { id = "respawn_secs", name = "Respawn",   min = 5,  max = 120, step = 5,  default = 30  },
    { id = "game_time",    name = "Time",      min = 60, max = 900, step = 60, default = 900 },
  },

  vars = {
    { id = "energy",       default = 100 },
    { id = "time_left",    default = 900, countdown_in = { S.IN_GAME, S.OUT_GAME } },
    { id = "points",       default = 100 },   -- start at 100; self-depletion costs 1
    { id = "shone_times",  default = 0   },
    { id = "depletions",   default = 0   },
    { id = "energy_spent", default = 0   },
  },

  monitor = {
    { var = "energy",       icon = "ENERGY", col = 0, row = 0, states = { S.IN_GAME } },
    { var = "points",       icon = "SCORE",  col = 1, row = 0, states = { S.IN_GAME } },
    { var = "time_left",    icon = "TIME",   col = 0, row = 1, states = { S.IN_GAME, S.OUT_GAME } },
    { var = "shone_times",  icon = "LIFE",   col = 1, row = 1, states = { S.IN_GAME } },
    { var = "game_time",    icon = "TIME",   col = 0, row = 0, states = { S.GAME_END } },
    { var = "points",       icon = "SCORE",  col = 1, row = 0, states = { S.GAME_END } },
    { var = "energy_spent", icon = "ENERGY", col = 0, row = 1, states = { S.GAME_END } },
    { var = "depletions",   icon = "DOWN",   col = 1, row = 1, states = { S.GAME_END } },
  },

  winners = {
    { var = "points",      dir = "max" },
    { var = "shone_times", dir = "min" },
  },

  totem_slots = {
    { role = "BONUS", min = 0, max = 16 },
    { role = "MALUS", min = 0, max = 16 },
  },
  teams = 0,
  time_left_var = "time_left",

  on_begin = function(vars)
    vars.energy    = vars.start_energy
    vars.time_left = vars.game_time
    vars.points       = 100
    vars.shone_times  = 0
    vars.depletions   = 0
    vars.energy_spent = 0
    pending_shone    = false
    shone_by         = nil
    pending_depleted = false
    respawn_at       = 0
    last_drain       = la.now()
    drain_interval   = (vars.drain_rate > 0) and (10000 // vars.drain_rate) or 1000
    la.shine_config{ cooldown_ms = 20, reps = 20 }
    la.ui("GameStart")
  end,

  on_message = {
    [S.IN_GAME] = {
      -- A pickup totem gives itself to whoever answers, so only answer
      -- from arm's length: the claim has to mean "I am standing at it".
      [MSG.BONUS_BEACON] = std.pickup_claim{ rssi = PICKUP_RSSI },
      [MSG.MALUS_BEACON] = std.pickup_claim{ rssi = PICKUP_RSSI },
      [MSG.LIT] = function(vars, pkt)
        if vars.energy > vars.lit_cost then
          vars.energy = vars.energy - vars.lit_cost
          la.show("Lit by " .. la.player_short(pkt.sender), 2000)
          la.ui("GotLit")
          return R.TAKEN
        end
        vars.energy   = 0
        pending_shone = true
        -- The packet is the only place the shiner's id is in hand; the
        -- transition below puts it on the tray for the whole wait.
        shone_by      = la.player_short(pkt.sender)
        return R.SHONE
      end,
    },
    [S.OUT_GAME] = {
      [MSG.LIT] = function() return R.DOWN end,
    },
  },

  on_reply = {
    [MSG.LIT] = {
      [R.TAKEN] = function() la.ui("Taken") end,
      [R.SHONE] = function(vars, reply)
        vars.energy = vars.energy + vars.start_energy   -- uncapped refill
        vars.points = vars.points + 1
        la.show(la.player_short(reply.sender) .. " SHONE!", 3000)
        la.ui("Lit")
      end,
      [R.DOWN] = function(vars, reply)
        la.show(la.player_short(reply.sender) .. " is OUT", 2000)
        la.ui("Lit")
      end,
    },
  },

  rules = {
    { from = S.IN_GAME, to = S.GAME_END,
      when   = function(vars) return vars.time_left <= 0 end,
      action = game_over },
    { from = S.IN_GAME, to = S.OUT_GAME,
      when   = function() return pending_shone end,
      action = function(vars)
        vars.shone_times = vars.shone_times + 1
        pending_shone = false
        respawn_at    = la.now() + vars.respawn_secs * 1000
        -- Two persistent lines for the whole wait, credit on top: who put
        -- us down, and what to do about it.  The "Down" cue is the moment
        -- feedback, so no transient line competes for the tray.  Here the way back is the clock,
        -- not a base, so the instruction says so.
        la.show("Wait to respawn", 0)
        la.show("LIT by " .. (shone_by or "?"), 0)
        la.ui("Down")
      end },
    { from = S.IN_GAME, to = S.OUT_GAME,
      when   = function() return pending_depleted end,
      action = function(vars)
        vars.depletions = vars.depletions + 1
        vars.points     = vars.points - 1
        pending_depleted = false
        respawn_at       = la.now() + vars.respawn_secs * 1000
        -- Nobody to credit: the drain did it.
        la.show("Wait to respawn", 0)
        la.show("Drained out!", 0)
        la.ui("Down")
      end },
    { from = S.OUT_GAME, to = S.GAME_END,
      when   = function(vars) return vars.time_left <= 0 end,
      action = game_over },
    { from = S.OUT_GAME, to = S.IN_GAME,
      when   = function() return la.now() >= respawn_at end,
      action = function(vars)
        vars.energy = vars.start_energy
        last_drain  = la.now()
        shone_by    = nil
        la.clear_tray()             -- drop the credit and the instruction
        la.show("Back in game!", 1000)
        la.ui("Up")
      end },
  },

  update = {
    [S.IN_GAME] = function(vars)
      -- Passive drain: 1 energy every drain_interval ms, drift-free.
      if la.now() - last_drain >= drain_interval then
        last_drain = last_drain + drain_interval
        if vars.energy > 0 then
          vars.energy = vars.energy - 1
        end
      end

      -- A confirmed lit target → notify it over radio.
      local target = la.shine_lit()
      if target then la.send(target, MSG.LIT) end

      -- Shine while the trigger is down.  No recharge in Outflow:
      -- energy only returns by eliminating someone or respawning.
      if la.trigger_down(1) and vars.energy > 0 and la.shine() then
        vars.energy       = vars.energy - 1
        vars.energy_spent = vars.energy_spent + 1
        la.ui_enlight(la.shine_ms())
      end

      -- Depletion from any cause — unless a fatal lit already claimed
      -- this cycle (the two exits stay mutually exclusive).
      if vars.energy == 0 and not pending_shone then
        pending_depleted = true
      end
    end,
  },

  totems = {
    BONUS = std.totems.bonus(),
    MALUS = std.totems.malus(),
  },
}
