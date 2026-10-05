-- ================================================================
-- LightAir game: Virus — infection tag.  Last clean player wins.
--
-- One player, drawn at random among those who joined (the DM included),
-- starts as the VIRUS: the DM's device draws when it starts the match and
-- sends the ID with the start signal (virus_id, draw = "player").  The
-- role is stated on the LCD ("VIRUS" / "CLEAN"),
-- and a red pulsing background alert marks the infected device.
--
-- The virus pays for its power:
--   * a cooldown between one shine and the next (virus_cooldown ms),
--   * an energy pool of only a FIFTH of the clean players' maximum.
--
-- Whoever is lit by a virus becomes a virus too (the lit carries a
-- "viral" payload byte).  New infections are announced with a flooded
-- broadcast so every device tracks how many clean players remain.
--
-- A clean player's shine does nothing to another clean player (the
-- shooter hears the friendly-fire cue), but it puts a virus DOWN: out of
-- play for virus_respawn_secs, then back at any totem — the virus walks up
-- to one and its device touches it (std.totem_touch): the totem plays its
-- arrival chaser and answers, and the answer is the respawn.  With no
-- totems in the match, the virus is back when the time is up.  A clean
-- SPLASH puts down the viruses standing near the one it hits.
--
-- A virus is back with 5 s of grace: no beam or area puts it down again,
-- and it cannot shine either.  A virus may also put another virus down.
--
-- Points: a virus infecting a clean player +5, a virus putting a virus down
-- +1, a clean player putting a virus down +2 (a SPLASH's area downs too),
-- and +10 to every player still clean when the game ends — on time, on
-- the last clean player, or ended by the DM.
--
-- The game ends when at most one clean player is left, or when the
-- time runs out.  Winner: most points; tie-break: whoever stayed clean
-- longest (clean_secs).
-- ================================================================

local std  = la.lib("std")
local proj = la.lib("projector")

local S   = { CLEAN = 0, VIRUS = 1, GAME_END = 2, DOWN = 3 }
local MSG = la.msg

-- Game-private message: infection announcement (flooded broadcast,
-- no payload; the sender IS the newly infected player).
-- Custom even msgType from the 0x10 player block; safe to pick here
-- because typeId + sessionToken isolate games on the wire.  Never
-- reuse the 0xA0 infrastructure or 0xF0 totem-protocol blocks.
local MSG_INFECTED = 0x16

-- Reply sub-types for MSG.LIT.  The firmware reads two of them (la.hit):
-- TAKEN means the hit landed and SHONE that it put the player out of play —
-- its area service triggers and credits on those.  SHONE is both of this
-- game's knock-outs: a viral beam infecting a clean player, and a clean
-- beam putting a virus down (so a clean SPLASH bursts on a virus and its
-- area knock-outs are credited back).  "Already a virus", "friend" and
-- "already down" stay off both.
--
-- A virus put down by a viral beam answers VDOWN instead: its shooter
-- scores it differently (+1), and no viral projector carries an area, so
-- the area service has nothing to do with it.
local R = { SHONE = la.hit.SHONE, VDOWN = 3, FRIEND = 4, DOWN = 5, SAFE = 6 }

-- Points, by what the shooter did (see the header).
local PTS = { INFECT = 5, CLEAN_DOWNS_VIRUS = 2, VIRUS_DOWNS_VIRUS = 1, STILL_CLEAN = 10 }

-- A virus back from DOWN can neither be put down nor shine for this long.
local GRACE_MS = 5000

-- Calibrated from measured RSSI-vs-distance (RSSI(d) = -46 - 20*log10(d),
-- d in metres — fits -60 dBm @ 5 m and -70 dBm @ 16 m).
local PICKUP_RSSI = -55         -- ~2 m: BONUS/MALUS claim gate
local TOTEM_RSSI  = -55         -- ~2 m: a down virus is "at" a totem

-- Continuous red pulse + soft vibration on the infected device.
local virus_bg = {
  priority = 1,
  steps = {
    { ms = 250, freq = 3500, vib = 20, rgb = { 255, 0, 0 } },
    { ms = 400,                        rgb = {  40, 0, 0 } },
  },
}

-- ---- Private state ------------------------------------------------
local virus_set        = {}      -- [playerId] = true once infected
local virus_count      = 0
local pending_infected = false   -- a viral lit reached us this cycle
local pending_down     = false   -- a clean lit reached us, a virus
local downed_by        = nil     -- who put us down, for the tray
local respawn_at       = 0       -- la.now() before which a down virus waits
local safe_until       = 0       -- la.now() before which a respawned virus is safe
local has_totems       = false   -- any totem in this match to respawn at
local can_respawn      = false
-- Touching is only asked for once the wait is over; a late answer to a
-- touch sent while still waiting, or after respawning, must not count.
local touching         = false
local touch            = std.totem_touch{ rssi = TOTEM_RSSI, every = 1000 }
-- The two roles are two projectors.  A clean player holds the plain
-- baseline, exactly as in every other game; everything special about the
-- virus lives in the VIRUS projector, granted on infection and never given
-- back, which is the shape of the game.  It is the baseline with three
-- things changed: how long Enlight must cool between beams (virus_cooldown,
-- the optics' own rather than hand-timed here), a pool of energy_max, which
-- become_virus() lowers to a fifth, and the role tag the beam carries — so
-- "is this beam viral" rides the payload field meant for it instead of
-- colliding with the projector's strength byte.
local P_VIRUS = 11
proj.define{
  vars     = { energy = "energy", spent = "energy_spent",
               reload = "reload", reload_ms = "reload_ms",
               icon = "energy_icon" },
  profiles = {
    -- bonus = false: the virus's projector is a role, not a pickup.
    { id = P_VIRUS, name = "VIRUS", bonus = false, cooldown_ms = "virus_cooldown",
      max_energy = "energy_max",
      recharge_delay_ms = proj.rel("recharge_delay_ms"),
      role_tag = 1 },
  },
}

local function is_virus() return virus_set[la.my_id()] == true end

-- What a claimed BONUS / MALUS totem does, picked per totem by the DM.
-- No lives here, so LIFE works on energy, against this player's own pool
-- (the virus's is a fifth of the others').  A virus keeps the VIRUS
-- projector in hand — it is what infects — so a projector bonus reaching
-- a virus becomes the LIFE bonus instead.
local pickup = std.pickup_effect{
  proj         = proj,
  start_energy = function(vars) return vars.energy_max end,
  projector_ok = function() return not is_virus() end,
}

local function note_infected(vars, id)
  if id and not virus_set[id] then
    virus_set[id] = true
    virus_count   = virus_count + 1
    vars.clean_left = la.player_count() - virus_count
  end
end

local function become_virus(vars)
  if not virus_set[la.my_id()] then
    virus_set[la.my_id()] = true
    virus_count = virus_count + 1
    la.broadcast_relay(MSG_INFECTED)
  end
  vars.clean_left = la.player_count() - virus_count
  vars.clean_secs = vars.game_time - vars.time_left
  vars.role       = "VIRUS"
  -- The virus recharges to only a fifth of the others' energy max.
  vars.energy_max = math.max(1, vars.start_energy // 5)
  if vars.energy > vars.energy_max then vars.energy = vars.energy_max end
  proj.grant(vars, P_VIRUS)       -- longer cooldown, and a viral role tag
  -- No OUT state in this game: infection is the way out of the game as a
  -- clean player, so that is where a DIM malus lifts.
  proj.set_dim(vars, false)
  la.background(virus_bg)
  la.show("YOU ARE THE VIRUS!", 0)
  la.ui("RoleChange")
end

local function last_clean(vars) return vars.clean_left <= 1 end

-- Is this beam viral?  Byte 3 is the projector's role tag (byte 1 is its
-- strength): the VIRUS projector tags 1, everything else 0 — an area hit
-- carries its policy's tag, SPLASH's 0.
local function viral(pkt) return pkt.len >= 3 and pkt:byte(3) == 1 end

local function game_over(vars)
  la.background()
  la.clear_tray()
  la.show("Game over!", 3000)
  la.ui("EndGame")
end

return {
  api     = 1,
  type_id = 0x0007,               -- next free GameTypeId
  name    = "Virus",

  initial_state = S.CLEAN,
  scoring_state = S.GAME_END,
  score_msg     = MSG.SCORE_COLLECT,

  config = {
    { id = "start_energy",   name = "Energy",   min = 10,  max = 60,   step = 5,   default = 30   },
    { id = "recharge_secs",  name = "Recharge", min = 5,   max = 20,   step = 5,   default = 10   },
    { id = "virus_cooldown", name = "CoolMs",   min = 250, max = 3000, step = 250, default = 1000 },
    -- How long a virus a clean player put down stays out, at least.
    { id = "virus_respawn_secs", name = "Respawn", min = 10, max = 100, step = 10, default = 30 },
    { id = "game_time",      name = "Time",     min = 60,  max = 900,  step = 60,  default = 600  },
  },

  vars = {
    -- Patient zero: a random joined player, drawn by the DM at Start and
    -- sent with the start signal.  Not in the menu.
    { id = "virus_id",   draw = "player" },
    { id = "energy",     default = 30 },
    { id = "energy_spent", default = 0 },
    -- The projector's reload clock, read by the energy cell's bar:
    -- reload = millis the wait began (0 = not waiting), reload_ms =
    -- how long it takes.  Both written by projector.lua.
    { id = "reload",       default = 0 },
    { id = "reload_ms",    default = 0 },
    -- The icon of the projector in hand (an la.icons value), written by
    -- projector.lua and read by the energy cell: FAST, LONG, … replace
    -- the standard energy glyph while they are the one in use.
    { id = "energy_icon",  default = la.icons.ENERGY },
    { id = "energy_max", default = 30 },
    { id = "time_left",  default = 600, countdown_in = { S.CLEAN, S.VIRUS, S.DOWN } },
    -- The down wait, for the DOWN loading bar: written by std.respawn_wait().
    { id = "respawn_zero", default = 0 },
    { id = "respawn_from", default = 0 },
    { id = "respawn_ms",   default = 0 },
    { id = "clean_left", default = 0  },   -- clean players remaining
    { id = "points",     default = 0  },   -- see the header
    { id = "infections", default = 0  },   -- players this device infected
    { id = "clean_secs", default = 0  },   -- how long we stayed clean
    -- The role, clearly stated on the LCD in both playing states.
    { id = "role", text = true, len = 8, default = "CLEAN" },
  },

  monitor = {
    -- CLEAN screen
    { var = "role",       icon = "ROLE",   col = 0, row = 0, states = { S.CLEAN } },
    { var = "clean_left", icon = "LIFE",   col = 1, row = 0, states = { S.CLEAN } },
    { var = "time_left",  icon = "TIME",   col = 0, row = 1, states = { S.CLEAN, S.VIRUS, S.DOWN } },
    -- Energy, and — while the pool is empty — a bar filling over the
    -- recharge.  The projector owns both the duration and the instant
    -- the wait began, because a refill starts at the trigger's RELEASE,
    -- not when the pool hit zero.
    { var = "energy",     icon = "ENERGY", col = 1, row = 1, states = { S.CLEAN, S.VIRUS },
      bar = true, bar_at = 0, fill_var = "reload_ms", start_var = "reload",
      icon_var = "energy_icon" },
    -- VIRUS screen
    { var = "role",       icon = "ROLE",   col = 0, row = 0, states = { S.VIRUS, S.DOWN } },
    { var = "points",     icon = "SCORE",  col = 1, row = 0, states = { S.VIRUS } },
    -- DOWN screen: a bar filling over the wait, from the instant it began
    { var = "respawn_zero", icon = "DOWN", col = 1, row = 0, states = { S.DOWN },
      bar = true, bar_at = 0, fill_var = "respawn_ms", start_var = "respawn_from" },
    { var = "points",     icon = "SCORE",  col = 1, row = 1, states = { S.DOWN } },
    -- GAME_END screen
    { var = "points",     icon = "SCORE",  col = 0, row = 0, states = { S.GAME_END } },
    { var = "clean_secs", icon = "TIME",   col = 1, row = 0, states = { S.GAME_END } },
    { var = "infections", icon = "LIFE",   col = 0, row = 1, states = { S.GAME_END } },
    { var = "role",       icon = "ROLE",   col = 1, row = 1, states = { S.GAME_END } },
  },

  winners = {
    { var = "points",     dir = "max" },   -- most points
    { var = "clean_secs", dir = "max" },   -- tie-break: stayed clean longest
  },

  totem_slots = {
    { role = "BONUS", min = 0, max = 16, options = proj.bonus_options() },
    { role = "MALUS", min = 0, max = 16, options = std.malus_options() },
  },
  teams = 0,
  time_left_var = "time_left",

  on_begin = function(vars)
    vars.energy_max = vars.start_energy
    vars.time_left  = vars.game_time
    vars.points     = 0
    vars.infections = 0
    vars.clean_secs = 0
    vars.role       = "CLEAN"
    virus_set        = {}
    virus_count      = 0
    pending_infected = false
    pending_down     = false
    downed_by        = nil
    respawn_at       = 0
    safe_until       = 0
    can_respawn      = false
    touching         = false
    touch.reset()
    has_totems = la.totem_for_role("BONUS", 0) ~= 0 or la.totem_for_role("MALUS", 0) ~= 0
    proj.reset(vars)                -- CLEAN in hand, pool full, optics pushed

    -- Everybody knows patient zero: the DM drew it and sent it at Start.
    virus_set[vars.virus_id] = true
    virus_count     = 1
    vars.clean_left = la.player_count() - 1
    if la.my_id() == vars.virus_id then
      pending_infected = true     -- the CLEAN->VIRUS rule fires on tick 1
    end
    la.ui("GameStart")
  end,

  on_message = {
    [S.CLEAN] = {
      -- A pickup totem gives itself to whoever answers, so only answer
      -- from arm's length: the claim has to mean "I am standing at it".
      [MSG.BONUS_BEACON] = std.pickup_claim{ rssi = PICKUP_RSSI, on_claim = pickup },
      [MSG.MALUS_BEACON] = std.pickup_claim{ rssi = PICKUP_RSSI, on_claim = pickup },
      [MSG.LIT] = function(vars, pkt)
        -- Only a viral lit infects; a clean player's is friendly fire.
        if viral(pkt) then
          pending_infected = true
          note_infected(vars, pkt.sender)   -- sender is certainly a virus
          return R.SHONE
        end
        return R.FRIEND
      end,
      [MSG_INFECTED] = function(vars, pkt)
        note_infected(vars, pkt.sender)
        la.show(la.player_short(pkt.sender) .. " is INFECTED", 3000)
      end,
    },
    [S.VIRUS] = {
      -- A pickup totem gives itself to whoever answers, so only answer
      -- from arm's length: the claim has to mean "I am standing at it".
      [MSG.BONUS_BEACON] = std.pickup_claim{ rssi = PICKUP_RSSI, on_claim = pickup },
      [MSG.MALUS_BEACON] = std.pickup_claim{ rssi = PICKUP_RSSI, on_claim = pickup },
      -- Any beam puts a virus down (direct, or the area of a clean SPLASH
      -- landing nearby) — except in the grace after it came back.
      [MSG.LIT] = function(vars, pkt)
        if la.now() < safe_until then return R.SAFE end
        pending_down = true
        downed_by    = la.player_short(pkt.sender)
        return viral(pkt) and R.VDOWN or R.SHONE
      end,
      [MSG_INFECTED] = function(vars, pkt)
        note_infected(vars, pkt.sender)
        la.show(la.player_short(pkt.sender) .. " is INFECTED", 3000)
      end,
    },
    [S.DOWN] = {
      [MSG.LIT] = function() return R.DOWN end,
      [MSG_INFECTED] = function(vars, pkt)
        note_infected(vars, pkt.sender)
        la.show(la.player_short(pkt.sender) .. " is INFECTED", 3000)
      end,
    },
  },

  on_reply = {
    [MSG.LIT] = {
      -- One knock-out code, two meanings: who shot says which.  A clean
      -- SPLASH's area downs come back here too, credited by the firmware.
      [R.SHONE] = function(vars, reply)
        local who = la.player_short(reply.sender)
        if is_virus() then
          vars.infections = vars.infections + 1
          vars.points     = vars.points + PTS.INFECT
          la.show(who .. " INFECTED! +" .. PTS.INFECT, 3000)
        else
          vars.points = vars.points + PTS.CLEAN_DOWNS_VIRUS
          la.show(who .. " is DOWN! +" .. PTS.CLEAN_DOWNS_VIRUS, 3000)
        end
        la.ui("Lit")
      end,
      [R.VDOWN] = function(vars, reply)
        vars.points = vars.points + PTS.VIRUS_DOWNS_VIRUS
        la.show(la.player_short(reply.sender) .. " is DOWN! +" .. PTS.VIRUS_DOWNS_VIRUS, 3000)
        la.ui("Lit")
      end,
      [R.FRIEND] = function() la.ui("Friend") end,
      [R.SAFE]   = function() la.ui("Immune") end,
      [R.DOWN]   = function(vars, reply)
        la.show(la.player_short(reply.sender) .. " is down", 2000)
        la.ui("Immune")
      end,
    },
    -- A totem answered this down virus's touch: it is at a totem, which
    -- has just played its arrival chaser.  That answer is the respawn.
    [MSG.TOTEM_TOUCH] = {
      [0] = function() if touching then can_respawn = true end end,
    },
  },

  rules = {
    { from = S.CLEAN, to = S.GAME_END,
      when   = function(vars) return vars.time_left <= 0 or last_clean(vars) end,
      -- Also the action the runner runs when the DM ends the match from
      -- here: whatever ends the game, a player still clean gets the bonus.
      action = function(vars)
        vars.clean_secs = vars.game_time - vars.time_left
        vars.points     = vars.points + PTS.STILL_CLEAN
        game_over(vars)
        la.show("Still clean! +" .. PTS.STILL_CLEAN, 3000)
      end },
    { from = S.CLEAN, to = S.VIRUS,
      when   = function() return pending_infected end,
      action = function(vars)
        pending_infected = false
        become_virus(vars)
      end },
    { from = S.VIRUS, to = S.GAME_END,
      when   = function(vars) return vars.time_left <= 0 or last_clean(vars) end,
      action = game_over },
    { from = S.VIRUS, to = S.DOWN,
      when   = function() return pending_down end,
      action = function(vars)
        pending_down = false
        can_respawn  = false
        touching     = false
        touch.reset()
        respawn_at   = std.respawn_wait(vars, vars.virus_respawn_secs)
        proj.strip(vars)            -- lifts DIM; the VIRUS projector stays
        la.background()             -- no viral pulse while out of play
        la.clear_tray()
        -- Two persistent lines for the wait, credit on top; the first
        -- becomes "Go to a totem" once the wait is over (update below).
        la.show("Wait to respawn", 0)
        la.show("LIT by " .. (downed_by or "?"), 0)
        la.ui("Down")
      end },
    { from = S.DOWN, to = S.GAME_END,
      when   = function(vars) return vars.time_left <= 0 or last_clean(vars) end,
      action = game_over },
    { from = S.DOWN, to = S.VIRUS,
      when   = function() return can_respawn end,
      action = function(vars)
        can_respawn = false
        touching    = false
        downed_by   = nil
        vars.energy = vars.energy_max
        safe_until  = la.now() + GRACE_MS
        la.background(virus_bg)
        la.clear_tray()
        la.show("Back in game!", 1000)
        la.show("Safe for " .. GRACE_MS // 1000 .. " s", GRACE_MS)
        la.ui("Up")
      end },
  },

  update = {
    -- Both roles run the same body: which projector is in hand already
    -- carries the difference, in the cooldown and in the role tag.
    [S.CLEAN] = function(vars)
      local target = proj.result(vars)
      if target then la.send(target, MSG.LIT, proj.payload(vars)) end
      proj.tick(vars)
    end,
    [S.VIRUS] = function(vars)
      if la.now() < safe_until then return end     -- grace: no shining
      local target = proj.result(vars)
      if target then la.send(target, MSG.LIT, proj.payload(vars)) end
      proj.tick(vars)
    end,
    -- Down: no shining.  Once the wait is over, back on the spot with no
    -- totem in the match; otherwise touch, once a second, until a totem
    -- in reach answers.
    [S.DOWN] = function(vars)
      if la.now() < respawn_at then return end
      if not has_totems then can_respawn = true; return end
      if not touching then
        touching = true
        la.clear_tray()
        la.show("Go to a totem", 0)
        la.show("LIT by " .. (downed_by or "?"), 0)
      end
      touch.send()
    end,
  },

  totems = {
    BONUS = std.totems.bonus(),
    MALUS = std.totems.malus(),
  },
}
