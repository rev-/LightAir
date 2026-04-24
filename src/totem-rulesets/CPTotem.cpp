#include <LightAir.h>
#include "TotemRoleIds.h"
#include "CPTotemPolicy.h"
#include "../config.h"

// ================================================================
// CPTotem — generalised "point-giving" totem runner.
//
// Replaces the original two-state (NEUTRAL/OWNED) implementation
// with a seven-state machine parameterised entirely by the 32-bit
// mode word and duration fields in the activation payload.
//
// See CPTotemPolicy.h for the full protocol documentation.
// ================================================================

using RadioMsg::MSG_CP_BEACON;
using RadioMsg::MSG_CP_SCORE;

static constexpr uint32_t CP_BEACON_INTERVAL_MS   = 2000;
static constexpr uint32_t CP_DEFAULT_COUNTDOWN_MS = 10000;
static constexpr uint32_t CP_DEFAULT_COOLDOWN_MS  = 30000;
static constexpr uint32_t CP_DEFAULT_PRIME_MS     =  5000;
static constexpr uint8_t  CP_NO_PLAYER            = 0;
static constexpr uint8_t  CP_NO_TEAM              = 0xFF;
static constexpr uint8_t  CP_NO_ROLE              = 0;
static constexpr uint8_t  CP_SECS_NA              = 0xFF;
static constexpr uint8_t  CP_PRESENCE_MAX         = 16;


class CPTotem : public LightAir_TotemRunner {

    // ── Configuration (set at activation; never changed during a game) ──
    uint32_t _mode;
    uint32_t _countdownMs;   // capture / scoring countdown
    uint32_t _cooldownMs;    // post-score cooldown; also CONTESTED timeout
    uint32_t _suspendMs;     // 0 = indefinite (exit only via RESUME)
    uint32_t _primeMs;       // PRIMED phase duration (TIMER_PRIMED only)

    // ── Mutable game state ───────────────────────────────────────────
    CPState  _state;
    uint16_t _context;

    // ── Current association ──────────────────────────────────────────
    uint8_t  _assocPlayer;
    uint8_t  _assocTeam;
    uint8_t  _assocRole;

    // ── Pre-contest snapshot (restored on RESOLUTION_PREV_WINS) ──────
    uint8_t  _prevPlayer;
    uint8_t  _prevTeam;
    uint8_t  _prevRole;

    // ── Timers ───────────────────────────────────────────────────────
    uint32_t _stateEnteredAt;  // millis() when current state was entered
    uint32_t _accumulatedMs;   // TIMER_ACCUMULATE: total entity-present time
    uint32_t _windowStart;     // start of current beacon window

    // ── Presence collection (reset each window) ──────────────────────
    struct PresenceEntry { uint8_t senderId, team, role; };
    PresenceEntry _presence[CP_PRESENCE_MAX];
    uint8_t       _presenceCount;

    // ── Mode field accessors ─────────────────────────────────────────
    uint8_t assocScope()   const { return (uint8_t)( _mode        & 0x03u); }
    uint8_t contestBeh()   const { return (uint8_t)((_mode >>  2) & 0x03u); }
    uint8_t postScore()    const { return (uint8_t)((_mode >>  4) & 0x03u); }
    uint8_t resolution()   const { return (uint8_t)((_mode >>  6) & 0x03u); }
    uint8_t timerMode()    const { return (uint8_t)((_mode >>  8) & 0x03u); }

    bool pauseInContest()  const { return (_mode & CPPolicy::FLAG_TIMER_PAUSE_IN_CONTEST) != 0; }
    bool lockOnPrime()     const { return (_mode & CPPolicy::FLAG_LOCK_ON_PRIME)          != 0; }
    bool lockOnCount()     const { return (_mode & CPPolicy::FLAG_LOCK_ON_COUNT)          != 0; }
    bool roleGated()       const { return (_mode & CPPolicy::FLAG_ROLE_GATED)             != 0; }
    bool suspendEnabled()  const { return (_mode & CPPolicy::FLAG_SUSPEND_ENABLED)        != 0; }
    bool contestTimer()    const { return (_mode & CPPolicy::FLAG_CONTEST_TIMER)          != 0; }
    bool reactivatable()   const { return (_mode & CPPolicy::FLAG_REACTIVATABLE)          != 0; }

    // ── Association helpers ──────────────────────────────────────────

    void clearAssoc() {
        _assocPlayer = CP_NO_PLAYER;
        _assocTeam   = CP_NO_TEAM;
        _assocRole   = CP_NO_ROLE;
    }

