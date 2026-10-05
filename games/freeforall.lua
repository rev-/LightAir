-- ================================================================
-- LightAir game: Free For All
--
-- The reference game file: the simplest complete ruleset, with every
-- section of the Lua game format (docs/lua-games-design.md) commented.
-- It is built like every other game, on the standard library
-- (games/lib/std.lua) and the projector (games/lib/projector.lua), so
-- a fix to a shared mechanic — hit strength, area hits, pickups —
-- reaches it like the rest.  Copy this file to start a new game.
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
-- TAKEN and SHONE are the firmware's (la.hit): its area service reads them.
local R = { TAKEN = la.hit.TAKEN, SHONE = la.hit.SHONE, DOWN = 3, IMMUNE = 4 }

-- The standard library: the recurring game patterns (the hit ladder,
-- immunity, pickups, the respawn bar, the totem programs), built only
-- out of la.* verbs.  la.lib loads a library once and caches it.
local std = la.lib("std")

-- Calibrated from measured RSSI-vs-distance (RSSI(d) = -46 - 20*log10(d),
-- d in metres — fits -60 dBm @ 5 m and -70 dBm @ 16 m).
local PICKUP_RSSI = -55     -- ~2 m: BONUS/MALUS claim gate

-- The projector is the only route to Enlight, for every ruleset: it owns
-- the trigger, the energy a beam costs, the recharge, the reach and what a
-- hit weighs on the wire.  Declaring nothing but the baseline gives one
-- energy per beam and a full refill after the configured idle, both read
-- from this game's own config (start_energy, recharge_secs).
local proj = la.lib("projector")
proj.define{ vars = { energy = "energy", spent = "energy_spent",
                      reload = "reload", reload_ms = "reload_ms",
                      icon = "energy_icon" } }

-- ---- Private game state -----------------------------------------
-- Anything that is NOT shown on the LCD, NOT edited in the menu and
-- NOT part of winner election can live as plain Lua locals.
local respawn_at         = 0      -- la.now() when respawn fires
local shone_by           = nil    -- who put us down: a player's short name, or "TOTEM"
-- After a player's beam lands, the next one from the same player is
-- refused for 3 s: one burst cannot empty a pool of lives.
local imm                = std.immunity(3000)

-- This player's starting lives: what on_begin loads, what a respawn
-- restores, and the S of a BONUS LIFE.  One resolver for all three, so
-- when players get roles with their own starting lives only this changes.
local function my_start_lives(vars) return vars.start_lives end

