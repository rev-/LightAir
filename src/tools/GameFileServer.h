#pragma once
#include <stdint.h>
#include <stddef.h>
#include "../config.h"

#ifdef ESP32
#include <WebServer.h>
#endif

// ----------------------------------------------------------------
// GameFileServer — Settings → "Share games".
//
// Opens a WiFi access point (SSID "LightAir-<PLAYERSHORT>", password
// ShareDefaults::AP_PASSWORD) and serves a single web page on
// http://192.168.4.1/ from which any phone or laptop can:
//
//   - DOWNLOAD every .lua game (and /games/lib helper) currently on
//     the device, as plain .lua file attachments;
//   - UPLOAD new or updated .lua files into /games (or /games/lib),
//     which is how games travel from device to device: download from
//     one, upload to the other;
//   - DELETE custom games (stock games reappear at next boot via the
//     embedded-bundle seeding, so deleting them is harmless).
//
// Usage (from LightAir_GameSetupMenu::runShareTool):
//   start() → loop { handleClient(); poll exit key } → device reboot.
//
// A reboot on exit is deliberate: SoftAP mode and the ESP-NOW radio
// share the WiFi peripheral, and the game list may have changed —
// rebooting restores the radio and rescans /games in one stroke.
// ----------------------------------------------------------------
class GameFileServer {
public:
    // Bring up the SoftAP + HTTP server.  Returns false if the AP
    // could not start (server not running).
    bool start();

    // Pump one round of HTTP handling; call continuously while the
    // share screen is open.
    void handleClient();

    // Tear down the HTTP server and the AP.
    void stop();

    bool        running()   const { return _running; }
    const char* ssid()      const { return _ssid; }
    const char* password()  const { return ShareDefaults::AP_PASSWORD; }
    const char* ipAddress() const { return _ip; }

    // Connected WiFi stations (shown on the LCD so the operator can
    // tell whether the phone has actually joined).
    uint8_t stationCount() const;

private:
    bool _running = false;
    char _ssid[20] = {0};
    char _ip[16]   = {0};

#ifdef ESP32
    WebServer _http{ShareDefaults::HTTP_PORT};

    void sendIndex();
    void sendDownload();
    void handleDelete();
    void handleUploadData();   // multipart chunks
    void handleUploadDone();
#endif

    // True when name is a safe bare .lua filename (no path separators,
    // charset [A-Za-z0-9_.-], reasonable length).
    static bool safeLuaName(const char* name);
};
