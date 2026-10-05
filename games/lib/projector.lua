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
-- profile.
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

-- ---- Shared pool -------------------------------------------------
-- define{ shared_pool = true }: every projector the player holds draws
-- from ONE pool, the baseline's — for a ruleset where the pool is the
-- player's life (Outflow).  The pool's size, its recharge and DIM are the
-- baseline's; each projector still brings its own optics, cost per beam,
-- strength, feedback and area.  A pickup puts a projector in hand without
-- touching the pool (a full pool would be a free heal), and switching
-- moves no energy.  The pool's state (its ramp clock) lives in slot 1.
local shared       = false

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

-- ---- DIM (a MALUS) -----------------------------------------------
-- While dimmed, every projector the player holds has a smaller pool, a
-- slower recharge and a longer cooldown.  Fixed factors: retune here.
-- min_cooldown_ms stands in for a profile that declares no cooldown at
-- all (the baseline) — doubling nothing would not be "longer".
local DIM = { energy_div = 2, recharge_mul = 2, cooldown_mul = 2,
              min_cooldown_ms = 250 }
local dimmed = false
local bonus_ids = {}       -- BONUS option label -> projector id, see bonus_options()

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
  -- Milliseconds, not seconds: a recharge quantised to whole seconds is far
  -- too coarse to separate a projector that snaps back from one that
  -- crawls.
  recharge_delay_ms = { 0, 60000 },
  recharge_ms       = { 0, 60000 },
  recharge_step_ms  = { 1, 60000 },
  max_owned         = { 1,  8 },
}

-- Fields that were renamed, and what replaced them.  Named rather than
-- silently ignored: a profile that keeps the old spelling would otherwise
-- get the field's default and behave subtly wrong.
local RETIRED = {
  recharge_delay_secs = "recharge_delay_ms",
  recharge_secs       = "recharge_ms",
}

local function clamp(v, lo, hi)
  if v < lo then return lo end
  if v > hi then return hi end
  return v
end

-- A profile field is a literal number, the id of a game var, or a function
-- of vars.  The var-id form is what lets a value the config menu owns (the
-- energy pool, the recharge time) track the menu without the profile being
-- rebuilt — the property std.shiner had, kept.  The function form covers
-- the rest: a config var in seconds feeding a field in milliseconds, say.
local function val(vars, v, dflt)
  if v == nil then return dflt end
  local t = type(v)
  if t == "string"   then return vars[v] or dflt end
  if t == "function" then return v(vars) or dflt end
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
--   One energy per beam, a full refill after the configured idle, pool
--   and delay read from the game's own config vars.  Its optics are
--   declared outright rather than left to whatever Enlight last held:
--   a baseline with no cycles of its own would inherit the previous
--   projector's, so switching back from LONG would keep LONG's beam.
--
--   range_m is a LABEL here and everywhere: what the profile is meant to
--   reach.  Nothing gates on it (see result()).
-- ================================================================
-- ICON_ENERGY, resolved once: a profile that names no icon, or names one
-- the firmware does not carry, keeps the standard energy glyph.
local ICON_FALLBACK = (la.icons and la.icons.ENERGY) or 0

local BASELINE = {
  id                  = 0,
  name                = "BASE",
  cycles              = 10,
  cooldown_ms         = 50,
  range_m             = 40,
  cost                = 1,
  max_energy          = "start_energy",
  recharge            = "refill",
  -- The menu owns this one in seconds; the projector works in ms.
  recharge_delay_ms   = function(vars) return (vars.recharge_secs or 0) * 1000 end,
  strength            = 1,
  role_tag            = 0,
  rssi_min            = 0,
  target_immunity_ms  = 0,
  ready_ms            = 0,
}

