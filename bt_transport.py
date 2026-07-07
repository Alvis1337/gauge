"""
Synchronous wrapper around bleak's async BLE GATT API, so obd.py's existing
threaded/lock-based ObdClient doesn't need to become async itself. Runs one
persistent asyncio event loop on a dedicated background thread; every bleak
call is marshalled onto it via run_coroutine_threadsafe().

Most ELM327 BLE adapters don't expose a dedicated OBD GATT profile — they
tunnel the same AT-command text protocol the WiFi/RFCOMM versions use
through a generic BLE-serial passthrough, under one of a handful of known
UUID conventions (Nordic UART Service, or one of a couple FFE0/FFF0-family
vendor services from the generic BLE-serial modules the adapter firmware
is often built on). connect() tries those first, then falls back to
inspecting every characteristic's advertised properties for a writable +
notifiable pair, since an unrecognized clone can still work as long as it
exposes a standard write/notify pair under a UUID none of the known
conventions use.
"""
import asyncio
import logging
import queue
import threading
import time
from typing import Optional

from bleak import BleakClient

log = logging.getLogger("bt_transport")

# (write_uuid, notify_uuid) — tried in order, first pair that both exist
# on the device wins.
_KNOWN_UUIDS = [
    # Nordic UART Service: write is peripheral's RX, notify is peripheral's TX.
    ("6e400002-b5a3-f393-e0a9-e50e24dcca9e", "6e400003-b5a3-f393-e0a9-e50e24dcca9e"),
    # HM-10/AT-09 style BLE-serial modules — one characteristic does both.
    ("0000ffe1-0000-1000-8000-00805f9b34fb", "0000ffe1-0000-1000-8000-00805f9b34fb"),
    # Another common vendor split: write FFF2, notify FFF1.
    ("0000fff2-0000-1000-8000-00805f9b34fb", "0000fff1-0000-1000-8000-00805f9b34fb"),
]

_CONNECT_TIMEOUT = 10.0


class _Loop:
    """One asyncio event loop on a dedicated daemon thread, shared by every
    BleTransport in the process — bleak doesn't need (and a single BLE
    radio can't really use) a fresh loop/thread per connection attempt."""
    _loop: Optional[asyncio.AbstractEventLoop] = None
    _lock = threading.Lock()

    @classmethod
    def get(cls) -> asyncio.AbstractEventLoop:
        with cls._lock:
            if cls._loop is None:
                ready = threading.Event()

                def _run():
                    loop = asyncio.new_event_loop()
                    asyncio.set_event_loop(loop)
                    cls._loop = loop
                    ready.set()
                    loop.run_forever()

                threading.Thread(target=_run, daemon=True, name="ble-loop").start()
                ready.wait()
            return cls._loop


def run_coro(coro, timeout: float):
    """Run a coroutine on the shared BLE event loop from any thread and
    block for its result. Used by bt_discovery.py and netdiag.py too, not
    just this module, since they also need to talk to bleak from
    synchronous code."""
    fut = asyncio.run_coroutine_threadsafe(coro, _Loop.get())
    return fut.result(timeout=timeout)


def _find_characteristics(client: BleakClient):
    """Known UUID conventions first, then a generic properties-based scan
    for any writable + notifiable pair."""
    by_uuid = {c.uuid.lower(): c for s in client.services for c in s.characteristics}

    for write_uuid, notify_uuid in _KNOWN_UUIDS:
        w, n = by_uuid.get(write_uuid), by_uuid.get(notify_uuid)
        if w and n:
            log.debug("using known BLE UART UUIDs write=%s notify=%s", write_uuid, notify_uuid)
            return w, n

    write_char = notify_char = None
    for svc in client.services:
        for c in svc.characteristics:
            props = c.properties
            if write_char is None and ("write" in props or "write-without-response" in props):
                write_char = c
            if notify_char is None and ("notify" in props or "indicate" in props):
                notify_char = c
    if write_char and notify_char:
        log.debug("using generic properties-based characteristics write=%s notify=%s",
                  write_char.uuid, notify_char.uuid)
    return write_char, notify_char


class BleTransport:
    """One ELM327-over-BLE connection. Exposes the small slice of
    connect/send/receive/close behavior obd.py actually needs, so its
    logic barely differs from the old TCP-socket version."""

    def __init__(self):
        self._client: Optional[BleakClient] = None
        self._write_char = None
        self._rx_queue: "queue.Queue[bytes]" = queue.Queue()

    def _on_notify(self, _sender, data):
        self._rx_queue.put(bytes(data))

    def connect(self, address: str, timeout: float = _CONNECT_TIMEOUT):
        async def _connect():
            client = BleakClient(address, timeout=timeout)
            await client.connect()
            return client

        client = run_coro(_connect(), timeout + 2)
        write_char, notify_char = _find_characteristics(client)
        if not write_char or not notify_char:
            run_coro(client.disconnect(), 5)
            raise ConnectionError(
                f"{address}: no writable+notifiable GATT characteristic pair found "
                f"(checked known UART UUID conventions and every advertised characteristic)"
            )

        async def _start_notify():
            await client.start_notify(notify_char, self._on_notify)

        run_coro(_start_notify(), 5)
        self._client = client
        self._write_char = write_char

    def send(self, payload: bytes):
        async def _write():
            await self._client.write_gatt_char(self._write_char, payload, response=False)
        run_coro(_write(), 5)

    def recv_until(self, marker: bytes, timeout: float) -> bytes:
        """Drain notify packets until `marker` shows up in the accumulated
        buffer or timeout elapses — the GATT equivalent of the old TCP
        path's `while b">" not in buf: buf += sock.recv(256)` loop."""
        buf = b""
        deadline = time.monotonic() + timeout
        while marker not in buf:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            try:
                buf += self._rx_queue.get(timeout=remaining)
            except queue.Empty:
                break
        return buf

    @property
    def connected(self) -> bool:
        return self._client is not None and self._client.is_connected

    def close(self):
        client, self._client = self._client, None
        if client is not None:
            try:
                run_coro(client.disconnect(), 5)
            except Exception:
                log.debug("error during BLE disconnect", exc_info=True)
