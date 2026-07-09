// BLE GATT transport for ELM327-over-BLE adapters — the firmware
// equivalent of bt_transport.py. NimBLE's notify callback fires on the
// NimBLE host task, not the caller's task, so received bytes are handed
// off through a FreeRTOS queue rather than a plain shared buffer — the
// same role Python's queue.Queue played in BleTransport._rx_queue.
//
// Most ELM327 BLE adapters don't expose a dedicated OBD GATT profile —
// they tunnel the same AT-command text protocol over a generic
// BLE-serial passthrough, under one of a handful of known UUID
// conventions (Nordic UART Service, or FFE0/FFF0-family vendor services
// from the generic BLE-serial modules the adapter firmware is often built
// on). connect() tries those first, then falls back to inspecting every
// characteristic's advertised properties for a writable + notifiable
// pair, since an unrecognized clone can still work as long as it exposes
// a standard write/notify pair under a UUID none of the known
// conventions use.
#pragma once
#include <NimBLEDevice.h>
#include <algorithm>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

struct BleRxChunk {
    uint8_t data[247];  // >= max BLE 5 ATT MTU payload; oversized chunks are truncated, never overrun
    size_t len;
};

class BleTransport {
public:
    ~BleTransport() { close(); }

    bool connect(const std::string &address, uint32_t timeout_ms) {
        _rxQueue = xQueueCreate(16, sizeof(BleRxChunk));

        uint8_t timeout_s = (uint8_t)std::min<uint32_t>(255, (timeout_ms + 999) / 1000);
        if (!timeout_s) timeout_s = 1;

        // Some adapters (e.g. iOS-Vlink / Vgate iCar) use a RANDOM BLE
        // address; others use PUBLIC. The scanner stores the address string
        // without the type, so try PUBLIC first then RANDOM.
        bool ble_ok = false;
        for (uint8_t atype : {BLE_ADDR_PUBLIC, BLE_ADDR_RANDOM}) {
            _client = NimBLEDevice::createClient();
            _client->setConnectTimeout(timeout_s);
            if (_client->connect(NimBLEAddress(address, atype))) {
                ble_ok = true;
                break;
            }
            _cleanupClient();
        }
        if (!ble_ok) return false;

        if (!_findCharacteristics()) {
            _client->disconnect();
            _cleanupClient();
            return false;
        }

        // Some adapters (Vgate iCar / iOS-Vlink) use indicate rather than
        // notify. subscribe(true) = notify, subscribe(false) = indicate.
        // Try both; the first that succeeds is what the adapter supports.
        auto rxCb = [this](NimBLERemoteCharacteristic *, uint8_t *data, size_t len, bool) {
            BleRxChunk chunk;
            size_t n = len > sizeof(chunk.data) ? sizeof(chunk.data) : len;
            memcpy(chunk.data, data, n);
            chunk.len = n;
            xQueueSend(_rxQueue, &chunk, 0);
        };
        if (!_notifyChar->subscribe(true, rxCb))
            _notifyChar->subscribe(false, rxCb);

        // Give the adapter ~500ms to settle after the GATT subscription
        // before the first AT command — Vgate/iOS-Vlink drops the initial
        // ATZ if sent immediately after connection.
        delay(500);

        return true;
    }

    bool send(const uint8_t *data, size_t len) {
        if (!_writeChar) return false;
        return _writeChar->writeValue(data, len, false);
    }

    // Drains notify packets into `out` until `marker` byte shows up or
    // timeout elapses — the GATT equivalent of the old TCP path's
    // "while marker not in buf: buf += sock.recv()" loop.
    //
    // Scans each chunk's bytes individually so the marker is caught
    // wherever it falls in the chunk, not just when it happens to be
    // the very last byte received. Some BLE-serial bridges batch bytes
    // and the '>' prompt can arrive mid-chunk with extra bytes after it.
    size_t recvUntil(uint8_t marker, uint8_t *out, size_t out_cap, uint32_t timeout_ms) {
        size_t total = 0;
        uint32_t deadline = millis() + timeout_ms;
        for (;;) {
            int32_t remaining = (int32_t)(deadline - millis());
            if (remaining <= 0) break;
            BleRxChunk chunk;
            if (xQueueReceive(_rxQueue, &chunk, pdMS_TO_TICKS(remaining)) != pdTRUE) break;
            size_t n = chunk.len;
            if (total + n > out_cap) n = out_cap - total;
            memcpy(out + total, chunk.data, n);
            for (size_t i = total; i < total + n; i++) {
                if (out[i] == marker) return i + 1;
            }
            total += n;
            if (total >= out_cap) break;
        }
        return total;
    }

    bool connected() const { return _client && _client->isConnected(); }

    void close() {
        if (_client) {
            if (_client->isConnected()) _client->disconnect();
            _cleanupClient();
        }
        if (_rxQueue) { vQueueDelete(_rxQueue); _rxQueue = nullptr; }
    }

private:
    NimBLEClient *_client = nullptr;
    NimBLERemoteCharacteristic *_writeChar = nullptr;
    NimBLERemoteCharacteristic *_notifyChar = nullptr;
    QueueHandle_t _rxQueue = nullptr;

    void _cleanupClient() {
        if (_client) { NimBLEDevice::deleteClient(_client); _client = nullptr; }
        _writeChar = nullptr;
        _notifyChar = nullptr;
    }

    // (service UUID, write char UUID, notify char UUID) — tried in order,
    // first whose service+both characteristics all exist on the device wins.
    bool _findCharacteristics() {
        static const char *kKnownTriples[][3] = {
            // Vgate iCar / iOS-Vlink proprietary service — write and notify
            // share the same characteristic UUID.
            {"e7810a71-73ae-499d-8c15-faa9aef0c3f2",
             "bef8d6c9-9c21-4c9e-b632-bd58c1009f9f", "bef8d6c9-9c21-4c9e-b632-bd58c1009f9f"},
            // Nordic UART Service: write is peripheral's RX, notify is peripheral's TX.
            {"6e400001-b5a3-f393-e0a9-e50e24dcca9e",
             "6e400002-b5a3-f393-e0a9-e50e24dcca9e", "6e400003-b5a3-f393-e0a9-e50e24dcca9e"},
            // HM-10/AT-09 style BLE-serial modules — one characteristic does both.
            {"0000ffe0-0000-1000-8000-00805f9b34fb",
             "0000ffe1-0000-1000-8000-00805f9b34fb", "0000ffe1-0000-1000-8000-00805f9b34fb"},
            // Another common vendor split: write FFF2, notify FFF1.
            {"0000fff0-0000-1000-8000-00805f9b34fb",
             "0000fff2-0000-1000-8000-00805f9b34fb", "0000fff1-0000-1000-8000-00805f9b34fb"},
        };

        for (auto &triple : kKnownTriples) {
            NimBLERemoteService *svc = _client->getService(triple[0]);
            if (!svc) continue;
            NimBLERemoteCharacteristic *w = svc->getCharacteristic(triple[1]);
            NimBLERemoteCharacteristic *n = svc->getCharacteristic(triple[2]);
            if (w && n) { _writeChar = w; _notifyChar = n; return true; }
        }

        auto *services = _client->getServices(true);
        for (auto *svc : *services) {
            for (auto *chr : *svc->getCharacteristics(true)) {
                if (!_writeChar && (chr->canWrite() || chr->canWriteNoResponse())) _writeChar = chr;
                if (!_notifyChar && (chr->canNotify() || chr->canIndicate())) _notifyChar = chr;
            }
        }
        return _writeChar && _notifyChar;
    }
};
