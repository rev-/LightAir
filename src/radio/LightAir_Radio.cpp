#include "LightAir_Radio.h"
#include "esp_log.h"
#include <string.h>
#include <stddef.h>

static const char* TAG = "LightAirRadio";

static const uint8_t kBroadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static const uint8_t kMacPrefix[5]    = {0x1A, 0x17, 0xA1, 0x00, 0x00};

// ----------------------------------------------------------------
LightAir_Radio::LightAir_Radio(LightAir_RadioTransport& transport,
                                uint8_t playerId, uint8_t sessionToken,
                                uint8_t role, uint8_t team,
                                const RadioConfig& cfg)
    : _transport(transport),
      _playerId(playerId), _sessionToken(sessionToken),
      _role(role), _team(team), _config(cfg),
      _dedupIdx(0)
{
    memset(_pending, 0, sizeof(_pending));
    memset(_dedup,   0, sizeof(_dedup));
}

// ----------------------------------------------------------------
void LightAir_Radio::buildMac(uint8_t playerId, uint8_t mac[6]) const {
    memcpy(mac, kMacPrefix, 5);
    mac[5] = playerId;
}

bool LightAir_Radio::begin() {
    uint8_t mac[6];
    buildMac(_playerId, mac);

    if (!_transport.begin(mac)) {
        ESP_LOGE(TAG, "Transport begin() failed");
        return false;
    }

    ESP_LOGI(TAG, "begin() OK  id=%u token=%u role=%u team=%u",
             _playerId, _sessionToken, _role, _team);
    return true;
}

bool LightAir_Radio::sendRaw(const uint8_t mac[6], const RadioPacket& pkt) {
    size_t wireLen = offsetof(RadioPacket, payload) + pkt.payloadLen;
    if (!_transport.send(mac, (const uint8_t*)&pkt, wireLen)) {
        ESP_LOGW(TAG, "transport.send failed");
        return false;
    }
    return true;
}

// ----------------------------------------------------------------
bool LightAir_Radio::storePending(const RadioPacket& pkt) {
    if (pkt.msgType & 1) return true;  // odd = reply; no ACK expected
    for (int i = 0; i < RADIO_MAX_PENDING; i++) {
        if (!_pending[i].active) {
            _pending[i].pkt    = pkt;
            _pending[i].sentAt = millis();
            _pending[i].active = true;
            return true;
        }
    }
    ESP_LOGW(TAG, "Pending queue full");
    return false;
}

int LightAir_Radio::findPending(uint8_t replyMsgType, uint32_t timestamp) const {
    if (!(replyMsgType & 1)) return -1;
    uint8_t origType = replyMsgType - 1;
    for (int i = 0; i < RADIO_MAX_PENDING; i++) {
        if (_pending[i].active &&
            _pending[i].pkt.msgType   == origType &&
            _pending[i].pkt.timestamp == timestamp)
            return i;
    }
    return -1;
}

bool LightAir_Radio::isDuplicate(uint8_t senderId, uint32_t timestamp) const {
    for (int i = 0; i < RADIO_DEDUP_WINDOW; i++)
        if (_dedup[i].valid &&
            _dedup[i].senderId  == senderId &&
            _dedup[i].timestamp == timestamp)
            return true;
    return false;
}

void LightAir_Radio::recordDedup(uint8_t senderId, uint32_t timestamp) {
    _dedup[_dedupIdx].senderId  = senderId;
    _dedup[_dedupIdx].timestamp = timestamp;
    _dedup[_dedupIdx].valid     = true;
    _dedupIdx = (_dedupIdx + 1) % RADIO_DEDUP_WINDOW;
}

// ----------------------------------------------------------------
// Public send API
// ----------------------------------------------------------------
bool LightAir_Radio::sendTo(uint8_t targetId, uint8_t msgType,
                             const uint8_t* payload, uint8_t payloadLen,
                             uint8_t resend) {
    uint8_t mac[6];
    buildMac(targetId, mac);

    RadioPacket pkt = {};
    pkt.senderId     = _playerId;
    pkt.role         = _role;
    pkt.team         = _team;
    pkt.msgType      = msgType;
    pkt.sessionToken = _sessionToken;
    pkt.timestamp    = millis();
    pkt.resend       = resend;
    pkt.payloadLen   = payloadLen;
    if (payload && payloadLen)
        memcpy(pkt.payload, payload, payloadLen);

    if (!sendRaw(mac, pkt)) return false;
    storePending(pkt);
    return true;
}

bool LightAir_Radio::broadcast(uint8_t msgType,
                                const uint8_t* payload, uint8_t payloadLen,
                                uint8_t resend) {
    RadioPacket pkt = {};
    pkt.senderId     = _playerId;
    pkt.role         = _role;
    pkt.team         = _team;
    pkt.msgType      = msgType;
    pkt.sessionToken = _sessionToken;
    pkt.timestamp    = millis();
    pkt.resend       = resend;
    pkt.payloadLen   = payloadLen;
    if (payload && payloadLen)
        memcpy(pkt.payload, payload, payloadLen);

    if (!sendRaw(kBroadcastMac, pkt)) return false;
    storePending(pkt);
    return true;
}

bool LightAir_Radio::reply(const RadioPacket& original,
                            const uint8_t* payload, uint8_t payloadLen) {
    uint8_t mac[6];
    buildMac(original.senderId, mac);

    RadioPacket pkt = {};
    pkt.senderId     = _playerId;
    pkt.role         = _role;
    pkt.team         = _team;
    pkt.msgType      = original.msgType + 1;  // odd = reply
    pkt.sessionToken = _sessionToken;
    pkt.timestamp    = original.timestamp;    // echoed for sender-side matching
    pkt.resend       = 0;                     // replies never flood
    pkt.payloadLen   = payloadLen;
    if (payload && payloadLen)
        memcpy(pkt.payload, payload, payloadLen);

    return sendRaw(mac, pkt);
    // replies not stored in pending (odd msgType → no ACK expected)
}