    void applyAssoc(const PresenceEntry& e) {
        _assocPlayer = e.senderId;
        _assocTeam   = e.team;
        _assocRole   = e.role;
    }

    void savePrev() {
        _prevPlayer = _assocPlayer;
        _prevTeam   = _assocTeam;
        _prevRole   = _assocRole;
    }

    void restorePrev() {
        _assocPlayer = _prevPlayer;
        _assocTeam   = _prevTeam;
        _assocRole   = _prevRole;
    }

    // Does entry e match the entity currently tracked by _assoc*?
    bool matchesAssoc(const PresenceEntry& e) const {
        switch (assocScope()) {
            case CPPolicy::Scope::PLAYER: return e.senderId == _assocPlayer;
            case CPPolicy::Scope::ROLE:   return e.role     == _assocRole;
            case CPPolicy::Scope::ANY:    return true;
            default:                      return e.team     == _assocTeam;  // TEAM
        }
    }

    // Is the currently associated entity represented in this window's presence?
    bool assocPresent() const {
        for (uint8_t i = 0; i < _presenceCount; i++)
            if (matchesAssoc(_presence[i])) return true;
        return false;
    }

    // First presence entry that does NOT match _assoc*; nullptr if none.
    const PresenceEntry* firstOther() const {
        for (uint8_t i = 0; i < _presenceCount; i++)
            if (!matchesAssoc(_presence[i])) return &_presence[i];
        return nullptr;
    }

    // Same as matchesAssoc but against an arbitrary snapshot, not _assoc*.
    bool snapshotPresent(uint8_t pid, uint8_t tm, uint8_t rl) const {
        for (uint8_t i = 0; i < _presenceCount; i++) {
            const PresenceEntry& e = _presence[i];
            switch (assocScope()) {
                case CPPolicy::Scope::PLAYER: if (e.senderId == pid) return true; break;
                case CPPolicy::Scope::ROLE:   if (e.role     == rl)  return true; break;
                case CPPolicy::Scope::ANY:    return _presenceCount > 0;
                default:                      if (e.team     == tm)  return true; break;
            }
        }
        return false;
    }

    // ── State entry ──────────────────────────────────────────────────

    // preserveAccum: keep _accumulatedMs (CONTEST_PAUSE returning to ASSOCIATED).
    void enterState(CPState s, uint32_t now, LightAir_TotemOutput& out,
                    bool preserveAccum = false) {
        _state          = s;
        _stateEnteredAt = now;
        if (!preserveAccum) _accumulatedMs = 0;
        updateBackground(out);
    }

    // ── UI ───────────────────────────────────────────────────────────

    void updateBackground(LightAir_TotemOutput& out) const {
        switch (_state) {

            case CPState::IDLE:
                out.ui.trigger(TotemUIEvent::Idle);
                break;

            case CPState::PRIMED: {
                // Dim version of the associated entity's colour (quarter brightness).
                uint8_t r = 64, g = 64, b = 64;
                if (assocScope() != CPPolicy::Scope::PLAYER && _assocTeam < TeamColors::kCount) {
                    r = TeamColors::kColors[_assocTeam][0] / 4;
                    g = TeamColors::kColors[_assocTeam][1] / 4;
                    b = TeamColors::kColors[_assocTeam][2] / 4;
                } else if (_assocPlayer < PlayerDefs::MAX_PLAYER_ID) {
                    r = PlayerColors::kColors[_assocPlayer][0] / 4;
                    g = PlayerColors::kColors[_assocPlayer][1] / 4;
                    b = PlayerColors::kColors[_assocPlayer][2] / 4;
                }
                out.ui.trigger(TotemUIEvent::Custom1, r, g, b);
                break;
            }

            case CPState::ASSOCIATED:
                if (assocScope() == CPPolicy::Scope::PLAYER ||
                    assocScope() == CPPolicy::Scope::ANY) {
                    out.ui.trigger(TotemUIEvent::Control, 0xFF, _assocPlayer);
                } else {
                    out.ui.trigger(TotemUIEvent::Control, _assocTeam);
                }
                break;

            case CPState::CONTESTED:
                out.ui.trigger(TotemUIEvent::ControlContest);
                break;

            case CPState::SUSPENDED:
                // Dim white pulse — totem is frozen.
                out.ui.trigger(TotemUIEvent::Custom2, 40, 40, 40);
                break;

            case CPState::COOLDOWN: {
                // Very dim colour of last scorer, indicating temporary lockout.
                uint8_t r = 32, g = 32, b = 32;
                if (assocScope() != CPPolicy::Scope::PLAYER && _assocTeam < TeamColors::kCount) {
                    r = TeamColors::kColors[_assocTeam][0] / 8;
                    g = TeamColors::kColors[_assocTeam][1] / 8;
                    b = TeamColors::kColors[_assocTeam][2] / 8;
                } else if (_assocPlayer < PlayerDefs::MAX_PLAYER_ID) {
                    r = PlayerColors::kColors[_assocPlayer][0] / 8;
                    g = PlayerColors::kColors[_assocPlayer][1] / 8;
                    b = PlayerColors::kColors[_assocPlayer][2] / 8;
                }
                out.ui.trigger(TotemUIEvent::Custom3, r, g, b);
                break;
            }

            case CPState::INACTIVE:
                // Off — totem is done for this game session.
                out.ui.trigger(TotemUIEvent::Custom4, 0, 0, 0);
                break;
        }
    }

