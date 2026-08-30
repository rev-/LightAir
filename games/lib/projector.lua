-- ================================================================
-- LightAir projector — the light-beam device a player carries.
--
-- Load with:  local proj = la.lib("projector")
--
-- This is the object between the game, the ruleset and Enlight.  It owns
-- what used to be spread across every ruleset's shine loop: the optics in
-- hand, the energy that pays for a beam, how that energy comes back, how
-- far the beam reaches, what a hit weighs on the wire, and which
-- projectors the player is carrying.
--
-- It replaces std.shiner, which was the same idiom with one fixed
-- profile.  A game that declares nothing but the baseline behaves exactly
-- as it did under the shiner.
--
-- ---- Layering ----------------------------------------------------
--
-- Enlight never learns what a projector is.  Three verbs carry
-- everything across: la.shine_config{} pushes the optics, la.shine()
-- starts a burst, la.shine_result() reports what came back.  Every
-- decision — whether to fire, what it cost, whether the target was in
-- range, what the hit weighs — is made here, in Lua, where it can be
-- retuned by shipping a file instead of reflashing.
--
-- ---- Why range is decided here -----------------------------------
--
-- la.shine_result() reports an ESTIMATED DISTANCE; it does not gate on
-- it.  That is deliberate: gating in the driver would freeze one policy
-- into firmware, while here a profile can gate on distance, correct for
-- target colour, or grade an effect by range — all as data.
--
-- ---- Allocation --------------------------------------------------
--
-- Everything allocates at define() time.  tick() runs allocation-free.
-- ================================================================

local P = { }

-- ---- Declaration, filled by define() -----------------------------
local cfg          = nil   -- the whole declaration
local defs         = {}    -- id -> clamped profile
local var          = {}    -- role -> var id, see define()

-- ---- Inventory ---------------------------------------------------
-- Slot 1 is the baseline: always held, never counted against
-- max_owned, never evicted.  Slots 2..n are powered.
local slots        = {}    -- { id, energy, acquired_at, last_shine_at, ramp_at }
local active_idx   = 1

-- ---- Live state --------------------------------------------------
local was_active   = false
local release_at   = 0
-- True between an accepted beam and the trigger release that follows it.
-- The recharge wait STARTS at that release, so nothing may tick while this
-- is set — otherwise a player who had been idle would see the pool refill
-- on the very tick they emptied it.
local awaiting_release = false
local ready_at     = 0     -- millis before which trigger() refuses
local lit_at       = {}    -- target id -> millis of the last accepted hit
local evicted_name = nil

-- ================================================================
--   Limits.  The load-time equivalent of the C++ clamp the projector
--   used to carry: a typo in a game file is corrected once, here,
--   instead of reaching the hardware or the balance.
--
--   The three optical bounds are re-applied by la.shine_config; these
--   exist so a bad value is visible at load rather than silently
--   corrected at the boundary.
-- ================================================================
local LIM = {
  cycles            = { 1,  100 },
  cooldown_ms       = { 0,  10000 },
  range_m           = { 0,  100 },     -- 0 = whatever the device can see
  cost              = { 0,  10 },
  max_energy        = { 0,  200 },
  strength          = { 0,  10 },
  role_tag          = { 0,  255 },
  rssi_min          = { -120, 0 },     -- dBm; 0 = no gate
  target_immunity_ms = { 0, 30000 },
  ready_ms          = { 0,  5000 },
  recharge_delay_secs = { 0, 60 },
  recharge_secs     = { 0,  60 },
  max_owned         = { 1,  8 },
}

local function clamp(v, lo, hi)
  if v < lo then return lo end
  if v > hi then return hi end
  return v
end

-- A profile field is either a literal number or the id of a game var, so
-- that a value the config menu owns (the energy pool, the recharge time)
-- tracks the menu without the profile being rebuilt.  This is the
-- property std.shiner had, kept.
local function val(vars, v, dflt)
  if v == nil then return dflt end
  if type(v) == "string" then return vars[v] or dflt end
  return v
end

-- Clamp only literals: a var id is resolved per tick and clamped there.
local function clamp_field(p, field)
  local lim = LIM[field]
  if lim and type(p[field]) == "number" then
    p[field] = clamp(p[field], lim[1], lim[2])
  end
end

