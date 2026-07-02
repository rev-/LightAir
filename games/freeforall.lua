-- ================================================================
-- LightAir game: Free For All
--
-- Reference port of src/rulesets/GameFreeForAll.cpp to the Lua
-- game format described in docs/lua-games-design.md.
--
-- Every player shines every other player.  Shone players respawn
-- automatically after respawn_secs.  Most points wins; tie-break
-- is fewest times shone.
--
-- This file is pure game logic.  Everything hardware-bound
-- (Enlight optics, radio transport, LCD, buzzer, keypad, the 10 ms
-- loop itself, score collection, winner election, the pre-game
-- menu and warmup countdown) stays in the C++ firmware and is
-- reached through the `la.*` verbs.
-- ================================================================

-- ---- States ----------------------------------------------------
-- Plain integers; bit N of a monitor var's state set = "shown in
-- state N", exactly like the C++ MonitorVar::stateMask.
local S = { IN_GAME = 0, OUT_GAME = 1, GAME_END = 2 }

-- ---- Radio vocabulary -------------------------------------------
-- la.msg holds the firmware's RadioMsg registry (MSG_LIT etc.) so
-- byte values never drift between Lua and C++.
local MSG = la.msg          -- MSG.LIT, MSG.SCORE_COLLECT, MSG.BONUS_BEACON, ...

-- Reply sub-types (payload[0] of the 0x11 reply) — game-private.
local R = { TAKEN = 1, SHONE = 2, DOWN = 3, IMMUNE = 4 }

local IMMUNITY_MS = 3000

-- ---- Private game state -----------------------------------------
-- Anything that is NOT shown on the LCD, NOT edited in the menu and
-- NOT part of winner election can live as plain Lua locals.
local respawn_at         = 0      -- la.now() when respawn fires
local lit_at             = {}     -- [senderId] = la.now() of last accepted lit
local trigger_was_active = false
local release_at         = 0

