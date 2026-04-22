#include "LightAir_GameRunner.h"
#include <Arduino.h>
#include <string.h>
#include <esp_system.h>

/* =========================================================
 *   BEGIN — one-time setup
 * ========================================================= */

void LightAir_GameRunner::begin(const LightAir_Game& game,
                                 LightAir_DisplayCtrl& display,
                                 LightAir_InputCtrl&   input,
                                 LightAir_Radio&       radio,
                                 LightAir_UICtrl*      ui) {
    _game    = &game;
    _display = &display;
    _input   = &input;
    _radio   = &radio;
    _ui      = ui;
    _bindingCount = 0;

    // -- Build display binding sets from MonitorVar::stateMask --

    // Pass 1: collect unique state indices that need a binding set.
    for (uint8_t v = 0; v < game.monitorCount; v++) {
        uint32_t mask = game.monitorVars[v].stateMask;
        for (uint8_t s = 0; s < 32 && mask; s++, mask >>= 1) {
            if (!(mask & 1)) continue;
            bool found = false;
            for (uint8_t b = 0; b < _bindingCount; b++)
                if (_bindings[b].state == s) { found = true; break; }
            if (!found && _bindingCount < DisplayDefaults::MAX_SETS) {
                uint8_t setId = display.createBindingSet();
                if (setId != 255)
                    _bindings[_bindingCount++] = { s, setId };
            }
        }
    }

    // Pass 2: bind each monitor var to the sets for its states.
    for (uint8_t v = 0; v < game.monitorCount; v++) {
        const MonitorVar& var = game.monitorVars[v];
        for (uint8_t b = 0; b < _bindingCount; b++) {
            if (!(var.stateMask & (1u << _bindings[b].state))) continue;
            display.selectBindingSet(_bindings[b].setId);
            const uint8_t px = var.col * DisplayDefaults::CELL_WIDTH;
            const uint8_t py = var.row * DisplayDefaults::CELL_HEIGHT
                               + 3 * DisplayDefaults::FONT_HEIGHT;
            if (var.type == VarType::INT)
                display.bindIntVariable(var.asInt, var.icon, px, py);
            else
                display.bindStringVariable(var.asChars, var.icon, px, py);
        }
    }

    // Create an empty binding set (no vars) used to freeze the display after scoreAnnounce.
    _emptyBindingSetId = display.createBindingSet();
    _endExitReady = false;

    // Reset state and activate the initial binding set.
    *game.currentState = game.initialState;
    activateStateDisplay(game.initialState);

    // Stamp the game's typeId on the radio layer so all outgoing packets
    // carry it and incoming packets from other games are filtered out.
    radio.setTypeId(game.typeId);

    // User-provided setup (radio init, opening messages, etc.)
    if (game.onBegin) game.onBegin(display, radio, ui, *this);
}

/* =========================================================
 *   ROSTER
 * ========================================================= */

void LightAir_GameRunner::clearRoster() {
    _rosterCount = 0;
}

void LightAir_GameRunner::addToRoster(uint8_t id) {
    if (_rosterCount >= GameDefaults::MAX_PARTICIPANTS) return;
    for (uint8_t i = 0; i < _rosterCount; i++)
        if (_roster[i] == id) return;  // ignore duplicate
    _roster[_rosterCount++] = id;
}

/* =========================================================
 *   TOTEMS
 * ========================================================= */

void LightAir_GameRunner::clearTotems() {
    _totemCount = 0;
}

void LightAir_GameRunner::addTotem(uint8_t id, uint8_t roleId) {
    if (_totemCount >= GameDefaults::MAX_PARTICIPANTS) return;
    for (uint8_t i = 0; i < _totemCount; i++)
        if (_totems[i].id == id) return;  // ignore duplicate
    _totems[_totemCount++] = { id, roleId };
}

uint8_t LightAir_GameRunner::totemIdForRole(uint8_t roleId, uint8_t idx) const {
    uint8_t count = 0;
    for (uint8_t t = 0; t < _totemCount; t++) {
        if (_totems[t].roleId == roleId) {
            if (count == idx) return _totems[t].id;
            count++;
        }
    }
    return 0;  // not found
}