    // ── Beacon & scoring ─────────────────────────────────────────────

    uint8_t countdownRemainingSecs(uint32_t now) const {
        if (_state == CPState::ASSOCIATED || _state == CPState::CONTESTED) {
            uint32_t elapsed = (timerMode() == CPPolicy::Timer::ACCUMULATE)
                               ? _accumulatedMs
                               : (now - _stateEnteredAt);
            if (elapsed >= _countdownMs) return 0;
            uint32_t rem = (_countdownMs - elapsed) / 1000u;
            return (rem > 254u) ? 254u : (uint8_t)rem;
        }
        if (_state == CPState::PRIMED) {
            uint32_t elapsed = now - _stateEnteredAt;
            if (elapsed >= _primeMs) return 0;
            uint32_t rem = (_primeMs - elapsed) / 1000u;
            return (rem > 254u) ? 254u : (uint8_t)rem;
        }
        return CP_SECS_NA;
    }

    void broadcastBeacon(uint32_t now, LightAir_TotemOutput& out) const {
        uint8_t pl[7] = {
            (uint8_t)_state,
            _assocPlayer,
            _assocTeam,
            _assocRole,
            (uint8_t)( _context       & 0xFFu),
            (uint8_t)((_context >> 8) & 0xFFu),
            countdownRemainingSecs(now),
        };
        out.radio.broadcast(MSG_CP_BEACON, pl, 7);
    }

    void awardScore(LightAir_TotemOutput& out) {
        uint8_t pl[3] = { _assocPlayer, _assocTeam, _assocRole };
        out.radio.broadcast(MSG_CP_SCORE, pl, 3);
        out.ui.trigger(TotemUIEvent::Bonus);
    }

    // ── Post-score transition ─────────────────────────────────────────

    void handlePostScore(uint32_t now, LightAir_TotemOutput& out) {
        switch (postScore()) {
            case CPPolicy::PostScore::LOOP:
                clearAssoc();
                enterState(CPState::IDLE, now, out);
                break;
            case CPPolicy::PostScore::COOLDOWN:
                enterState(CPState::COOLDOWN, now, out);
                break;
            case CPPolicy::PostScore::INACTIVE:
                enterState(CPState::INACTIVE, now, out);
                break;
            case CPPolicy::PostScore::REPEAT:
                // Stay in ASSOCIATED; restart the countdown.
                _stateEnteredAt = now;
                _accumulatedMs  = 0;
                break;
        }
    }

    // ── Contest handling ──────────────────────────────────────────────

    // Called when a different entity appears while PRIMED or ASSOCIATED.
    void handleConflict(uint32_t now, LightAir_TotemOutput& out,
                        const PresenceEntry& other) {
        switch (contestBeh()) {
            case CPPolicy::Contest::HOLD:
                // Ignore the newcomer.
                break;

            case CPPolicy::Contest::RESET:
                // New entity takes over immediately.
                applyAssoc(other);
                _stateEnteredAt = now;
                _accumulatedMs  = 0;
                updateBackground(out);
                break;

            case CPPolicy::Contest::PAUSE:
            case CPPolicy::Contest::PAUSE_THEN_RESET:
                savePrev();
                enterState(CPState::CONTESTED, now, out);
                break;
        }
    }

    // ── Per-state window evaluators ───────────────────────────────────

    void evaluateIdle(uint32_t now, LightAir_TotemOutput& out) {
        if (_presenceCount == 0) return;

        // Pick the first valid candidate.
        for (uint8_t i = 0; i < _presenceCount; i++) {
            if (roleGated() && _presence[i].role == CP_NO_ROLE) continue;
            applyAssoc(_presence[i]);

            if (timerMode() == CPPolicy::Timer::PRIMED) {
                enterState(CPState::PRIMED, now, out);
            } else if (timerMode() == CPPolicy::Timer::IMMEDIATE) {
                enterState(CPState::ASSOCIATED, now, out);
                awardScore(out);
                handlePostScore(now, out);
            } else {
                enterState(CPState::ASSOCIATED, now, out);
            }
            return;
        }
    }

