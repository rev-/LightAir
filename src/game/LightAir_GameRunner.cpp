#include "LightAir_GameRunner.h"
#include <Arduino.h>

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
            if (var.type == VarType::INT)
                display.bindIntVariable(var.asInt, var.icon, var.col, var.row);
            else
                display.bindStringVariable(var.asChars, var.icon, var.col, var.row);
        }
    }

    // Reset state and activate the initial binding set.
    *game.currentState = game.initialState;
    activateStateDisplay(game.initialState);

    // User-provided setup (radio init, opening messages, etc.)
    if (game.onBegin) game.onBegin(display, radio);
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

    // Step 2a: DirectRadioRules — handle all incoming MessageReceived events.
    // First matching rule fires per event; unmatched events get a standard empty reply.
    for (uint8_t e = 0; e < radio.count; e++) {
        const RadioEvent& ev = radio.events[e];
        if (ev.type != RadioEventType::MessageReceived) continue;

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

    // Step 2b: ReplyRadioRules — handle all ReplyReceived and Timeout events.
    // First matching rule fires per event.
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

    // Step 2c: StateRules — evaluate transitions (first match wins).
    // Sees game state already modified by both radio rule tables above.
    for (uint8_t i = 0; i < _game->ruleCount; i++) {
        const StateRule& r = _game->rules[i];
        if (r.fromState != *_game->currentState) continue;
        if (r.condition && !r.condition(inputs, radio)) continue;

        *_game->currentState = r.toState;
        activateStateDisplay(r.toState);
        if (r.onTransition) r.onTransition(*_display, output);
        break;
    }

    // Step 2d: StateBehavior — per-state continuous logic.
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
        if (r.subType)
            _radio->replyTo(r.senderId, r.origMsgType, r.origTimestamp, &r.subType, 1);
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