-- What a claimed pickup does.  The DM picks each BONUS / MALUS totem's
-- effect in the Totems submenu (O key), from the `options` lists in
-- totem_slots below, and std.pickup_effect applies the one the claimed
-- totem carries: BONUS LIFE (+S lives, capped at 2*S), a powered
-- projector, MALUS LIFE (out of lives) or MALUS DIM.  A MALUS LIFE has
-- no player to credit, so the tray says the totem did it.
local pickup = std.pickup_effect{ proj = proj, lives = "lives",
                                  start_lives = my_start_lives,
                                  on_malus_life = function() shone_by = "TOTEM" end }

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
    { id = "start_energy",  name = "Energy",   min = 10, max = 60,  step = 5,  default = 30  },
    { id = "recharge_secs", name = "Recharge", min = 5,  max = 20,  step = 5,  default = 10  },
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
    { id = "energy",       default = 30 },
    { id = "time_left",    default = 900, countdown_in = { S.IN_GAME, S.OUT_GAME } },
    -- The respawn wait, for the OUT_GAME loading bar: written when
    -- the wait starts.
    { id = "respawn_zero", default = 0 },
    { id = "respawn_from", default = 0 },
    { id = "respawn_ms",   default = 0 },
    { id = "points",       default = 0  },
    { id = "energy_spent", default = 0  },
    -- The projector's reload clock, read by the energy cell's bar:
    -- reload = millis the wait began (0 = not waiting), reload_ms =
    -- how long it takes.  Both written by projector.lua.
    { id = "reload",       default = 0 },
    { id = "reload_ms",    default = 0 },
    -- The icon of the projector in hand (an la.icons value), written by
    -- projector.lua and read by the energy cell: FAST, LONG, … replace
    -- the standard energy glyph while they are the one in use.
    { id = "energy_icon",  default = la.icons.ENERGY },
    { id = "shone_times",  default = 0  },
  },

  -- ---- LCD layout per state (monitorVars) --------------------------
  monitor = {
    -- in-game screen
    { var = "lives",       icon = "LIFE",   col = 0, row = 0, states = { S.IN_GAME } },
    -- Energy, and — while the pool is empty — a bar filling over the
    -- recharge.  The projector owns both the duration and the instant
    -- the wait began, because a refill starts at the trigger's RELEASE,
    -- not when the pool hit zero.
    { var = "energy",      icon = "ENERGY", col = 1, row = 0, states = { S.IN_GAME },
      bar = true, bar_at = 0, fill_var = "reload_ms", start_var = "reload",
      icon_var = "energy_icon" },
    { var = "time_left",   icon = "TIME",   col = 0, row = 1, states = { S.IN_GAME, S.OUT_GAME } },
    -- Out: a bar filling over the respawn time, from the instant the
    -- wait began.
    { var = "respawn_zero", icon = "DOWN",   col = 1, row = 0, states = { S.OUT_GAME },
      bar = true, bar_at = 0, fill_var = "respawn_ms", start_var = "respawn_from" },
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
    -- options: what the DM can pick per totem with O (see `pickup` above).
    { role = "BONUS", min = 0, max = 16, options = proj.bonus_options() },
    { role = "MALUS", min = 0, max = 16, options = std.malus_options() },
  },
  teams = 0,                       -- teamless game

  -- Announced to totems in the activation reply so they can arm
  -- their self-revert watchdog.
  time_left_var = "time_left",

  -- ---- Lifecycle ----------------------------------------------------
  -- Called by the runner after the (C++-owned) warmup countdown, once
  -- config values have been distributed and applied.
  on_begin = function(vars)
    vars.lives     = my_start_lives(vars)
    vars.time_left = vars.game_time
    vars.points        = 0
    vars.energy_spent  = 0
    vars.shone_times   = 0
    respawn_at         = 0
    shone_by           = nil
    imm.reset()
    proj.reset(vars)                -- fills the pool and pushes the optics
    la.ui("GameStart")
  end,

  -- ---- Incoming requests, per state (DirectRadioRules) --------------
  -- The handler's return value IS the reply: an integer answers with that
  -- sub-type, returning nothing answers nothing at all.  Beacons a game
  -- ignores stay unanswered, so a totem that waits for a deliberate answer
  -- (BASE, BONUS, MALUS) hears only the players that acted on it.
  on_message = {
    [S.IN_GAME] = {
      -- A pickup totem gives itself to whoever answers, so only answer
      -- from arm's length: the claim has to mean "I am standing at it".
      -- The answer (this player's id) is what the totem animates and
      -- starts its cooldown on.
      [MSG.BONUS_BEACON] = std.pickup_claim{ rssi = PICKUP_RSSI, on_claim = pickup },
      [MSG.MALUS_BEACON] = std.pickup_claim{ rssi = PICKUP_RSSI, on_claim = pickup },
      -- A LIT costs its strength in lives (STRONG weighs 3), unless the
      -- shooter's last beam landed inside the immunity window.  An area hit
      -- (pkt.area: someone else's SPLASH landing nearby) costs the band's
      -- strength and ignores the window.  The last life answers SHONE; the
      -- state rule below moves us out.
      [MSG.LIT] = std.lit_target{
        lives = "lives", immunity = imm,
        reply = { taken = R.TAKEN, shone = R.SHONE, immune = R.IMMUNE },
        -- This packet is the only place the shiner's id is in hand — the
        -- state rule that follows sees no packet — so the name for the
        -- tray is taken here.
        on_shone = function(_, pkt) shone_by = la.player_short(pkt.sender) end,
      },
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
        -- The beam is invisible and the buzzer cannot name anybody, so
        -- the one place a shiner learns WHO they put down is the tray.
        -- `reply.sender` is the target: the reply came from them.
        la.show(la.player_short(reply.sender) .. " SHONE!", 3000)
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
        -- Starts the wait and the OUT_GAME bar that fills over it.
        respawn_at = std.respawn_wait(vars, vars.respawn_secs)
        -- Two persistent lines for the whole wait, credit on top: who put
        -- us down, and what to do about it.  The "Down" cue is the moment
        -- feedback, so no transient line competes for the tray.  Here the way back is the clock,
        -- not a base, so the instruction says so.
        la.show("Wait to respawn", 0)
        la.show("LIT by " .. (shone_by or "?"), 0)
        proj.strip(vars)            -- going out loses powered projectors and DIM
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
        vars.lives  = my_start_lives(vars)
        vars.energy = vars.start_energy
        imm.reset()
        shone_by = nil
        la.clear_tray()             -- drop the credit and the instruction
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
      local target = proj.result(vars)       -- player id or nil
      if target then la.send(target, MSG.LIT, proj.payload(vars)) end

      -- Trigger, energy, recharge: the projector owns all of it, and its
      -- baseline profile is the one this ruleset used to spell out here.
      proj.tick(vars)
    end,
  },

  -- ---- Totem behaviour (TotemVM programs, pure data) --------------------
  -- Totems hold no game files.  Each entry is a declarative state machine
  -- that the projector serializes into the single 0xF1 activation packet;
  -- the interpreter is fixed totem firmware.  std.totems builds the
  -- standard roles — std.lua's pickup() is the BONUS / MALUS program
  -- written out, and docs/totem-behavior-handshake.md is the reference:
  -- READY (idle animation, a beacon every 2 s, any reply claims it), then
  -- COOLDOWN for the DM's configured seconds.
  totems = {
    BONUS = std.totems.bonus(),
    MALUS = std.totems.malus(),
  },
}