-- ================================================================
--   Relations to the baseline.
--
--   A powered projector is a power-up only relative to the baseline it
--   replaces in hand.  Fixed numbers cannot promise that: the menu owns
--   the baseline's pool and recharge, so a strong enough baseline would
--   out-shoot every "powered" one.  So the standard profiles state their
--   values as functions of the baseline's RESOLVED value — whatever this
--   game declared for id 0, read through its config vars on the tick.
--
--   Every field is stated that way, including the ones the baseline
--   fixes today (cycles, cooldown): they may become menu values, and a
--   relation keeps holding when they do.
--
--     cycles = proj.rel("cycles", function(r) return r * 5 end)
--     cost   = proj.rel("cost")                   -- same as the baseline
--
--   The result is clamped to the field's LIM, like a literal at load.
-- ================================================================
local function base_value(vars, field)
  local b = defs[0]
  return val(vars, b and b[field], 0)
end

local function rel(field, f)
  local lim = LIM[field]
  return function(vars)
    local v = base_value(vars, field)
    if f then v = f(v) end
    if lim then v = clamp(v, lim[1], lim[2]) end
    return v
  end
end
P.rel = rel

local function half(x)       return x // 2 end
local function twice(x)      return x * 2 end
-- A pool may never round down to nothing: a projector with no energy
-- could be held but never fired.
local function half_pool(x)  return math.max(1, x // 2) end

-- ================================================================
--   STANDARD PROFILES
--
--   Ready-made profiles a game can drop straight into its `profiles`
--   list.  Their ids are FIXED and reserved, because a projector id
--   travels on the wire: a LIT names the projector that fired, and every
--   receiver looks the profile up by that id locally.  A game's own
--   profiles should start above this range.
--
--     profiles = { proj.standard.SPLASH, { id = 10, name = "MINE", ... } }
--
--   Each is the baseline with a few relations changed (B = baseline):
--
--             pool   recharge               cooldown  cycles  strength
--     FAST    B      ramp: B/2 - 500 ms     B/2       B       B
--                    idle, then 10 ms/unit
--     STRONG  B/2    B/2                    2B        B       3
--     SPLASH  B/2    B/2                    2B        B       B + area
--     LONG    B/2    B                      B/2       5B      B
--
--   Every one costs what the baseline costs.  Every receiver resolves
--   these against the same config, so a profile looked up by id means the
--   same thing on every device.
-- ================================================================
P.standard = {
  -- SPLASH — the burst projector.  The point is not the direct hit but
  -- what it does to everyone standing near the person it lands on: the
  -- direct hit is a single standard hit, while the beacon it triggers
  -- hands out two at close range and one further out.
  --
  -- It is the ONLY profile that declares an area.  An area is loud, in
  -- radio traffic and in play, and a field where every projector splashed
  -- would be chaos rather than tactics.
  SPLASH = {
    id       = 1,
    name     = "SPLASH",
    icon     = "SPLASH",

    -- Optics: the baseline's beam, twice the wait between beams, so it is
    -- a considered shot rather than a held trigger.
    cycles      = rel("cycles"),
    cooldown_ms = rel("cooldown_ms", twice),
    range_m     = 40,        -- label only

    -- Economy: half the pool, back in half the time.
    cost              = rel("cost"),
    max_energy        = rel("max_energy", half_pool),
    recharge          = "refill",
    recharge_delay_ms = rel("recharge_delay_ms", half),

    -- Handling: heavy to bring up after a switch.
    ready_ms = 600,

    -- Effect.  target_immunity_ms stops the same target absorbing the
    -- direct hit twice inside one burst's echo.
    strength           = rel("strength"),
    target_immunity_ms = 1500,

    -- The area its hits throw around whoever they land on: an area policy
    -- for the firmware's area service (la.area_policy, registered by
    -- define() under this projector's id).  The firmware triggers it, grades
    -- it, credits it; the ruleset's own LIT handler decides what a hit does.
    area = {
      on       = "lit",      -- every hit that lands bursts, the knock-out included
      -- Graded: RSSI is coarse, so a misread moves a bystander one band
      -- rather than between hit and nothing.
      bands    = { { -55, 2 }, { -70, 1 } },
      friendly = "game",     -- the ruleset's friendly fire, against the shooter
      self     = false,      -- the shooter is never caught in their own area
      credit   = true,       -- an area knock-out scores for the shooter
    },

    -- A punch that flares out: the step ms are a SHAPE, scaled to fit the
    -- real burst, so what identifies the projector is the pattern rather
    -- than one note's pitch.  Short and hard, then longer and lower.
    shine_action = {
      priority = 2,
      steps = { { ms = 1, freq = 1400, vib = 255, rgb = { 255, 200, 0 } },
                { ms = 3, freq =  700, vib = 200, rgb = { 255,  60, 0 } } },
    },
  },

  -- FAST — the baseline's beam with half the wait between beams, and a
  -- pool that comes back sooner and trickles rather than arriving all at
  -- once, which suits a projector meant to be held down: an idle of half
  -- the baseline's recharge less half a second, then one unit every
  -- 10 ms.  It is the only standard profile that ramps.
  FAST = {
    id       = 2,
    name     = "FAST",
    icon     = "FAST",

    cycles      = rel("cycles"),
    cooldown_ms = rel("cooldown_ms", half),
    range_m     = 30,        -- label only

    cost              = rel("cost"),
    max_energy        = rel("max_energy"),
    recharge          = "ramp",
    recharge_delay_ms = rel("recharge_delay_ms", function(ms) return ms // 2 - 500 end),
    recharge_step_ms  = 10,

    ready_ms = 150,
    strength = rel("strength"),

    -- Two even rising ticks: light, quick, unmistakably not the heavy ones.
    shine_action = {
      priority = 1,
      steps = { { ms = 1, freq = 2600, vib = 80, rgb = {   0, 180, 255 } },
                { ms = 1, freq = 3400, vib = 90, rgb = { 120, 220, 255 } } },
    },
  },

  -- LONG — five times the baseline's integration, which is what reaches
  -- further.  Half the pool, but each beam is five times the light and
  -- the wait between beams is half the baseline's.  Slow to bring up.
  LONG = {
    id       = 3,
    name     = "LONG",
    icon     = "LONG",

    cycles      = rel("cycles", function(r) return r * 5 end),
    cooldown_ms = rel("cooldown_ms", half),
    range_m     = 0,         -- label only: no limit

    cost              = rel("cost"),
    max_energy        = rel("max_energy", half_pool),
    recharge          = "refill",
    recharge_delay_ms = rel("recharge_delay_ms"),

    ready_ms = 400,
    strength = rel("strength"),

    -- A short chirp then a long held tone: reads as "reaching out".
    shine_action = {
      priority = 1,
      steps = { { ms = 1, freq = 2000, vib = 120, rgb = { 220, 120, 255 } },
                { ms = 4, freq = 1200, vib = 150, rgb = { 180,   0, 255 } } },
    },
  },

  -- STRONG — the heavy hitter: one beam weighs three standard hits, which
  -- in a lives game is three lives at once.  Half the pool, back in half
  -- the time, and twice the wait between beams.
  STRONG = {
    id       = 4,
    name     = "STRONG",
    icon     = "STRONG",

    cycles      = rel("cycles"),
    cooldown_ms = rel("cooldown_ms", twice),
    range_m     = 40,        -- label only

    cost              = rel("cost"),
    max_energy        = rel("max_energy", half_pool),
    recharge          = "refill",
    recharge_delay_ms = rel("recharge_delay_ms", half),

    ready_ms = 400,
    strength = 3,

    -- Three descending notes, evenly spaced: heavy, and nothing else in the
    -- catalogue has three.
    shine_action = {
      priority = 1,
      steps = { { ms = 1, freq = 1000, vib = 255, rgb = { 255, 140, 0 } },
                { ms = 1, freq =  800, vib = 255, rgb = { 255,  80, 0 } },
                { ms = 1, freq =  600, vib = 230, rgb = { 200,  30, 0 } } },
    },
  },
}

-- ================================================================
--   define(declaration)
--
--     proj.define{
--       vars = { energy = "energy", spent = "energy_spent",
--                reload = "reload", reload_ms = "reload_ms" },
--       max_owned = 3,
--       shared_pool = false,          -- true: one pool for all (see above)
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
  shared = cfg.shared_pool and true or false

  local base = {}
  for k, v in pairs(BASELINE) do base[k] = v end
  defs[0] = base

  for _, raw in ipairs(cfg.profiles or {}) do
    local p = {}
    for k, v in pairs(raw) do p[k] = v end
    -- The recharge fields were seconds once and are milliseconds now.  A
    -- profile still naming the old key would silently get a zero delay, so
    -- say so at load rather than shipping a projector that never waits.
    for old, new in pairs(RETIRED) do
      if p[old] ~= nil then
        error("projector profile '" .. tostring(p.name or p.id) ..
              "' uses retired field '" .. old .. "'; use '" .. new .. "'", 0)
      end
    end
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

  -- The standard catalogue is always KNOWN, declared or not: a BONUS totem
  -- may hand any of it to any game.  Known is not owned — the inventory
  -- still starts with the baseline alone.
  for _, std_p in pairs(P.standard) do
    if not defs[std_p.id] then
      local p = {}
      for k, v in pairs(std_p) do p[k] = v end
      for k in pairs(LIM) do clamp_field(p, k) end
      defs[std_p.id] = p
    end
  end

  -- A profile's area is a policy for the firmware's area service, under
  -- the projector's own id: a hit from this projector that the target's
  -- ruleset answers TAKEN / SHONE triggers it, and every device registers
  -- the same policies because every device runs the same file.
  for id, p in pairs(defs) do
    if p.area then
      assert(id ~= 0, "the baseline projector cannot carry an area")
      local a = p.area
      la.area_policy(id, { projector = id, on = a.on or "lit", bands = a.bands,
                           friendly = a.friendly, self = a.self, credit = a.credit,
                           role_tag = type(p.role_tag) == "number" and p.role_tag or 0 })
    end
  end

  cfg.max_owned = clamp(cfg.max_owned or 3, LIM.max_owned[1], LIM.max_owned[2])
  return P
end

-- ---- Inventory helpers -------------------------------------------
local function profile_of(idx) return defs[slots[idx].id] end
local function active()        return profile_of(active_idx) end
-- Whose economy the pool follows, and where its clock is kept: the
-- projector in hand's, or with a shared pool always the baseline's.
local function pool_profile()  return shared and defs[0] or active() end
local function pool_slot()     return shared and slots[1] or slots[active_idx] end

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

local function max_energy(vars, p)
  local m = val(vars, p.max_energy, 0)
  if dimmed and m > 0 then m = math.max(1, m // DIM.energy_div) end
  return m
end

-- A recharge duration, stretched while dimmed.
local function stretch(ms)
  if dimmed then return ms * DIM.recharge_mul end
  return ms
end

-- The cooldown pushed to Enlight.  Always an integer, never nil: a profile
-- that declares none must still undo a dimmed (or a previous profile's)
-- cooldown when it becomes the one in hand.
local function cooldown_of(vars, p)
  local c = val(vars, p.cooldown_ms, 0)
  if dimmed then
    c = (c > 0) and (c * DIM.cooldown_mul) or DIM.min_cooldown_ms
  end
  return c
end

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
--     reload    — millis at which the recharge clock started, 0 = idle
--     reload_ms — how long this profile's recharge takes, in milliseconds
--   Both are ordinary game vars, which is what lets the display bind to
--   them without the projector reaching into the display layer.
--
--   The bar only ever shows while the pool is EMPTY, so it covers the wait
--   until energy starts coming back — for every recharge mode, that is the
--   recharge_delay_ms idle and nothing more.  A "ramp" projector starts
--   trickling the moment the idle ends, the pool leaves zero and the bar
--   gives way to the number; timing the bar to the whole refill made it
--   vanish a third of the way across.
-- ================================================================
local function reload_total_ms(vars, p)
  return stretch(val(vars, p.recharge_delay_ms, 0))
end

local function publish_reload(vars, started_at, p)
  if var.reload    then vars[var.reload]    = started_at end
  if var.reload_ms then vars[var.reload_ms] = reload_total_ms(vars, p) end
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
  -- A shared pool stays put: it is never loaded from a slot.
  if not shared then set_energy(vars, slots[idx].energy) end

  -- Optics.  Queued by the verb and applied in the OUTPUT phase, so this
  -- can never reconfigure Enlight mid-measurement.
  -- A profile that names no cycles takes the baseline's, never whatever
  -- Enlight was left holding by the projector before it.
  la.shine_config{ reps = val(vars, p.cycles, nil) or base_value(vars, "cycles"),
                   cooldown_ms = cooldown_of(vars, p) }
  if la.shine_action then la.shine_action(p.shine_action) end
  if var.name then vars[var.name] = p.name or "" end
  -- The energy cell's icon follows the projector in hand.  Published as an
  -- la.icons value into an ordinary var, so the display binding reads it
  -- through a pointer and nothing here reaches into the display layer.
  if var.icon then
    vars[var.icon] = (p.icon and la.icons and la.icons[p.icon]) or ICON_FALLBACK
  end

  ready_at = la.now() + val(vars, p.ready_ms, 0)
  -- A shared pool's reload clock is the pool's, and a switch leaves it be.
  if not shared then publish_reload(vars, 0, p) end
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

-- quiet = true skips the ProjectorChange cue, for a caller that plays its
-- own (a BONUS pickup has a sound of its own).
function P.select(vars, id, quiet)
  local idx = find_slot(id)
  if not idx or not available(id) then return false end
  if idx ~= active_idx then
    activate(vars, idx)
    if not quiet then la.ui("ProjectorChange") end
  end
  return true
end

-- Add at full energy, or refill if already held.  Keeps acquired_at on a
-- re-grant so restocking cannot be used to dodge eviction.  With a shared
-- pool there is nothing of its own to fill: the projector is added and
-- the pool is left as it is.
function P.give(vars, id)
  if not defs[id] then return false end
  if id == 0 then return true end                 -- always held already

  local held = find_slot(id)
  if held and shared then return true end
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

function P.grant(vars, id, quiet)
  if not P.give(vars, id) then return false end
  return P.select(vars, id, quiet)
end

-- ================================================================
--   DIM — the MALUS that weakens every projector the player holds.
--
--   set_dim(vars, true): half the pool (current energy clamped down to
--   it), twice the recharge, twice the cooldown.  set_dim(vars, false) lifts it; the pool then
--   refills by the ordinary recharge, not at once.  reset() lifts it too.
--   (A profile with no cooldown gets DIM.min_cooldown_ms instead.)
--   A ruleset lifts it when the player goes out of the game.
-- ================================================================
function P.set_dim(vars, on)
  on = on and true or false
  if on == dimmed then return end
  dimmed = on
  local p = active()
  -- Re-push the optics for the projector in hand.
  la.shine_config{ cooldown_ms = cooldown_of(vars, p) }
  if on and shared then
    local m = max_energy(vars, defs[0])
    if get_energy(vars) > m then set_energy(vars, m) end
  elseif on then
    slots[active_idx].energy = get_energy(vars)
    for i = 1, #slots do
      local m = max_energy(vars, profile_of(i))
      if slots[i].energy > m then slots[i].energy = m end
    end
    set_energy(vars, slots[active_idx].energy)
  end
  publish_reload(vars, 0, pool_profile())
end

function P.dimmed() return dimmed end

-- The pool of the projector in hand — or the shared pool — as it stands
-- (dimmed or not).
function P.max_energy(vars) return max_energy(vars, pool_profile()) end

-- ================================================================
--   BONUS totem options.
--
--   The list the DM cycles with O on a BONUS totem, for a game's
--   totem_slots entry:
--
--     { role = "BONUS", min = 0, max = 16, options = proj.bonus_options() }
--
--   "LIFE", then the standard catalogue in id order, then the game's own
--   profiles (id > 0) unless a profile says `bonus = false` — a practice
--   or role projector is not something a totem should hand out.  (A
--   ruleset whose pool is the player's life offers them too, with
--   define{ shared_pool = true }: picking one up then swaps no lives.)
--
--   Labels are cut to the menu's 8 characters; bonus_id() maps a label
--   back to its projector, so a long custom name still resolves.
--   Call after define().
-- ================================================================
function P.bonus_options()
  local list = { "LIFE" }
  bonus_ids = {}
  local ids = {}
  for id, p in pairs(defs) do
    if id ~= 0 and p.bonus ~= false then ids[#ids + 1] = id end
  end
  table.sort(ids)
  for _, id in ipairs(ids) do
    local label = string.sub(tostring(defs[id].name or id), 1, 8)
    if label ~= "LIFE" and not bonus_ids[label] then
      bonus_ids[label] = id
      list[#list + 1] = label
    end
  end
  return list
end

-- Projector id behind a BONUS option label, or nil (e.g. for "LIFE").
function P.bonus_id(label) return bonus_ids[label] end

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

-- ================================================================
--   strip(vars) — out of the game.
--
--   Everything a BONUS can hand out — the standard catalogue and a game's
--   own profiles, unless declared bonus = false — is dropped, DIM is
--   lifted, and the baseline comes back in hand with its own banked pool.
--   Role and practice projectors (bonus = false: Virus's VIRUS, a stand's
--   TRIAL) are kept: they are what the player IS, not what they picked up.
--   Silent: going out has its own cue.  Call it from the transition that
--   takes a player out, BEFORE a respawn writes the pool — the pool it
--   writes must be the baseline's, not the one of a projector about to go.
-- ================================================================
function P.strip(vars)
  P.set_dim(vars, false)
  local in_hand = slots[active_idx].id
  slots[active_idx].energy = get_energy(vars)      -- bank before reshuffling
  for i = #slots, 2, -1 do
    local p = defs[slots[i].id]
    if p and p.bonus ~= false then table.remove(slots, i) end
  end
  local idx = find_slot(in_hand)
  if idx then
    active_idx = idx                               -- a kept role projector
  else
    active_idx = 1                                 -- skip the banking step:
    activate(vars, 1)                              --   that slot is gone
  end
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
  dimmed         = false

  slots[1].energy = max_energy(vars, defs[0])
  activate(vars, 1)
  set_energy(vars, slots[1].energy)
  publish_reload(vars, 0, defs[0])   -- activate() leaves a shared pool's alone
  if var.spent then vars[var.spent] = 0 end
  ready_at = 0                       -- no deploy delay on the opening beam
end

-- ================================================================
--   result() — interpret the measurement
--
--   Returns the target's player id, plus the estimated distance, or nil
--   when there is nothing to act on.  The second return is the reason
--   (the measurement's status), so a ruleset can tell a miss from a
--   totem.
--
--   Nothing gates on distance.  A profile's range_m is a label for now:
--   the estimate is not trustworthy enough to refuse a hit on (see
--   docs/projector.md §2), and reach is what a profile's cycles buy.
-- ================================================================
function P.result(vars)
  local status, id, metres = la.shine_result()
  if status ~= "player" then return nil, status end
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

  local delay_ms = stretch(val(vars, p.recharge_delay_ms, 0))
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

  -- ramp: one unit every recharge_step_ms when the profile names one,
  -- otherwise every recharge_ms / max, stepped in integers.
  --
  -- Standard policy for a ramp: the trickle STARTS when the idle ends, the
  -- first unit arriving at that instant, so an empty pool fills at an even
  -- rate from then on — never before.  ramp_at is otherwise left at the
  -- last beam, and counting from there would hand back everything "earned"
  -- during the idle in one lump the moment it ends.
  local step_ms
  if p.recharge_step_ms ~= nil then
    step_ms = stretch(val(vars, p.recharge_step_ms, 1))
  else
    local total_ms = stretch(val(vars, p.recharge_ms, 0))
    step_ms = (max > 0) and (total_ms // max) or 0
  end
  if step_ms < 1 then step_ms = 1 end
  local s = pool_slot()
  local ramp_from = release_at + delay_ms
  if s.ramp_at < ramp_from then s.ramp_at = ramp_from end
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
    pool_slot().ramp_at             = now
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
  if not awaiting_release then tick_recharge(vars, pool_profile(), now) end

  -- A projector that just became unavailable hands back to the baseline.
  if active_idx ~= 1 and not available(P.active_id()) then
    P.select(vars, 0)
  end

  return active_trigger
end

return P
