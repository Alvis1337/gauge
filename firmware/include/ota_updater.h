// Fetches the latest firmware.bin from the GitHub release published by
// .github/workflows/firmware.yml and flashes it into the inactive OTA
// slot (see platformio.ini's min_spiffs.csv partition change), then
// reboots into it. Runs synchronously — callers should invoke this from
// its own task, not the UI/render loop, and poll a status string rather
// than blocking (see settings screen wiring in main.cpp).
#pragma once
#include <HTTPClient.h>
#include <Update.h>
#include <WiFiClientSecure.h>
#include "gauge_settings.h"

namespace ota_updater {

// GitHub's release-asset redirect lands on a different host
// (release-assets.githubusercontent.com) serving a pre-signed URL — the
// client has to actually follow that redirect, and setInsecure() skips
// certificate validation on whichever host it ends up on. That's a
// deliberate tradeoff for a hobby project pulling its own public repo's
// release over HTTPS, not a hardened production update channel.
inline const char *kUrl = "https://github.com/Alvis1337/gauge/releases/latest/download/firmware.bin";

// Returns "" on success (device reboots before returning) or "up to date"
// (nothing to do), or a human-readable error string on failure.
inline String checkAndUpdate(GaugeSettings &settings) {
    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient https;
    static const char *kHeaders[] = {"ETag"};
    https.collectHeaders(kHeaders, 1);
    https.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    if (!https.begin(client, kUrl)) {
        return "could not start HTTPS request";
    }

    int code = https.GET();
    if (code != HTTP_CODE_OK) {
        String err = "GET failed: HTTP " + String(code);
        https.end();
        return err;
    }

    // The release asset's ETag changes every time the workflow publishes
    // a new binary — comparing it against what we last successfully
    // flashed avoids redownloading and reflashing the identical image on
    // every boot just because WiFi happened to be in range.
    String etag = https.header("ETag");
    if (!etag.isEmpty() && etag == settings.lastOtaEtag().c_str()) {
        https.end();
        return "up to date";
    }

    int len = https.getSize();
    if (!Update.begin(len > 0 ? len : UPDATE_SIZE_UNKNOWN)) {
        https.end();
        return "Update.begin() failed: " + String(Update.errorString());
    }

    WiFiClient *stream = https.getStreamPtr();
    size_t written = Update.writeStream(*stream);
    https.end();

    if (written == 0 || (len > 0 && written != (size_t)len)) {
        Update.abort();
        return "incomplete write (" + String(written) + "/" + String(len) + " bytes)";
    }
    if (!Update.end()) {
        return "Update.end() failed: " + String(Update.errorString());
    }
    if (!Update.isFinished()) {
        return "update did not finish cleanly";
    }

    if (!etag.isEmpty()) settings.setLastOtaEtag(etag.c_str());
    ESP.restart();
    return "";  // unreachable
}

}  // namespace ota_updater