/* =========================================================
 *   TEAM MAP
 * ========================================================= */

void LightAir_GameRunner::setTeam(uint8_t id, uint8_t team) {
    if (id < PlayerDefs::MAX_PLAYER_ID) _teamMap[id] = team;
}

uint8_t LightAir_GameRunner::teamOf(uint8_t id) const {
    if (id < PlayerDefs::MAX_PLAYER_ID) return _teamMap[id];
    return 0xFF;
}

/* =========================================================
 *   UPDATE — one loop iteration
 * ========================================================= */

void LightAir_GameRunner::update() {
    uint32_t loopStart = millis();

    // ---- Step 1: READ ----
    const InputReport& inputs = _input->poll();
    const RadioReport& radio  = _radio->poll();

    // ---- Step 2: LOGIC ----
    GameOutput output;

    if (_scoreActive) {
        scoreUpdate(inputs, radio, output);
        _display->update();
        flushOutput(output);
        while ((millis() - loopStart) < GameDefaults::LOOP_MS) {}
        return;
    }

    // ---- Score collection helpers (derived constants) ----
    const bool     scoringEnabled = _game->winnerVars &&
                                    _game->winnerVarCount > 0 &&
                                    _game->scoringState != 255 &&
                                    _rosterCount > 0;
    const uint32_t expectedMask   = _rosterCount < 32
                                    ? (1u << _rosterCount) - 1u
                                    : 0xFFFFFFFFu;

    // Step 2a: Infrastructure intercepts — handle before DirectRadioRules.
    // Marked events are skipped by the DirectRadioRules loop below.
    bool infraHandled[RADIO_MAX_PENDING] = {};

    // MSG_END_GAME: force scoringState entry on any device that hasn't yet transitioned.
    for (uint8_t e = 0; e < radio.count; e++) {
        const RadioEvent& ev = radio.events[e];
        if (ev.type != RadioEventType::MessageReceived) continue;
        if (ev.packet.msgType != GameDefaults::MSG_END_GAME) continue;
        infraHandled[e] = true;
        if (scoringEnabled && *_game->currentState != _game->scoringState) {
            *_game->currentState = _game->scoringState;
            activateStateDisplay(_game->scoringState);
            output.ui.trigger(LightAir_UICtrl::UIEvent::EndGame);
        }
    }

    // MSG_TOTEM_BEACON: reply with the totem's assigned roleId in payload[0].
    // payload[1] = *configSecs for this role if a requirement has one set.
    // No reply is sent to non-totem senders or unconfigured totems.
    for (uint8_t e = 0; e < radio.count; e++) {
        const RadioEvent& ev = radio.events[e];
        if (ev.type           != RadioEventType::MessageReceived) continue;
        if (ev.packet.msgType != RadioMsg::MSG_TOTEM_BEACON)      continue;
        infraHandled[e] = true;

        uint8_t id = ev.packet.senderId;
        if (!TotemDefs::isTotemId(id)) continue;

        for (uint8_t t = 0; t < _totemCount; t++) {
            if (_totems[t].id != id) continue;
            uint8_t roleId = _totems[t].roleId;
            uint8_t buf[2] = { roleId, 0 };
            uint8_t bufLen = 1;
            // Include configSecs if any requirement for this role has one.
            if (_game->totemRequirements) {
                for (uint8_t r = 0; r < _game->totemRequirementCount; r++) {
                    const LightAir_TotemRequirement& req = _game->totemRequirements[r];
                    if (req.roleId == roleId && req.configSecs != nullptr) {
                        buf[1] = (uint8_t)*req.configSecs;
                        bufLen = 2;
                        break;
                    }
                }
            }
            output.radio.replyWithPayload(ev.packet, buf, bufLen);
            break;
        }
    }

    // Step 2b: DirectRadioRules — handle all incoming MessageReceived events.
    // Events intercepted above (MSG_END_GAME, MSG_TOTEM_BEACON) are skipped.
    for (uint8_t e = 0; e < radio.count; e++) {
        const RadioEvent& ev = radio.events[e];
        if (ev.type != RadioEventType::MessageReceived) continue;
        if (infraHandled[e]) continue;

        bool matched = false;
        for (uint8_t i = 0; i < _game->directRadioRuleCount; i++) {
            const DirectRadioRule& r = _game->directRadioRules[i];
            if (r.fromState != *_game->currentState) continue;
            if (r.msgType   != ev.packet.msgType)    continue;
            if (r.condition && !r.condition(ev.packet)) continue;

            if (r.onReceive) r.onReceive(ev.packet, *_display, output);
            output.radio.reply(ev.packet, r.replySubType);
            matched = true;
            break;
        }
        if (!matched)
            output.radio.reply(ev.packet);  // standard empty reply — prevents sender timeout
    }

    // Step 2c: ReplyRadioRules — handle all ReplyReceived and Timeout events.
    for (uint8_t e = 0; e < radio.count; e++) {
        const RadioEvent& ev = radio.events[e];
        if (ev.type != RadioEventType::ReplyReceived &&
            ev.type != RadioEventType::Timeout) continue;

        uint8_t state = *_game->currentState;
        for (uint8_t i = 0; i < _game->replyRadioRuleCount; i++) {
            const ReplyRadioRule& r = _game->replyRadioRules[i];
            if (!(r.activeInStateMask & (1u << state))) continue;
            if (r.eventType != ev.type) continue;
            if (ev.type == RadioEventType::ReplyReceived &&
                r.replySubType != 0 &&
                (ev.packet.payloadLen == 0 || ev.packet.payload[0] != r.replySubType)) continue;
            if (r.condition && !r.condition(ev.packet, ev.original)) continue;

            if (r.onReply) r.onReply(ev.packet, ev.original, *_display, output);
            break;
        }
    }

    // Step 2d: StateRules — evaluate transitions (first match wins).
    for (uint8_t i = 0; i < _game->ruleCount; i++) {
        const StateRule& r = _game->rules[i];
        if (r.fromState != *_game->currentState) continue;
        if (r.condition && !r.condition(inputs, radio)) continue;

        *_game->currentState = r.toState;
        activateStateDisplay(r.toState);
        if (r.onTransition) r.onTransition(*_display, output);
        break;
    }

    // After Step 2d: detect scoringState entry and kick off score collection.
    if (scoringEnabled) {
        uint8_t cur = *_game->currentState;
        if (cur == _game->scoringState && !_scoreActive) {
            _scoreActive      = true;
            _scoreResultShown = false;
            _scoreAccumMask   = 0;
            _scoreSentAt      = 0;
            memset(_scoreSlots, 0, sizeof(_scoreSlots));

            // Record own scores immediately.
            uint8_t ownIdx = _rosterCount;  // sentinel: not found
            uint8_t myId   = _radio->playerId();
            for (uint8_t r = 0; r < _rosterCount; r++) {
                if (_roster[r] == myId) { ownIdx = r; break; }
            }
            if (ownIdx < _rosterCount) {
                scoreFillSlot(_scoreSlots[ownIdx]);
                _scoreAccumMask |= (1u << ownIdx);
            }

            // Flood MSG_END_GAME so devices still in a non-scoring state transition.
            output.radio.broadcast(GameDefaults::MSG_END_GAME, nullptr, 0, 2);
            scoreBroadcastFused(output);
            _scoreSentAt = millis();

            if (_scoreAccumMask == expectedMask) {
                scoreAnnounce();
                _scoreResultShown = true;
                postScoreAnnounce();
            }
        } else if (cur != _game->scoringState) {
            _scoreActive = false;
        }
    }

    // Step 2e: StateBehavior — per-state continuous logic.
    for (uint8_t i = 0; i < _game->behaviorCount; i++) {
        if (_game->behaviors[i].state != *_game->currentState) continue;
        if (_game->behaviors[i].onUpdate)
            _game->behaviors[i].onUpdate(inputs, radio, *_display, output);
        break;
    }

    // ---- Step 3: OUTPUT ----
    _display->update();
    flushOutput(output);

    // Enforce fixed loop duration.
    while ((millis() - loopStart) < GameDefaults::LOOP_MS) {}
}