return {
  api     = 1,                    -- binding version this file targets
  type_id = 0x0001,               -- GameTypeId::FREE_FOR_ALL
  name    = "Free for All",       -- <=15 chars, shown in game list

  initial_state = S.IN_GAME,
  scoring_state = S.GAME_END,     -- C++ score collection kicks in here
  score_msg     = MSG.SCORE_COLLECT,

  -- ---- Config vars (pre-game startup menu, edited on the host) ----
  -- The C++ setup menu renders these, lets the DM adjust them, and
  -- broadcasts the values in the existing config blob.  After that
  -- they are readable (and writable) as vars.<id>.
  config = {
    { id = "start_lives",   name = "Lives",    min = 1,  max = 5,   step = 1,  default = 3   },
    { id = "respawn_secs",  name = "Respawn",  min = 5,  max = 120, step = 5,  default = 30  },
    { id = "start_energy",  name = "Energy",   min = 10, max = 100, step = 10, default = 50  },
    { id = "recharge_secs", name = "Recharge", min = 0,  max = 20,  step = 5,  default = 10  },
    { id = "game_time",     name = "Time",     min = 60, max = 900, step = 60, default = 900 },
  },

  -- ---- Game vars (the C++/Lua shared blackboard) -------------------
  -- Each entry becomes one int slot owned by the firmware.  The LCD
  -- binds directly to the slot (no per-tick marshalling); Lua reads
  -- and writes it through the `vars` proxy.
  -- countdown_in: firmware decrements the slot once per second
  -- (drift-free) while the game is in one of the listed states —
  -- replaces the hand-rolled tickGameTime() of the C++ rulesets.
  vars = {
    { id = "lives",        default = 3  },
    { id = "energy",       default = 50 },
    { id = "time_left",    default = 900, countdown_in = { S.IN_GAME, S.OUT_GAME } },
    { id = "points",       default = 0  },
    { id = "energy_spent", default = 0  },
    { id = "shone_times",  default = 0  },
  },

  -- ---- LCD layout per state (monitorVars) --------------------------
  monitor = {
    -- in-game screen
    { var = "lives",       icon = "LIFE",   col = 0, row = 0, states = { S.IN_GAME } },
    { var = "energy",      icon = "ENERGY", col = 1, row = 0, states = { S.IN_GAME } },
    { var = "time_left",   icon = "TIME",   col = 0, row = 1, states = { S.IN_GAME, S.OUT_GAME } },
    { var = "points",      icon = "SCORE",  col = 1, row = 1, states = { S.IN_GAME } },
    -- end-game screen (config vars can be monitored too)
    { var = "game_time",    icon = "TIME",   col = 0, row = 0, states = { S.GAME_END } },
    { var = "points",       icon = "SCORE",  col = 1, row = 0, states = { S.GAME_END } },
    { var = "energy_spent", icon = "ENERGY", col = 0, row = 1, states = { S.GAME_END } },
    { var = "shone_times",  icon = "LIFE",   col = 1, row = 1, states = { S.GAME_END } },
  },

  -- ---- Winner election (C++ collects and ranks) --------------------
  winners = {
    { var = "points",      dir = "max" },   -- primary: most points
    { var = "shone_times", dir = "min" },   -- tie-break: fewest shone
  },

  -- ---- Totem requirements (assigned by the host in the menu) -------
  totem_slots = {
    { role = "BONUS", min = 0, max = 16, config_var = "bonus_cooldown" },
    { role = "MALUS", min = 0, max = 16 },
  },
  teams = 0,                       -- teamless game

  -- Announced to totems in the activation reply so they can arm
  -- their self-revert watchdog.
  time_left_var = "time_left",

  -- ---- Lifecycle ----------------------------------------------------
  -- Called by the runner after the (C++-owned) warmup countdown, once
  -- config values have been distributed and applied.
  on_begin = function(vars)
    vars.lives     = vars.start_lives
    vars.energy    = vars.start_energy
    vars.time_left = vars.game_time
    vars.points        = 0
    vars.energy_spent  = 0
    vars.shone_times   = 0
    respawn_at         = 0
    lit_at             = {}
    trigger_was_active = false
    release_at         = 0
    la.ui("GameStart")
  end,

  -- ---- Incoming requests, per state (DirectRadioRules) --------------
  -- Handler returns the reply sub-type (payload[0] of the auto-reply);
  -- nil/0 = plain empty reply.  Unhandled messages still get the
  -- standard empty reply from the runner, exactly as today.
  on_message = {
    [S.IN_GAME] = {
      [MSG.LIT] = function(vars, pkt)
        local t = lit_at[pkt.sender]
        if t and la.now() - t < IMMUNITY_MS then
          return R.IMMUNE
        end
        vars.lives = vars.lives - 1
        lit_at[pkt.sender] = la.now()
        if vars.lives > 0 then
          la.ui("GotLit")
          return R.TAKEN
        end
        return R.SHONE          -- state rule below moves us to OUT_GAME
      end,
    },
    [S.OUT_GAME] = {
      [MSG.LIT] = function() return R.DOWN end,
    },
  },

  -- ---- Replies to our own requests (ReplyRadioRules) -----------------
  -- Keyed by the original request msgType, then by reply sub-type.
  -- Active in every state except scoring_state unless states= is given.
  on_reply = {
    [MSG.LIT] = {
      [R.TAKEN]  = function(vars, reply, orig) la.ui("Taken")  end,
      [R.IMMUNE] = function(vars, reply, orig) la.ui("Immune") end,
      [R.SHONE]  = function(vars, reply, orig)
        vars.points = vars.points + 1
        la.ui("Lit")
      end,
    },
  },

  -- ---- State transitions, first match wins (StateRules) ---------------
  rules = {
    { from = S.IN_GAME, to = S.GAME_END,
      when   = function(vars) return vars.time_left <= 0 end,
      action = function(vars)
        la.show("Game over!", 3000)
        la.ui("EndGame")
      end },

    { from = S.IN_GAME, to = S.OUT_GAME,
      when   = function(vars) return vars.lives <= 0 end,
      action = function(vars)
        vars.shone_times = vars.shone_times + 1
        respawn_at = la.now() + vars.respawn_secs * 1000
        la.show("Shone!", 2000)
        la.ui("Down")
      end },

    { from = S.OUT_GAME, to = S.GAME_END,
      when   = function(vars) return vars.time_left <= 0 end,
      action = function(vars)
        la.show("Game over!", 3000)
        la.ui("EndGame")
      end },

    { from = S.OUT_GAME, to = S.IN_GAME,
      when   = function(vars) return la.now() >= respawn_at end,
      action = function(vars)
        vars.lives  = vars.start_lives
        vars.energy = vars.start_energy
        lit_at = {}
        la.show("Back in game!", 1000)
        la.ui("Up")
      end },
  },

  -- ---- Per-state tick body, 100 Hz (StateBehaviors) --------------------
  -- OUT_GAME / GAME_END need no body: the countdown is declarative
  -- (countdown_in) and the end screen is static.
  update = {
    [S.IN_GAME] = function(vars)
      -- A confirmed lit target → notify it over radio.
      -- Points are only awarded when the target replies R.SHONE.
      local target = la.shine_lit()          -- player id or nil
      if target then la.send(target, MSG.LIT) end

      -- Fire while the trigger is down and energy remains.
      local active = la.trigger_down(1)
      if active and vars.energy > 0 and la.shine() then
        vars.energy       = vars.energy - 1
        vars.energy_spent = vars.energy_spent + 1
        la.ui_enlight(la.shine_ms())
      end

      -- Release edge starts the recharge cooldown.
      if trigger_was_active and not active then
        release_at = la.now()
      end
      trigger_was_active = active

      -- Restore full energy once the cooldown has elapsed.
      if not active and vars.energy < vars.start_energy
         and la.now() - release_at >= vars.recharge_secs * 1000 then
        vars.energy = vars.start_energy
      end
    end,
  },

  -- ---- Totem behaviour (TotemVM programs, pure data) --------------------
  -- Totems hold no game files.  Each entry below is a declarative
  -- state machine that the projector serializes into the single 0xF1
  -- activation packet; the interpreter is fixed totem firmware.
  -- (games/lib/std.lua has factories that build these same tables —
  -- std.totems.bonus() etc.; they are written out here in full as the
  -- tutorial.)  Model reference: docs/totem-behavior-handshake.md.
  --
  -- BONUS: state 1 = READY (idle sparkle, beacon every 2 s; any reply
  -- claims it), state 2 = COOLDOWN (silent for the configured seconds,
  -- then back to READY, whose `enter` rule restores the idle look).
  totems = {
    BONUS = { vm = 1, cfg_default = 30, states = {
      { -- state 1: READY
        { enter = true,
          run = { {"anim", "BonusIdle", {"rgb", 0, 180, 0}} } },
        { every = 2000,
          run = { {"bcast", MSG.BONUS_BEACON, 0} } },  -- payload byte 0 = ready
        { reply = MSG.BONUS_BEACON,
          run = { {"start", 0}, {"anim", "Bonus"}, {"goto", 2} } },
      },
      { -- state 2: COOLDOWN
        { every = 250,
          when = { {"elapsed", 0, ">=", {"cfg"}} },  -- {"cfg"} = this role's
          run = { {"goto", 1} } },                   -- config seconds
      },
    } },

    MALUS = { vm = 1, cfg_default = 30, states = {
      { -- state 1: READY
        { enter = true,
          run = { {"anim", "MalusIdle", {"rgb", 200, 0, 0}} } },
        { every = 2000,
          run = { {"bcast", MSG.MALUS_BEACON, 0} } },
        { reply = MSG.MALUS_BEACON,
          run = { {"start", 0}, {"anim", "Malus"}, {"goto", 2} } },
      },
      { -- state 2: COOLDOWN
        { every = 250,
          when = { {"elapsed", 0, ">=", {"cfg"}} },
          run = { {"goto", 1} } },
      },
    } },
  },
}