bool LightAir_Radio::replyTo(uint8_t senderId, uint8_t origMsgType, uint32_t origTimestamp,
                              const uint8_t* payload, uint8_t payloadLen) {
    uint8_t mac[6];
    buildMac(senderId, mac);

    RadioPacket pkt = {};
    pkt.senderId     = _playerId;
    pkt.role         = _role;
    pkt.team         = _team;
    pkt.msgType      = origMsgType + 1;   // odd = reply
    pkt.sessionToken = _sessionToken;
    pkt.timestamp    = origTimestamp;     // echoed for sender-side matching
    pkt.resend       = 0;
    pkt.payloadLen   = payloadLen;
    if (payload && payloadLen)
        memcpy(pkt.payload, payload, payloadLen);

    return sendRaw(mac, pkt);
}

// ----------------------------------------------------------------
// sendConfig — chunk and send a pre-session config blob
// ----------------------------------------------------------------
bool LightAir_Radio::sendConfig(const uint8_t* data, uint16_t totalLen, uint8_t targetId) {
    if (totalLen == 0) return true;

    uint8_t totalChunks = (uint8_t)((totalLen + RADIO_MAX_PAYLOAD - 1) / RADIO_MAX_PAYLOAD);

    uint8_t mac[6];
    if (targetId == 0xFF)
        memcpy(mac, kBroadcastMac, 6);
    else
        buildMac(targetId, mac);

    for (uint8_t i = 0; i < totalChunks; i++) {
        uint16_t offset   = (uint16_t)i * RADIO_MAX_PAYLOAD;
        uint8_t  chunkLen = ((uint16_t)(totalLen - offset) > RADIO_MAX_PAYLOAD)
                          ? RADIO_MAX_PAYLOAD
                          : (uint8_t)(totalLen - offset);

        RadioPacket pkt  = {};
        pkt.senderId     = _playerId;
        pkt.role         = i;                  // chunk index (0-based)
        pkt.team         = totalChunks;        // total chunk count
        pkt.msgType      = RadioRole::CONFIG;  // 0xFF sentinel in msgType field
        pkt.sessionToken = RadioToken::UNSET;
        pkt.timestamp    = millis();
        pkt.resend       = 0;
        pkt.payloadLen   = chunkLen;
        memcpy(pkt.payload, data + offset, chunkLen);

        if (!sendRaw(mac, pkt)) return false;
    }
    return true;
}

// ----------------------------------------------------------------
// processPacket() — classify one received packet; append to _report
// ----------------------------------------------------------------
void LightAir_Radio::processPacket(const RadioPacket& pkt, int8_t rssi) {
    // 1. Session token gate
    bool isConfig = (pkt.sessionToken == RadioToken::UNSET &&
                     _sessionToken    == RadioToken::UNSET);
    if (!isConfig && pkt.sessionToken != _sessionToken) return;

    // 2. Flood relay: re-broadcast if not seen before
    if (pkt.resend > 0 && !isDuplicate(pkt.senderId, pkt.timestamp)) {
        recordDedup(pkt.senderId, pkt.timestamp);
        RadioPacket relay = pkt;
        relay.resend--;
        sendRaw(kBroadcastMac, relay);
    }

    // 3. Ignore our own packets echoed back via broadcast relay
    if (pkt.senderId == _playerId) return;

    if (_report.count >= RADIO_MAX_PENDING) return;  // report buffer full

    // 4. Odd msgType = reply: try to match a pending request
    if (pkt.msgType & 1) {
        int idx = findPending(pkt.msgType, pkt.timestamp);
        if (idx < 0) return;  // reply for someone else (relayed) — ignore
        RadioEvent& evt      = _report.events[_report.count++];
        evt.type             = RadioEventType::ReplyReceived;
        evt.packet           = pkt;
        evt.original         = _pending[idx].pkt;
        evt.rssi             = rssi;
        _pending[idx].active = false;
        return;
    }

    // 5. Even msgType = incoming request from another node
    RadioEvent& evt = _report.events[_report.count++];
    evt.type        = RadioEventType::MessageReceived;
    evt.packet      = pkt;
    evt.rssi        = rssi;
}

// ----------------------------------------------------------------
// poll() — call once per loop iteration
// ----------------------------------------------------------------
const RadioReport& LightAir_Radio::poll() {
    _report.count = 0;

    // Drain all received packets from the transport queue
    uint8_t rawBuf[sizeof(RadioPacket)];
    int     rawLen = 0;
    int8_t  rawRssi = 0;
    while (_transport.receive(rawBuf, rawLen, sizeof(RadioPacket), rawRssi)) {
        const int kMinLen = (int)offsetof(RadioPacket, payload);
        if (rawLen < kMinLen) continue;  // malformed — skip

        RadioPacket pkt = {};
        memcpy(&pkt, rawBuf, rawLen);
        processPacket(pkt, rawRssi);
    }

    // Check all pending messages for timeout
    uint32_t now = millis();
    for (int i = 0; i < RADIO_MAX_PENDING; i++) {
        if (!_pending[i].active) continue;
        if ((now - _pending[i].sentAt) < _config.replyTimeoutMs) continue;
        if (_report.count >= RADIO_MAX_PENDING) break;  // report buffer full

        RadioEvent& evt    = _report.events[_report.count++];
        evt.type           = RadioEventType::Timeout;
        evt.original       = _pending[i].pkt;
        _pending[i].active = false;
    }

    return _report;
}