-- ================================================================
--   THE BASELINE
--
--   Reproduces std.shiner exactly: one energy per beam, a full refill
--   after the configured idle, pool and delay read from the game's own
--   config vars.  A game that declares no profiles gets this and
--   behaves as it did before the projector existed.
-- ================================================================
-- ICON_ENERGY, resolved once: a profile that names no icon, or names one
-- the firmware does not carry, keeps the standard energy glyph.
local ICON_FALLBACK = (la.icons and la.icons.ENERGY) or 0

local BASELINE = {
  id                  = 0,
  name                = "BASE",
  cost                = 1,
  max_energy          = "start_energy",
  recharge            = "refill",
  recharge_delay_secs = "recharge_secs",
  strength            = 1,
  role_tag            = 0,
  range_m             = 0,
  rssi_min            = 0,
  target_immunity_ms  = 0,
  ready_ms            = 0,
}

-- ================================================================
--   STANDARD PROFILES
--
--   Ready-made profiles a game can drop straight into its `profiles`
--   list.  Their ids are FIXED and reserved, because a projector id
--   travels on the wire: a splash beacon names the projector that fired,
--   and every receiver looks the profile up by that id locally.  A game's
--   own profiles should start above this range.
--
--     profiles = { proj.standard.SPLASH, { id = 10, name = "MINE", ... } }
-- ================================================================
P.standard = {
  -- SPLASH — the burst projector.  A heavy, slow shot whose point is not
  -- the direct hit but what it does to everyone standing near the person
  -- it lands on: the direct hit is a single standard hit, while the
  -- beacon it triggers hands out two at close range and one further out.
  --
  -- It is the ONLY profile that declares a splash.  Splash is loud, in
  -- radio traffic and in play, and a field where every projector splashed
  -- would be chaos rather than tactics.
  SPLASH = {
    id       = 1,
    name     = "SPLASH",
    icon     = "SPLASH",

    -- Optics: a long integration for a heavy shot, and a cooldown that
    -- makes it a considered shot rather than a held trigger.
    cycles      = 20,
    cooldown_ms = 900,
    range_m     = 12,        -- a burst weapon, not a sniper

    -- Economy: few charges, slow to come back.  The reload bar earns its
    -- keep on this one.
    cost                = 2,
    max_energy          = 8,
    recharge            = "refill",
    recharge_delay_secs = 6,

    -- Handling: heavy to bring up after a switch.
    ready_ms = 600,

    -- Effect.  target_immunity_ms stops the same target absorbing the
    -- direct hit twice inside one burst's echo.
    strength           = 1,
    target_immunity_ms = 1500,

    splash = {
      on     = "lit",        -- every accepted hit bursts, not just the fatal one
      -- Graded: RSSI is coarse, so a misread moves a bystander one band
      -- rather than between hit and nothing.
      bands  = { { -55, 2 }, { -70, 1 } },
      strength = 1,          -- what a receiver without the profile falls back to
    },

    -- A low double thump, so the burst does not sound like the baseline.
    shine_action = {
      priority = 2,
      steps = { { ms = 60, freq = 1400, vib = 200, rgb = { 255, 120, 0 } },
                { ms = 90, freq =  900, vib = 255, rgb = { 255, 40, 0 } } },
    },
  },
}

-- ================================================================
--   define(declaration)
--
--     proj.define{
--       vars = { energy = "energy", spent = "energy_spent",
--                reload = "reload", reload_secs = "reload_secs" },
--       max_owned = 3,
--       is_available = function(id) return ... end,   -- optional
--       profiles = { { id = 1, name = "STRONG", ... }, ... },
--     }
--
--   Profile id 0 is the baseline.  Declaring one replaces the standard
--   baseline's values in place; it is still structural and still
--   undroppable, so a baseline may not be recharge = "consumed" — that
--   would ask for it to be deleted at zero.
-- ================================================================
function P.define(decl)
  cfg  = decl or {}
  var  = cfg.vars or {}
  defs = {}

  local base = {}
  for k, v in pairs(BASELINE) do base[k] = v end
  defs[0] = base

  for _, raw in ipairs(cfg.profiles or {}) do
    local p = {}
    for k, v in pairs(raw) do p[k] = v end
    local id = p.id or 0
    if id == 0 then
      -- Retune the baseline in place rather than naming a different id:
      -- everything else treats slot 1 as structural.
      if p.recharge == "consumed" then p.recharge = "none" end
      for k, v in pairs(p) do defs[0][k] = v end
    else
      for k in pairs(LIM) do clamp_field(p, k) end
      defs[id] = p
    end
  end
  for k in pairs(LIM) do clamp_field(defs[0], k) end

  cfg.max_owned = clamp(cfg.max_owned or 3, LIM.max_owned[1], LIM.max_owned[2])
  return P
