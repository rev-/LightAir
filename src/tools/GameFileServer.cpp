#include "GameFileServer.h"
#include <string.h>

bool GameFileServer::safeLuaName(const char* name) {
    if (!name) return false;
    size_t len = strlen(name);
    if (len < 5 || len > 40) return false;               // "x.lua" .. path budget
    if (strcmp(name + len - 4, ".lua") != 0) return false;
    if (name[0] == '.') return false;
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        if (!ok) return false;
    }
    return true;
}

#ifdef ESP32

#include <Arduino.h>
#include <ArduinoLog.h>
#include <WiFi.h>
#include <FS.h>
#include <LittleFS.h>
#include "../nvs_config.h"

// Upload-in-progress state (one server, one upload at a time).
static File   s_upFile;
static size_t s_upWritten  = 0;
static bool   s_upRejected = false;
static char   s_note[64]   = {0};   // last action's outcome, shown on the page

/* =========================================================
 *   LIFECYCLE
 * ========================================================= */

bool GameFileServer::start() {
    if (_running) return true;

    // SSID = prefix + this device's short player label, so several
    // devices sharing at once stay distinguishable.
    PlayerConfig cfg;
    player_config_load(cfg);
    const char* shortName = (cfg.id < PlayerDefs::MAX_PLAYER_ID)
                                ? PlayerDefs::playerShort[cfg.id] : "NON";
    snprintf(_ssid, sizeof(_ssid), "%s%s", ShareDefaults::AP_SSID_PREFIX, shortName);

    WiFi.mode(WIFI_AP);
    if (!WiFi.softAP(_ssid, ShareDefaults::AP_PASSWORD)) {
        Log.errorln("Share: softAP start failed");
        WiFi.mode(WIFI_OFF);
        return false;
    }
    snprintf(_ip, sizeof(_ip), "%s", WiFi.softAPIP().toString().c_str());

    s_note[0] = 0;
    _http.on("/",   HTTP_GET,  [this]() { sendIndex(); });
    _http.on("/dl", HTTP_GET,  [this]() { sendDownload(); });
    _http.on("/rm", HTTP_POST, [this]() { handleDelete(); });
    _http.on("/upload", HTTP_POST,
             [this]() { handleUploadDone(); },
             [this]() { handleUploadData(); });
    _http.onNotFound([this]() { _http.send(404, "text/plain", "Not found"); });
    _http.begin();

    _running = true;
    Log.infoln("Share: AP \"%s\" up, http://%s/", _ssid, _ip);
    return true;
}

void GameFileServer::handleClient() {
    if (_running) _http.handleClient();
}

void GameFileServer::stop() {
    if (!_running) return;
    _http.stop();
    if (s_upFile) s_upFile.close();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    _running = false;
}

uint8_t GameFileServer::stationCount() const {
    return _running ? WiFi.softAPgetStationNum() : 0;
}

/* =========================================================
 *   HELPERS
 * ========================================================= */

// The only directory this server ever touches — /games/stock and
// /games/lib are firmware territory and unreachable from here.
static const char* dirPath() { return LuaDefaults::CUSTOM_DIR; }

static void appendFileRows(String& html) {
    File d = LittleFS.open(dirPath());
    if (!d || !d.isDirectory()) return;
    for (File f = d.openNextFile(); f; f = d.openNextFile()) {
        if (f.isDirectory()) continue;
        const char* name = f.name();
        size_t len = strlen(name);
        if (len < 5 || strcmp(name + len - 4, ".lua") != 0) continue;
        html += "<tr><td>";
        html += name;
        html += "</td><td>";
        html += (unsigned)f.size();
        html += " B</td><td><a href=\"/dl?f=";
        html += name;
        html += "\">download</a></td><td><form method=\"post\" action=\"/rm\" "
                "onsubmit=\"return confirm('Delete ";
        html += name;
        html += "?')\"><input type=\"hidden\" name=\"f\" value=\"";
        html += name;
        html += "\"><button>delete</button></form></td></tr>";
        f.close();
    }
}

/* =========================================================
 *   ROUTES
 * ========================================================= */