    void evaluatePrimed(uint32_t now, LightAir_TotemOutput& out) {
        bool ap                  = assocPresent();
        const PresenceEntry* oth = lockOnPrime() ? nullptr : firstOther();

        if (!ap && !lockOnPrime()) {
            // Associated entity left and association is not locked.
            clearAssoc();
            enterState(CPState::IDLE, now, out);
            return;
        }

        if (oth) {
            handleConflict(now, out, *oth);
            return;
        }

        if ((now - _stateEnteredAt) >= _primeMs) {
            // Prime phase complete — start the real countdown.
            enterState(CPState::ASSOCIATED, now, out);
        }
    }

    void evaluateAssociated(uint32_t now, LightAir_TotemOutput& out) {
        bool ap                  = assocPresent();
        const PresenceEntry* oth = lockOnCount() ? nullptr : firstOther();

        if (!ap) {
            if (timerMode() == CPPolicy::Timer::ACCUMULATE) {
                // Timer pauses; check for a new entity.
                if (oth) handleConflict(now, out, *oth);
                // else: just wait
            } else {
                clearAssoc();
                enterState(CPState::IDLE, now, out);
            }
            return;
        }

        if (oth) {
            handleConflict(now, out, *oth);
            return;
        }

        // Associated entity present, no conflict.
        if (timerMode() == CPPolicy::Timer::ACCUMULATE)
            _accumulatedMs += (now - _windowStart);

        uint32_t elapsed = (timerMode() == CPPolicy::Timer::ACCUMULATE)
                           ? _accumulatedMs
                           : (now - _stateEnteredAt);

        if (elapsed >= _countdownMs) {
            awardScore(out);
            handlePostScore(now, out);
        }
    }

    void evaluateContested(uint32_t now, LightAir_TotemOutput& out) {
        // Optional hard timeout for the contest (uses _countdownMs as duration).
        if (contestTimer() && (now - _stateEnteredAt) >= _countdownMs) {
            clearAssoc();
            enterState(CPState::IDLE, now, out);
            return;
        }

        if (_presenceCount == 0) {
            clearAssoc();
            enterState(CPState::IDLE, now, out);
            return;
        }

        bool prevPresent  = snapshotPresent(_prevPlayer, _prevTeam, _prevRole);

        // Temporarily adopt prev snapshot so firstOther() sees "others" correctly.
        uint8_t sp = _assocPlayer, st = _assocTeam, sr = _assocRole;
        _assocPlayer = _prevPlayer; _assocTeam = _prevTeam; _assocRole = _prevRole;
        const PresenceEntry* otherEntry = firstOther();
        bool othersPresent = (otherEntry != nullptr);
        _assocPlayer = sp; _assocTeam = st; _assocRole = sr;

        // Both sides still here — contest unresolved.
        if (prevPresent && othersPresent) return;

        bool resetTimer = (contestBeh() == CPPolicy::Contest::PAUSE_THEN_RESET);

        if (prevPresent && !othersPresent) {
            // Original holder remains alone.
            restorePrev();
            enterState(CPState::ASSOCIATED, now, out, !resetTimer);
            return;
        }

        // Only non-prev entities (or no one) remain.
        switch (resolution()) {
            case CPPolicy::Resolution::PREV_WINS:
                // Prev did not win; neutralise regardless.
                clearAssoc();
                enterState(CPState::IDLE, now, out);
                break;

            case CPPolicy::Resolution::LAST_STANDING:
                if (othersPresent) {
                    applyAssoc(*otherEntry);
                    enterState(CPState::ASSOCIATED, now, out);  // new entity: always fresh start
                } else {
                    clearAssoc();
                    enterState(CPState::IDLE, now, out);
                }
                break;

            case CPPolicy::Resolution::NEUTRALIZE:
                clearAssoc();
                enterState(CPState::IDLE, now, out);
                break;

            case CPPolicy::Resolution::FIRST_REPLY:
                // First presence entry in this window wins; always fresh countdown.
                applyAssoc(_presence[0]);
                enterState(CPState::ASSOCIATED, now, out);
                break;
        }
    }

    void evaluateSuspended(uint32_t now, LightAir_TotemOutput& out) {
        if (_suspendMs > 0 && (now - _stateEnteredAt) >= _suspendMs) {
            // Timed suspension expired; clear and return to IDLE.
            clearAssoc();
            enterState(CPState::IDLE, now, out);
        }
    }