/* =========================================================
 *   SCORE UPDATE — ongoing scoring phase (runs when _scoreActive)
 * ========================================================= */

void LightAir_GameRunner::scoreUpdate(const InputReport& inputs,
                                       const RadioReport& radio,
                                       GameOutput& output) {
    const bool     scoringEnabled = _game->winnerVars &&
                                    _game->winnerVarCount > 0 &&
                                    _game->scoringState != 255 &&
                                    _rosterCount > 0;
    const uint8_t  slotSize       = scoringEnabled ? _game->winnerVarCount * 4 : 0;
    const uint32_t expectedMask   = _rosterCount < 32
                                    ? (1u << _rosterCount) - 1u
                                    : 0xFFFFFFFFu;

    // Accumulate per-player score messages.
    if (scoringEnabled) {
        for (uint8_t e = 0; e < radio.count; e++) {
            const RadioEvent& ev = radio.events[e];
            if (ev.type != RadioEventType::MessageReceived) continue;
            if (ev.packet.msgType != _game->scoreMsgType)  continue;

            uint8_t expectedLen = 4 + _rosterCount * slotSize;
            if (ev.packet.payloadLen < expectedLen) continue;

            uint32_t recvMask = 0;
            memcpy(&recvMask, ev.packet.payload, 4);
            uint32_t newBits = recvMask & ~_scoreAccumMask;

            if (newBits) {
                for (uint8_t r = 0; r < _rosterCount; r++) {
                    if (!(newBits & (1u << r))) continue;
                    memcpy(_scoreSlots[r],
                           ev.packet.payload + 4 + r * slotSize,
                           slotSize);
                }
                _scoreAccumMask |= newBits;
                scoreBroadcastFused(output);
                _scoreSentAt = millis();
            }

            if (_scoreAccumMask == expectedMask && !_scoreResultShown) {
                scoreAnnounce();
                _scoreResultShown = true;
                postScoreAnnounce();
            }
        }
    }

    // MSG_TOTEM_BEACON: no DirectRadioRules run in scoring phase; reply directly.
    for (uint8_t e = 0; e < radio.count; e++) {
        const RadioEvent& ev = radio.events[e];
        if (ev.type           != RadioEventType::MessageReceived) continue;
        if (ev.packet.msgType != RadioMsg::MSG_TOTEM_BEACON)      continue;

        uint8_t id = ev.packet.senderId;
        if (!TotemDefs::isTotemId(id)) continue;

        for (uint8_t t = 0; t < _totemCount; t++) {
            if (_totems[t].id != id) continue;
            uint8_t roleId = _totems[t].roleId;
            uint8_t buf[2] = { roleId, 0 };
            uint8_t bufLen = 1;
            if (_game->totemRequirements) {
                for (uint8_t r = 0; r < _game->totemRequirementCount; r++) {
                    const LightAir_TotemRequirement& req = _game->totemRequirements[r];
                    if (req.roleId == roleId && req.configSecs != nullptr) {
                        buf[1] = (uint8_t)*req.configSecs;
                        bufLen = 2;
                        break;
                    }
                }
            }
            output.radio.replyWithPayload(ev.packet, buf, bufLen);
            break;
        }
    }

    // Timed retry — re-broadcast fused scores while waiting for all devices.
    if (scoringEnabled && !_scoreResultShown &&
        _scoreSentAt != 0 &&
        millis() - _scoreSentAt >= GameDefaults::SCORE_RETRY_MS) {
        scoreBroadcastFused(output);
        _scoreSentAt = millis();
    }

    // A+B chord on end-game screen triggers reboot.
    if (_endExitReady) {
        bool aDown = false, bDown = false;
        for (uint8_t i = 0; i < inputs.keyEventCount; i++) {
            const InputReport::KeyEntry& ke = inputs.keyEvents[i];
            if (ke.state == KeyState::PRESSED || ke.state == KeyState::HELD) {
                if (ke.key == 'A') aDown = true;
                if (ke.key == 'B') bDown = true;
            }
        }
        if (aDown && bDown) {
            if (_game->onEnd) _game->onEnd(*_display);
            _display->update();
            esp_restart();
        }
    }
}

