// Auto-discovery for BLE ELM327-compatible OBD-II adapters — firmware
// equivalent of bt_discovery.py. BLE devices broadcast their name before
// any GATT connection is made, so candidates are filtered by name up
// front, then verified with a real ELM327 ATZ handshake before being
// reported as found — an unrelated BLE device (a phone, earbuds) whose
// name happens to match one of the hints below isn't enough on its own.
#pragma once
#include <NimBLEDevice.h>
#include <algorithm>
#include <string>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "ble_transport.h"

struct BtScanResult {
    std::string address;
    std::string name;
};

namespace bt_discovery {

// Now that the settings UI can trigger a manual scan from its own task
// while obd_task (on the other core) may independently call discover()
// -> scan() as part of its own auto-discovery fallback, two tasks could
// otherwise hit NimBLE's single scan state machine at once. A plain
// mutex serializes them — whichever call loses the race just waits for
// the other's scan window to finish instead of racing it.
inline SemaphoreHandle_t _scanMutex() {
    static SemaphoreHandle_t m = xSemaphoreCreateMutex();
    return m;
}

// Substrings seen in real ELM327/OBD BLE adapter advertised names —
// matched case-insensitively. Not exhaustive; a device that doesn't match
// any of these can still be picked manually from the full scan list in
// the settings UI.
inline bool looksLikeObdName(const std::string &name) {
    if (name.empty()) return false;
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    static const char *hints[] = {"obd", "elm327", "icar", "vgate", "vlinker", "obdii", "obd2"};
    for (auto *h : hints) if (lower.find(h) != std::string::npos) return true;
    return false;
}

// Inner scan — caller must hold _scanMutex().
inline std::vector<BtScanResult> _scanNoLock(uint32_t scan_seconds = 6) {
    NimBLEScan *pScan = NimBLEDevice::getScan();
    pScan->setActiveScan(true);
    NimBLEScanResults results = pScan->start(scan_seconds, false);
    std::vector<BtScanResult> out;
    for (int i = 0; i < results.getCount(); i++) {
        NimBLEAdvertisedDevice dev = results.getDevice(i);
        std::string name = dev.getName();
        std::string addr = dev.getAddress().toString();
        out.push_back({addr, name.empty() ? addr : name});
    }
    std::stable_sort(out.begin(), out.end(), [](const BtScanResult &a, const BtScanResult &b) {
        return looksLikeObdName(a.name) && !looksLikeObdName(b.name);
    });
    pScan->clearResults();
    return out;
}

// Every nearby BLE device, with name-hinted OBD-looking devices sorted
// first. No GATT connection is made here — this backs the settings UI's
// device list, where a person's judgment substitutes for verifying every
// device seen.
inline std::vector<BtScanResult> scan(uint32_t scan_seconds = 6) {
    xSemaphoreTake(_scanMutex(), portMAX_DELAY);
    auto out = _scanNoLock(scan_seconds);
    xSemaphoreGive(_scanMutex());
    return out;
}

// Opens a real GATT connection and confirms an ELM327 ATZ handshake
// actually comes back — so a device with merely a plausible name isn't
// mistaken for a working adapter.
inline bool verify(const std::string &address, uint32_t timeout_ms = 5000) {
    BleTransport transport;
    if (!transport.connect(address, timeout_ms)) return false;
    const char *atz = "ATZ\r";
    bool sent = transport.send((const uint8_t *)atz, 4);
    uint8_t buf[128];
    size_t n = sent ? transport.recvUntil('>', buf, sizeof(buf), timeout_ms) : 0;
    transport.close();
    if (n == 0) return false;
    std::string text((const char *)buf, n);
    std::transform(text.begin(), text.end(), text.begin(), ::toupper);
    return text.find("ELM327") != std::string::npos || text.find('>') != std::string::npos;
}

// Scans for nearby BLE devices whose advertised name hints at being an
// OBD adapter, verifies each via a real GATT connect in order, and
// returns the first one that actually replies like an ELM327 — or "".
// This is the automatic fallback the polling loop reaches for once the
// configured address goes unreachable; the interactive "Scan for
// Bluetooth adapters" list in the settings UI uses scan() instead and
// lets a person pick.
//
// The mutex is held across both the scan AND the verify() connect so
// a concurrent settings-UI scan() call can't start a BLE passive scan
// while we're mid-connect — the NimBLE radio can't do both at once.
inline std::string discover(uint32_t scan_seconds = 6) {
    xSemaphoreTake(_scanMutex(), portMAX_DELAY);
    auto candidates = _scanNoLock(scan_seconds);
    std::string found;
    for (auto &c : candidates) {
        if (!looksLikeObdName(c.name)) continue;
        if (verify(c.address)) { found = c.address; break; }
    }
    xSemaphoreGive(_scanMutex());
    return found;
}

}  // namespace bt_discovery