    void evaluateCooldown(uint32_t now, LightAir_TotemOutput& out) {
        if ((now - _stateEnteredAt) >= _cooldownMs) {
            clearAssoc();
            enterState(CPState::IDLE, now, out);
        }
    }

    void evaluateWindow(uint32_t now, LightAir_TotemOutput& out) {
        switch (_state) {
            case CPState::IDLE:       evaluateIdle(now, out);       break;
            case CPState::PRIMED:     evaluatePrimed(now, out);     break;
            case CPState::ASSOCIATED: evaluateAssociated(now, out); break;
            case CPState::CONTESTED:  evaluateContested(now, out);  break;
            case CPState::SUSPENDED:  evaluateSuspended(now, out);  break;
            case CPState::COOLDOWN:   evaluateCooldown(now, out);   break;
            case CPState::INACTIVE:                                  break;
        }
    }

public:
    CPTotem() { reset(); }

    void onActivate(const uint8_t* payload, uint8_t len,
                    LightAir_TotemOutput& out) override {
        _mode        = 0;
        _countdownMs = CP_DEFAULT_COUNTDOWN_MS;
        _cooldownMs  = CP_DEFAULT_COOLDOWN_MS;
        _suspendMs   = 0;
        _primeMs     = CP_DEFAULT_PRIME_MS;

        if (len >= 5) {
            _mode = (uint32_t)payload[1]
                  | ((uint32_t)payload[2] <<  8)
                  | ((uint32_t)payload[3] << 16)
                  | ((uint32_t)payload[4] << 24);
        }
        if (len >= 7) {
            uint16_t s = (uint16_t)payload[5] | ((uint16_t)payload[6] << 8);
            if (s > 0) _countdownMs = (uint32_t)s * 1000u;
        }
        if (len >= 10 && payload[9] > 0)
            _cooldownMs = (uint32_t)payload[9] * 1000u;
        if (len >= 11)
            _suspendMs = (uint32_t)payload[10] * 1000u;
        if (len >= 12 && payload[11] > 0)
            _primeMs = (uint32_t)payload[11] * 1000u;

        uint16_t initCtx = 0;
        if (len >= 9) initCtx = (uint16_t)payload[7] | ((uint16_t)payload[8] << 8);

        reset();
        _context     = initCtx;
        _windowStart = millis();
        out.ui.trigger(TotemUIEvent::Idle);
    }

    void onMessage(const RadioPacket& msg, LightAir_TotemOutput& out) override {
        if (msg.msgType != MSG_CP_BEACON + 1) return;
        if (msg.payloadLen == 0) return;

        auto action = (CPAction)msg.payload[0];

        if (action == CPAction::PRESENCE) {
            if (_presenceCount < CP_PRESENCE_MAX)
                _presence[_presenceCount++] = { msg.senderId, msg.team, msg.role };
            return;
        }

        uint32_t now = millis();

        if (action == CPAction::SUSPEND && suspendEnabled() &&
            _state != CPState::SUSPENDED && _state != CPState::INACTIVE) {
            enterState(CPState::SUSPENDED, now, out);
            return;
        }

        if (action == CPAction::RESUME && _state == CPState::SUSPENDED) {
            clearAssoc();
            enterState(CPState::IDLE, now, out);
            return;
        }

        if (action == CPAction::REACTIVATE && reactivatable() &&
            _state == CPState::INACTIVE) {
            clearAssoc();
            enterState(CPState::IDLE, now, out);
            return;
        }

        if (action == CPAction::CONTEXT && msg.payloadLen >= 3) {
            _context = (uint16_t)msg.payload[1] | ((uint16_t)msg.payload[2] << 8);
            broadcastBeacon(now, out);  // push updated context to players immediately
            return;
        }
    }

    void update(LightAir_TotemOutput& out) override {
        uint32_t now = millis();
        if ((now - _windowStart) < CP_BEACON_INTERVAL_MS) return;

        evaluateWindow(now, out);

        _presenceCount = 0;
        _windowStart   = now;
        broadcastBeacon(now, out);
    }

    void reset() override {
        _state          = CPState::IDLE;
        _context        = 0;
        clearAssoc();
        _prevPlayer     = CP_NO_PLAYER;
        _prevTeam       = CP_NO_TEAM;
        _prevRole       = CP_NO_ROLE;
        _stateEnteredAt = 0;
        _accumulatedMs  = 0;
        _windowStart    = 0;
        _presenceCount  = 0;
    }
};

// ── Singleton ────────────────────────────────────────────────────
static CPTotem s_cp;
LightAir_TotemRunner* totemRunner_cp = &s_cp;