/* =========================================================
 *   SCORE COLLECTION HELPERS
 * ========================================================= */

// Fill buf with winnerVarCount × int32_t LE from winnerVars[v].value.
void LightAir_GameRunner::scoreFillSlot(uint8_t* buf) const {
    for (uint8_t v = 0; v < _game->winnerVarCount; v++) {
        int32_t val = (int32_t)*_game->winnerVars[v].value;
        memcpy(buf + v * 4, &val, 4);
    }
}

// Return true if slot a strictly beats slot b under winnerVars priority + direction.
bool LightAir_GameRunner::scoreSlotBeats(const uint8_t* a, const uint8_t* b) const {
    for (uint8_t v = 0; v < _game->winnerVarCount; v++) {
        int32_t va, vb;
        memcpy(&va, a + v * 4, 4);
        memcpy(&vb, b + v * 4, 4);
        if (_game->winnerVars[v].dir == WinnerDir::MAX) {
            if (va > vb) return true;
            if (va < vb) return false;
        } else {
            if (va < vb) return true;
            if (va > vb) return false;
        }
    }
    return false;  // all equal — not a strict win
}

// Return true if all winnerVar values are identical between a and b.
bool LightAir_GameRunner::scoreSlotsEqual(const uint8_t* a, const uint8_t* b) const {
    return memcmp(a, b, _game->winnerVarCount * 4) == 0;
}