end

-- ---- Inventory helpers -------------------------------------------
local function profile_of(idx) return defs[slots[idx].id] end
local function active()        return profile_of(active_idx) end

local function find_slot(id)
  for i = 1, #slots do
    if slots[i].id == id then return i end
  end
  return nil
end

-- The baseline is never consulted: it is the fallback, so a baseline that
-- could report itself unavailable would leave the player unable to shine.
local function available(id)
  if id == 0 then return true end
  if not cfg.is_available then return true end
  return cfg.is_available(id) and true or false
end

-- ---- Energy ------------------------------------------------------
-- The energy var is the authority for the ACTIVE projector's pool: a
-- ruleset may write it directly (Outflow's passive drain does).  Its
-- value is banked into the slot on a switch and reloaded from the next.
local function get_energy(vars)     return vars[var.energy] or 0 end
local function set_energy(vars, v)  vars[var.energy] = v end

local function max_energy(vars, p)  return val(vars, p.max_energy, 0) end

-- ================================================================
--   The reload bar.
--
--   The LCD shows the energy cell as a number, and as a filling bar
--   while the pool is empty.  The bar's clock cannot be the moment
--   energy reached zero: with a "refill" recharge the wait starts when
--   the TRIGGER IS RELEASED, so a player holding a dead trigger would
--   watch a bar complete while nothing came back.
--
--   So the projector publishes both halves and owns their timing:
--     reload      — millis at which the recharge clock started, 0 = idle
--     reload_secs — how long this profile's recharge takes, in seconds
--   Both are ordinary game vars, which is what lets the display bind to
--   them without the projector reaching into the display layer.
-- ================================================================
local function reload_total_secs(vars, p)
  local delay = val(vars, p.recharge_delay_secs, 0)
  if p.recharge == "ramp" then
    return delay + val(vars, p.recharge_secs, 0)
  end
  return delay
end

local function publish_reload(vars, started_at, p)
  if var.reload      then vars[var.reload]      = started_at end
  if var.reload_secs then vars[var.reload_secs] = reload_total_secs(vars, p) end
end

-- ================================================================
--   Switching
-- ================================================================
local function activate(vars, idx)
  -- Bank the outgoing pool before leaving: a ruleset may have written the
  -- energy var directly since the last switch.
  if idx ~= active_idx then
    slots[active_idx].energy = get_energy(vars)
  end
  active_idx = idx

  local p = active()
  set_energy(vars, slots[idx].energy)

  -- Optics.  Queued by the verb and applied in the OUTPUT phase, so this
  -- can never reconfigure Enlight mid-measurement.
  la.shine_config{ reps = val(vars, p.cycles, nil),
                   cooldown_ms = val(vars, p.cooldown_ms, nil) }
  if la.shine_action then la.shine_action(p.shine_action) end
  if var.name then vars[var.name] = p.name or "" end
  -- The energy cell's icon follows the projector in hand.  Published as an
  -- la.icons value into an ordinary var, so the display binding reads it
  -- through a pointer and nothing here reaches into the display layer.
  if var.icon then
    vars[var.icon] = (p.icon and la.icons and la.icons[p.icon]) or ICON_FALLBACK
  end

  ready_at = la.now() + val(vars, p.ready_ms, 0)
  publish_reload(vars, 0, p)
end

local function evict_oldest(vars)
  if #slots <= 1 then return end
  local oldest = 2
  for i = 3, #slots do
    if slots[i].acquired_at < slots[oldest].acquired_at then oldest = i end
  end
  evicted_name = defs[slots[oldest].id].name or "?"
  P.drop(vars, slots[oldest].id)
end

-- ================================================================
--   Public inventory API
-- ================================================================
function P.owns(id) return find_slot(id) ~= nil end
function P.active_id() return slots[active_idx].id end
function P.active_profile() return active() end
function P.owned_count() return #slots end

function P.select(vars, id)
  local idx = find_slot(id)
  if not idx or not available(id) then return false end
  if idx ~= active_idx then
    activate(vars, idx)
    la.ui("ProjectorChange")
  end
  return true
end

-- Add at full energy, or refill if already held.  Keeps acquired_at on a
-- re-grant so restocking cannot be used to dodge eviction.
function P.give(vars, id)
  if not defs[id] then return false end
  if id == 0 then return true end                 -- always held already

  local held = find_slot(id)
  if held then
    slots[held].energy = max_energy(vars, defs[id])
    if held == active_idx then set_energy(vars, slots[held].energy) end
    return true
  end

  if #slots - 1 >= cfg.max_owned then evict_oldest(vars) end
  local now = la.now()
  slots[#slots + 1] = { id = id, energy = max_energy(vars, defs[id]),
                        acquired_at = now, last_shine_at = now, ramp_at = now }
  return true
end

function P.grant(vars, id)
  if not P.give(vars, id) then return false end
  return P.select(vars, id)
end

function P.drop(vars, id)
  local idx = find_slot(id)
  if not idx or idx == 1 then return false end    -- slot 1 is structural
  local was_active_slot = (idx == active_idx)

  table.remove(slots, idx)
  if was_active_slot then
    -- Point at the baseline BEFORE activating, so activate() skips its
    -- usual "bank the outgoing pool" step: that slot no longer exists and
    -- its energy went with it.
    active_idx = 1
    activate(vars, 1)
    la.ui("ProjectorChange")
  elseif idx < active_idx then
    active_idx = active_idx - 1                    -- the removal shifted us down
  end
  return true
end

local function cycle(vars, dir)
  if #slots <= 1 then return end
  for step = 1, #slots - 1 do
    local idx = ((active_idx - 1 + dir * step) % #slots) + 1
    if available(slots[idx].id) then
      activate(vars, idx)
      la.ui("ProjectorChange")
      return
    end
  end
end

function P.next(vars) cycle(vars,  1) end
function P.prev(vars) cycle(vars, -1) end

-- True once per eviction, for the tray message.  Reading it clears it.
function P.consume_evicted()
  local n = evicted_name
  evicted_name = nil
  return n
end

-- ================================================================
--   reset(vars) — from on_begin
-- ================================================================
function P.reset(vars)
  if not cfg then P.define{} end
  local now = la.now()
  slots = { { id = 0, energy = 0, acquired_at = now,
              last_shine_at = now, ramp_at = now } }
  active_idx  = 1
  was_active  = false
  release_at  = 0
  awaiting_release = false
  lit_at      = {}
  evicted_name   = nil
  splash_sent_at = nil

  slots[1].energy = max_energy(vars, defs[0])
  activate(vars, 1)
  set_energy(vars, slots[1].energy)
  if var.spent then vars[var.spent] = 0 end
  ready_at = 0                       -- no deploy delay on the opening beam
end

-- ================================================================
--   result() — interpret the measurement
--
--   Returns the target's player id, plus the estimated distance, or nil
--   when there is nothing to act on.  The second return is the reason,
--   so a ruleset can tell "missed" from "out of reach" and say so.
--
--   Fails OPEN on distance: a device with no reference calibration
--   reports 0 metres, and a profile that declares no range_m gates on
--   nothing.  Either way the behaviour is what it was before ranges
--   existed, which is what keeps an uncalibrated device playable.
-- ================================================================
function P.result(vars)
  local status, id, metres = la.shine_result()
  if status ~= "player" then return nil, status end

  local p = active()
  local reach = val(vars, p.range_m, 0)
  if reach > 0 and metres > 0 and metres > reach then
    return nil, "far"
  end
  return id, metres
end

-- ================================================================
--   Attacker-side anti-spam.
--
--   The window is per TARGET and deliberately survives a switch:
--   resetting it would turn switching into a way to bypass it.
-- ================================================================
function P.may_light(vars, target)
  local window = val(vars, active().target_immunity_ms, 0)
  if window <= 0 then return true end
  local t = lit_at[target]
  return t == nil or (la.now() - t) >= window
end

function P.note_lit(target) lit_at[target] = la.now() end

-- ================================================================
--   payload() — what a hit carries on the wire
--
--     la.send(target, MSG.LIT, proj.payload(vars))
--
--   [strength, projector id, role tag, rssi gate].  The gate travels as
--   a positive magnitude because payload bytes are unsigned: 50 means
--   -50 dBm.  A receiver running an older file reads the first byte and
--   ignores the rest, which is the same rule as "empty payload = one
--   standard hit".
-- ================================================================
function P.payload(vars)
  local p = active()
  local gate = val(vars, p.rssi_min, 0)
  return val(vars, p.strength, 1),
         P.active_id(),
         val(vars, p.role_tag, 0),
         (gate < 0) and -gate or 0
end

-- ================================================================
--   SPLASH
--
--   A player who has just absorbed a beam broadcasts a beacon; anyone
--   near enough absorbs a share of the same shot.  The reach is declared
--   by the ATTACKER's projector and relayed by the victim, so a
--   short-range profile splashes tightly and a heavy one does not.
--
--   Distance is judged from the RSSI of that beacon, which is coarse —
--   body shadowing alone is worth 10-20 dB at 2.4 GHz.  That is
--   acceptable here and nowhere else: a splash radius is meant to be
--   fuzzy, graded bands degrade by one step rather than between hit and
--   nothing, and there is no optical measurement to a bystander who was
--   never aimed at, so RSSI is not a worse choice than something better.
--
--   Wire format, MSG.SPLASH, single-hop:
--     [1] attacker's projector id — lets a bystander find the profile
--         locally and grade the damage; the flat values below stand in
--         when it cannot
--     [2] splash strength, in standard hits
--     [3] RSSI gate, positive magnitude (55 means -55 dBm)
--     [4] origin: 1 = a direct optical LIT.  ONLY a direct hit emits;
--         on_splash never calls emit_splash.  That is what stops one
--         beam from cascading across a whole field.
--     [5] the SHOOTER's id, so friendly fire is judged against whoever
--         fired rather than against the victim who relayed it
-- ================================================================
local SPLASH_ORIGIN_DIRECT = 1
-- nil, not 0: "never sent" has to be distinguishable from "sent at time
-- zero", or the rate limit would swallow the first beacon of a match.
local splash_sent_at       = nil

-- Cheapest useful rate limit: one beacon per shot, and never two inside
-- the same window even if a ruleset calls this twice for one event.
local SPLASH_MIN_GAP_MS = 250

local function splash_of(id)
  local p = defs[id]
  return p and p.splash or nil
end

-- Bands, when a profile declares them, ARE the reach: the outermost one is
-- the cutoff, and the flat `rssi` is just the one-band shorthand.  Keeping
-- one answer for "how far does this splash go" stops the two from
-- disagreeing, which would gate a bystander out at the flat threshold
-- before their band was ever consulted.
local function splash_gate(s)
  if s.bands and #s.bands > 0 then
    local weakest = s.bands[1][1]
    for _, band in ipairs(s.bands) do
      if band[1] < weakest then weakest = band[1] end
    end
    return weakest
  end
  return s.rssi or 0
end

-- Victim side.  `pkt` is the incoming LIT packet and `event` is what just
-- happened to this player — "lit" for any accepted hit, "shone" for the
-- one that put them down.  A profile declares which it answers.
function P.emit_splash(vars, pkt, event)
  if pkt.len < 2 then return false end
  local attacker_proj = pkt:byte(2)
  local s = splash_of(attacker_proj)
  if not s then return false end
  if (s.on or "lit") ~= (event or "lit") then return false end

  local now = la.now()
  if splash_sent_at and (now - splash_sent_at) < SPLASH_MIN_GAP_MS then return false end
  splash_sent_at = now

  local gate = splash_gate(s)
  -- Single-hop: la.broadcast, never la.broadcast_relay.  A flooded splash
  -- would reach the entire field, which is the opposite of a radius.
  la.broadcast(la.msg.SPLASH,
               attacker_proj,
               s.strength or 1,
               (gate < 0) and -gate or 0,
               SPLASH_ORIGIN_DIRECT,
               pkt.sender)
  return true
end

-- Bystander side.  Returns how much this player absorbs, or nil for a
-- beacon that does not reach them.  The second return says why, so a
-- ruleset can stay quiet rather than reporting a miss.
function P.on_splash(vars, pkt)
  if pkt.len < 4 then return nil, "short" end
  local origin = pkt:byte(4)
  -- Only a direct optical hit may splash.  A beacon claiming any other
  -- origin is either a cascade or a stray, and is dropped either way.
  if origin ~= SPLASH_ORIGIN_DIRECT then return nil, "cascade" end
  -- Never splash yourself: the emitter already took the direct hit.
  if pkt.sender == la.my_id() then return nil, "self" end

  -- Graded by distance where the profile says so, flat otherwise.  The
  -- bands are read locally by attacker projector id rather than sent, so
  -- a profile can carry as many as it likes without growing the packet;
  -- the gate on the wire is what a bystander without the profile falls
  -- back to, and it already carries the outermost band.
  local s = splash_of(pkt:byte(1))
  if s and s.bands then
    for _, band in ipairs(s.bands) do
      if pkt.rssi >= band[1] then return band[2] end
    end
    return nil, "far"
  end

  local gate = pkt:byte(3)
  if gate > 0 and pkt.rssi < -gate then return nil, "far" end
  return pkt:byte(2)
end

-- ================================================================
--   Recharge
-- ================================================================
local function tick_recharge(vars, p, now)
  local mode = p.recharge or "refill"
  if mode == "none" or mode == "consumed" then return end

  local max = max_energy(vars, p)
  local e   = get_energy(vars)
  if max <= 0 or e >= max then
    publish_reload(vars, 0, p)
    return
  end

  local delay_ms = val(vars, p.recharge_delay_secs, 0) * 1000
  if (now - release_at) < delay_ms then
    -- Waiting out the idle: the clock started at the release, which is
    -- exactly what the bar must show.
    publish_reload(vars, release_at, p)
    return
  end

  if type(mode) == "function" then
    mode(vars, now - release_at)
    return
  end

  if mode == "refill" then
    set_energy(vars, max)
    publish_reload(vars, 0, p)
    return
  end

  -- ramp: one unit every recharge_secs / max, stepped in integers.
  local total_ms = val(vars, p.recharge_secs, 0) * 1000
  local step_ms  = (max > 0) and (total_ms // max) or 0
  if step_ms < 1 then step_ms = 1 end
  local s = slots[active_idx]
  while now >= s.ramp_at and get_energy(vars) < max do
    set_energy(vars, get_energy(vars) + 1)
    s.ramp_at = s.ramp_at + step_ms
  end
  if get_energy(vars) >= max then
    s.ramp_at = now
    publish_reload(vars, 0, p)
  else
    publish_reload(vars, release_at, p)
  end
end

-- ================================================================
--   tick(vars) — the whole trigger-to-beam path, once per cycle
--
--   Energy is spent ONLY on a run Enlight actually accepted.  la.shine()
--   returns false while a burst is still in flight or cooling down, and
--   the short-circuit is what keeps one trigger pull costing one beam
--   rather than one per tick.
--
--   Returns true while the trigger is down, for a ruleset that cares.
-- ================================================================
function P.tick(vars)
  local p      = active()
  local now    = la.now()
  local active_trigger = la.trigger_down(1)

  local cost = val(vars, p.cost, 1)
  if active_trigger
     and now >= ready_at
     and get_energy(vars) >= cost
     and (cfg.can == nil or cfg.can(vars))
     and la.shine() then
    set_energy(vars, get_energy(vars) - cost)
    if var.spent then vars[var.spent] = (vars[var.spent] or 0) + cost end
    la.ui_enlight(la.shine_ms())
    slots[active_idx].last_shine_at = now
    slots[active_idx].ramp_at       = now
    awaiting_release                = true

    -- A spent "consumed" projector leaves, but never the baseline.
    if p.recharge == "consumed" and get_energy(vars) <= 0 and active_idx ~= 1 then
      P.drop(vars, P.active_id())
      p = active()
    end
  end

  -- The wait is anchored by the release that FOLLOWS a beam.  A press that
  -- spends nothing — an empty pool — neither re-anchors it nor holds it
  -- back: once started, the wait runs to completion, so leaning on a dead
  -- trigger still gets the energy back on time.  Only a press that actually
  -- fires restarts the clock, and it does so from its own release.
  if was_active and not active_trigger and awaiting_release then
    release_at       = now
    awaiting_release = false
  end
  was_active = active_trigger

  -- Runs whatever the trigger is doing.  The one thing that stops it is a
  -- beam waiting for its release, because until then the wait has not begun.
  if not awaiting_release then tick_recharge(vars, p, now) end

  -- A projector that just became unavailable hands back to the baseline.
  if active_idx ~= 1 and not available(P.active_id()) then
    P.select(vars, 0)
  end

  return active_trigger
end

return P
