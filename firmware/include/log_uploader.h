// Uploads the OBD log ring buffer to a webhook URL via HTTPS POST.
// Payload is Discord-compatible JSON {"content":"```\n...\n```"} so it
// works with Discord webhooks, webhook.site, Pipedream, etc. with no
// server setup. Must be called before NimBLEDevice::init() (Update Mode)
// — mbedTLS needs ~32KB heap that NimBLE would otherwise consume.
#pragma once
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include "obd_log.h"

namespace log_uploader {

inline String upload(const char *webhookUrl) {
    if (!webhookUrl || strlen(webhookUrl) == 0)
        return "No webhook URL set — configure it in Update Mode";

    static char lines[obd_log::LINES][obd_log::LINE_LEN];
    size_t n = obd_log::snapshot(lines, obd_log::LINES);
    if (n == 0) return "Log is empty — no OBD activity recorded yet";

    // Build Discord-compatible JSON payload.
    String body = "{\"content\":\"```\\n";
    for (size_t i = 0; i < n; i++) {
        for (const char *p = lines[i]; *p; p++) {
            if      (*p == '"')  body += "\\\"";
            else if (*p == '\\') body += "\\\\";
            else                 body += *p;
        }
        body += "\\n";
    }
    body += "```\"}";

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient https;
    https.setTimeout(15000);  // 15s HTTP response timeout (ms)
    if (!https.begin(client, webhookUrl)) return "HTTPS begin failed";
    https.addHeader("Content-Type", "application/json");
    int code = https.POST(body);
    String err = code >= 200 && code < 300 ? "" : "POST failed: HTTP " + String(code);
    https.end();
    return err;
}

}  // namespace log_uploader