// Build fused payload (mask + all slots) and queue it as a broadcast.
void LightAir_GameRunner::scoreBroadcastFused(GameOutput& output) const {
    uint8_t slotSize = _game->winnerVarCount * 4;
    uint8_t totalLen = 4 + _rosterCount * slotSize;
    if (totalLen > GameDefaults::RADIO_OUT_PAYLOAD) return;  // payload too large — skip

    uint8_t buf[GameDefaults::RADIO_OUT_PAYLOAD] = {};
    memcpy(buf, &_scoreAccumMask, 4);
    for (uint8_t r = 0; r < _rosterCount; r++) {
        if (_scoreAccumMask & (1u << r))
            memcpy(buf + 4 + r * slotSize, _scoreSlots[r], slotSize);
    }
    output.radio.broadcast(_game->scoreMsgType, buf, totalLen);
}

// Find the winner from accumulated slots and call showMessage() on the display.
// If the game provides onScoreAnnounce, delegates entirely to that callback
// (used for team-aggregate or other non-individual winner logic).
// Otherwise shows two tray lines:
//   top    — "[NAME] WINS!"  or  "TIE: [NAMES]"
//   bottom — "You arrived Xth"  (only if own ID is in the roster)
void LightAir_GameRunner::scoreAnnounce() const {
    // Delegate to game-specific announce if provided.
    if (_game->onScoreAnnounce) {
        ScoreTable table;
        table.rosterCount   = _rosterCount;
        table.roster        = _roster;
        table.accumMask     = _scoreAccumMask;
        table.slots         = _scoreSlots;
        table.winnerVarCount = _game->winnerVarCount;
        table.winnerVars    = _game->winnerVars;
        table.teamMap       = _teamMap;
        table.myPlayerId    = _radio->playerId();
        _game->onScoreAnnounce(table, *_display);
        return;
    }

    // Default: individual-player ranking.
    uint8_t bestIdx = 0;
    bool    tied    = false;

    for (uint8_t r = 1; r < _rosterCount; r++) {
        if (!(_scoreAccumMask & (1u << r))) continue;
        const uint8_t* slotR    = _scoreSlots[r];
        const uint8_t* slotBest = _scoreSlots[bestIdx];
        if (scoreSlotBeats(slotR, slotBest)) {
            bestIdx = r;
            tied    = false;
        } else if (scoreSlotsEqual(slotR, slotBest)) {
            tied = true;
        }
    }

    // --- Winner / tie message ---
    char msg[32];
    if (!tied) {
        snprintf(msg, sizeof(msg), "%s WINS!",
                 PlayerDefs::playerShort[_roster[bestIdx]]);
    } else {
        // Collect tied participant short-names, space-separated.
        const uint8_t* slotBest = _scoreSlots[bestIdx];
        char    names[24] = {};
        uint8_t off       = 0;
        for (uint8_t r = 0; r < _rosterCount && off < 20; r++) {
            if (!(_scoreAccumMask & (1u << r))) continue;
            if (scoreSlotsEqual(_scoreSlots[r], slotBest)) {
                if (off) names[off++] = ' ';
                memcpy(names + off, PlayerDefs::playerShort[_roster[r]], 3);
                off += 3;
            }
        }
        snprintf(msg, sizeof(msg), "TIE: %s", names);
    }

    // --- Own position (shown below winner; tray pushes newest msg to top) ---
    // Show position first so it ends up on the bottom row when winner is pushed on top.
    uint8_t myId   = _radio->playerId();
    uint8_t mySlot = 0xFF;
    for (uint8_t r = 0; r < _rosterCount; r++)
        if (_roster[r] == myId) { mySlot = r; break; }

    if (mySlot != 0xFF && (_scoreAccumMask & (1u << mySlot))) {
        uint8_t rank = 1;
        for (uint8_t r = 0; r < _rosterCount; r++) {
            if (r == mySlot) continue;
            if (!(_scoreAccumMask & (1u << r))) continue;
            if (scoreSlotBeats(_scoreSlots[r], _scoreSlots[mySlot])) rank++;
        }
        const char* sfx = (rank == 1) ? "st" : (rank == 2) ? "nd"
                        : (rank == 3) ? "rd" : "th";
        char pos[24];
        snprintf(pos, sizeof(pos), "You arrived %u%s", rank, sfx);
        _display->showMessage(pos, 0);   // → bottom row after winner pushed on top
    }

    _display->showMessage(msg, 0);  // → top row
}