void GameFileServer::sendIndex() {
    String html;
    html.reserve(3072);
    html += "<!DOCTYPE html><html><head><meta name=\"viewport\" "
            "content=\"width=device-width,initial-scale=1\">"
            "<title>LightAir games</title><style>"
            "body{font-family:sans-serif;margin:1em;max-width:40em}"
            "table{border-collapse:collapse;width:100%}"
            "td,th{padding:.3em .5em;border-bottom:1px solid #ccc;text-align:left}"
            "form{display:inline}h2{margin-top:1.2em}"
            ".note{background:#eef;padding:.5em;border-radius:.3em}"
            "</style></head><body><h1>LightAir — ";
    html += _ssid;
    html += "</h1>";
    if (s_note[0]) {
        html += "<p class=\"note\">";
        html += s_note;
        html += "</p>";
        s_note[0] = 0;
    }
    html += "<p>Download a custom game from this device, or upload one to "
            "it. Stock games aren't shown here: they're managed by the "
            "firmware and reset to the shipped version on every boot.</p>";

    html += "<h2>Custom games</h2><table>";
    appendFileRows(html);
    html += "</table>";

    html += "<h2>Upload</h2><form method=\"post\" action=\"/upload\" "
            "enctype=\"multipart/form-data\">"
            "<input type=\"file\" name=\"file\" accept=\".lua\" required> "
            "<button>upload</button></form>";

    char foot[96];
    snprintf(foot, sizeof(foot),
             "<p>%u KB free of %u KB.</p></body></html>",
             (unsigned)((LittleFS.totalBytes() - LittleFS.usedBytes()) / 1024),
             (unsigned)(LittleFS.totalBytes() / 1024));
    html += foot;

    _http.send(200, "text/html", html);
}

void GameFileServer::sendDownload() {
    String fn  = _http.arg("f");
    if (!safeLuaName(fn.c_str())) {
        _http.send(400, "text/plain", "Bad file name");
        return;
    }
    char path[64];
    snprintf(path, sizeof(path), "%s/%s", dirPath(), fn.c_str());
    File f = LittleFS.open(path, "r");
    if (!f) {
        _http.send(404, "text/plain", "No such file");
        return;
    }
    _http.sendHeader("Content-Disposition",
                     String("attachment; filename=\"") + fn + "\"");
    // text/plain (not octet-stream) so a phone browser can also preview it.
    _http.streamFile(f, "text/plain");
    f.close();
}

void GameFileServer::handleDelete() {
    String fn  = _http.arg("f");
    if (!safeLuaName(fn.c_str())) {
        _http.send(400, "text/plain", "Bad file name");
        return;
    }
    char path[64];
    snprintf(path, sizeof(path), "%s/%s", dirPath(), fn.c_str());
    if (LittleFS.remove(path))
        snprintf(s_note, sizeof(s_note), "Deleted %s", fn.c_str());
    else
        snprintf(s_note, sizeof(s_note), "Could not delete %s", fn.c_str());
    Log.infoln("Share: %s", s_note);
    _http.sendHeader("Location", "/");
    _http.send(303);
}

void GameFileServer::handleUploadData() {
    HTTPUpload& up = _http.upload();

    if (up.status == UPLOAD_FILE_START) {
        s_upWritten  = 0;
        s_upRejected = false;

        if (!safeLuaName(up.filename.c_str())) {
            s_upRejected = true;
            snprintf(s_note, sizeof(s_note), "Rejected: not a .lua file");
            return;
        }
        char path[64];
        snprintf(path, sizeof(path), "%s/%s", dirPath(), up.filename.c_str());
        s_upFile = LittleFS.open(path, "w");
        if (!s_upFile) {
            s_upRejected = true;
            snprintf(s_note, sizeof(s_note), "Rejected: cannot write file");
        }

    } else if (up.status == UPLOAD_FILE_WRITE) {
        if (s_upRejected || !s_upFile) return;
        if (s_upWritten + up.currentSize > ShareDefaults::MAX_UPLOAD) {
            s_upRejected = true;
            s_upFile.close();
            snprintf(s_note, sizeof(s_note), "Rejected: file too large");
            return;
        }
        if (s_upFile.write(up.buf, up.currentSize) != up.currentSize) {
            s_upRejected = true;
            s_upFile.close();
            snprintf(s_note, sizeof(s_note), "Rejected: flash full");
            return;
        }
        s_upWritten += up.currentSize;

    } else if (up.status == UPLOAD_FILE_END) {
        if (s_upFile) s_upFile.close();
        if (!s_upRejected)
            snprintf(s_note, sizeof(s_note), "Uploaded %s (%u bytes)",
                     up.filename.c_str(), (unsigned)s_upWritten);

    } else if (up.status == UPLOAD_FILE_ABORTED) {
        if (s_upFile) s_upFile.close();
        s_upRejected = true;
        snprintf(s_note, sizeof(s_note), "Upload aborted");
    }
}

void GameFileServer::handleUploadDone() {
    // A rejected upload may have left a truncated file behind — remove it.
    if (s_upRejected) {
        HTTPUpload& up = _http.upload();
        if (safeLuaName(up.filename.c_str())) {
            char path[64];
            snprintf(path, sizeof(path), "%s/%s", dirPath(), up.filename.c_str());
            LittleFS.remove(path);
        }
    } else {
        Log.infoln("Share: %s", s_note);
    }
    _http.sendHeader("Location", "/");
    _http.send(303);
}

#else  // !ESP32 — device-only tool; keep the TU compilable on host.

bool GameFileServer::start() { return false; }
void GameFileServer::handleClient() {}
void GameFileServer::stop() {}
uint8_t GameFileServer::stationCount() const { return 0; }

#endif
