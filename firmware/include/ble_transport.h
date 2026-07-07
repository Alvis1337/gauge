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

        NimBLEAddress addr(address, BLE_ADDR_PUBLIC);
        _client = NimBLEDevice::createClient();
        // setConnectTimeout takes whole seconds, not ms — round up so a
        // sub-second caller-requested timeout doesn't truncate to 0.
        uint8_t timeout_s = (uint8_t)std::min<uint32_t>(255, (timeout_ms + 999) / 1000);
        _client->setConnectTimeout(timeout_s ? timeout_s : 1);
        if (!_client->connect(addr)) {
            _cleanupClient();
            return false;
        }

        if (!_findCharacteristics()) {
            _client->disconnect();
            _cleanupClient();
            return false;
        }

        _notifyChar->subscribe(true, [this](NimBLERemoteCharacteristic *, uint8_t *data, size_t len, bool) {
            BleRxChunk chunk;
            size_t n = len > sizeof(chunk.data) ? sizeof(chunk.data) : len;
            memcpy(chunk.data, data, n);
            chunk.len = n;
            xQueueSend(_rxQueue, &chunk, 0);
        });

        return true;
    }

    bool send(const uint8_t *data, size_t len) {
        if (!_writeChar) return false;
        return _writeChar->writeValue(data, len, false);
    }

    // Drains notify packets into `out` until `marker` byte shows up or
    // timeout elapses — the GATT equivalent of the old TCP path's
    // "while marker not in buf: buf += sock.recv()" loop.
    size_t recvUntil(uint8_t marker, uint8_t *out, size_t out_cap, uint32_t timeout_ms) {
        size_t total = 0;
        uint32_t deadline = millis() + timeout_ms;
        while (total == 0 || out[total - 1] != marker) {
            int32_t remaining = (int32_t)(deadline - millis());
            if (remaining <= 0) break;
            BleRxChunk chunk;
            if (xQueueReceive(_rxQueue, &chunk, pdMS_TO_TICKS(remaining)) != pdTRUE) break;
            size_t n = chunk.len;
            if (total + n > out_cap) n = out_cap - total;
            memcpy(out + total, chunk.data, n);
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