// Arm the A+B exit after winner announcement.
// The GAME_END binding set remains active so monitor vars stay visible
// alongside the tray messages produced by scoreAnnounce().
void LightAir_GameRunner::postScoreAnnounce() {
    _display->showMessage("A+B: Restart");
    _endExitReady = true;
}

/* =========================================================
 *   HELPERS
 * ========================================================= */

void LightAir_GameRunner::activateStateDisplay(uint8_t state) {
    for (uint8_t b = 0; b < _bindingCount; b++) {
        if (_bindings[b].state == state) {
            _display->activateBindingSet(_bindings[b].setId);
            return;
        }
    }
    // No binding set for this state — display left unchanged.
}

void LightAir_GameRunner::flushOutput(const GameOutput& out) {
    // Radio messages
    for (uint8_t i = 0; i < out.radio.count; i++) {
        const RadioOutMsg& m = out.radio.msgs[i];
        if (m.isBroadcast)
            _radio->broadcast(m.msgType, m.payload, m.payloadLen, m.resend);
        else
            _radio->sendTo(m.targetId, m.msgType, m.payload, m.payloadLen, m.resend);
    }

    // Radio replies
    for (uint8_t i = 0; i < out.radio.replyCount; i++) {
        const RadioReplyMsg& r = out.radio.replies[i];
        if (r.payloadLen)
            _radio->replyTo(r.senderId, r.origMsgType, r.origTimestamp, r.payload, r.payloadLen);
        else
            _radio->replyTo(r.senderId, r.origMsgType, r.origTimestamp);
    }

    // UI events (skipped if no UICtrl was provided)
    if (!_ui) return;
    for (uint8_t i = 0; i < out.ui.count; i++) {
        const UIOutMsg& m = out.ui.msgs[i];
        if (m.event == LightAir_UICtrl::UIEvent::Enlight)
            _ui->triggerEnlight(m.enlightMs);
        else
            _ui->trigger(m.event);
    }
}
