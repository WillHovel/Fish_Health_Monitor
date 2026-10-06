#!/usr/bin/env python3
"""
tools/ble_monitor.py
=====================
Live BLE telemetry monitor for the aquatic_biomonitor_fw firmware.

Connects to the AquaBioMonitor peripheral, subscribes to telemetry
notifications, plots live BPM / HRV / VR / Cough traces in real time
with matplotlib, and exposes a command-line interface to switch species
and control acquisition without a physical button.

DEPENDENCIES
------------
    pip install bleak matplotlib numpy

USAGE
-----
    # Auto-scan for "AquaBioMonitor" and connect:
    python tools/ble_monitor.py

    # Connect to a specific BLE address:
    python tools/ble_monitor.py --address AA:BB:CC:DD:EE:FF

    # Larger rolling window (seconds of history shown):
    python tools/ble_monitor.py --window 120

INTERACTIVE COMMANDS (type in terminal while monitor is running)
-----------------------------------------------------------------
    start          → Send acquisition start command (0x01)
    stop           → Send acquisition stop command  (0x00)
    species 1      → Switch to Glass Catfish
    species 2      → Switch to Atlantic Silverside
    species 3      → Switch to Inland Silverside
    species 4      → Switch to Chromis viridis
    quit / q       → Disconnect and exit

TELEMETRY WIRE FORMAT (26 bytes, little-endian)
-----------------------------------------------
    Offset  Size  Type      Field
    ──────  ────  ────────  ──────────────────────────
       0      4   float32   BPM (beats per minute)
       4      4   float32   HRV RMSSD (ms)
       8      4   float32   Ventilation Rate (br/min)
      12      4   float32   Cough Rate (coughs/min)
      16      4   float32   Filtered Signal (mV)
      20      1   uint8     Health State (0–5)
      21      1   uint8     Active Species ID (1–4)
      22      4   uint8[4]  Reserved

AUTHOR  : aquatic_biomonitor_fw project
LICENSE : Apache-2.0
"""

import argparse
import asyncio
import struct
import sys
import threading
import time
from collections import deque
from datetime import datetime
from typing import Optional

import numpy as np
import matplotlib
matplotlib.use("TkAgg")          # Change to "Qt5Agg" if TkAgg unavailable
import matplotlib.pyplot as plt
import matplotlib.animation as animation
from matplotlib.gridspec import GridSpec

from bleak import BleakClient, BleakScanner
from bleak.exc import BleakError

# ─── BLE Service / Characteristic UUIDs ──────────────────────────────────────
DEVICE_NAME   = "AquaBioMonitor"
SERVICE_UUID  = "12345678-1234-5678-1234-56789abcdef0"
SPECIES_UUID  = "12345678-1234-5678-1234-56789abcdef1"   # WRITE
TELEM_UUID    = "12345678-1234-5678-1234-56789abcdef2"   # NOTIFY
ACQ_CTRL_UUID = "12345678-1234-5678-1234-56789abcdef3"   # WRITE (0x00/0x01)

# ─── Telemetry packet ─────────────────────────────────────────────────────────
TELEM_FMT    = "<5fBB4x"   # 5 floats + 2 uint8 + 4 padding = 26 bytes
TELEM_SIZE   = struct.calcsize(TELEM_FMT)

HEALTH_LABELS = {
    0: ("NORMAL",        "green"),
    1: ("TACHYCARDIA",   "orange"),
    2: ("BRADYCARDIA",   "dodgerblue"),
    3: ("RESP DISTRESS", "red"),
    4: ("LOW HRV",       "darkorange"),
    5: ("NO SIGNAL",     "grey"),
}

SPECIES_NAMES = {
    1: "Glass Catfish",
    2: "Atlantic Silverside",
    3: "Inland Silverside",
    4: "Chromis viridis",
}

# ─── Shared state (thread-safe via deque + threading.Event) ──────────────────
MAX_POINTS = 500   # rolling history depth

_data: dict[str, deque] = {
    "bpm":   deque(maxlen=MAX_POINTS),
    "hrv":   deque(maxlen=MAX_POINTS),
    "vr":    deque(maxlen=MAX_POINTS),
    "cough": deque(maxlen=MAX_POINTS),
    "mv":    deque(maxlen=MAX_POINTS),
    "t":     deque(maxlen=MAX_POINTS),   # relative time in seconds
}

_meta = {
    "health":  0,
    "health_label": "NO SIGNAL",
    "health_color": "grey",
    "species": 1,
    "species_name": "---",
    "connected": False,
    "last_rx": 0.0,
    "rx_count": 0,
}

_t0 = time.monotonic()   # time reference for relative timestamps

# BLE async loop runs in a background daemon thread
_ble_loop: Optional[asyncio.AbstractEventLoop] = None
_ble_client: Optional[BleakClient] = None
_cmd_queue: asyncio.Queue   # populated by input thread, consumed by BLE loop

_stop_event = threading.Event()


# ─── BLE notification handler ─────────────────────────────────────────────────

def _on_telem_notify(handle: int, data: bytearray) -> None:
    """Called in the BLE thread whenever a telemetry notification arrives."""
    if len(data) < TELEM_SIZE:
        return

    bpm, hrv, vr, cough, mv, health, species = struct.unpack_from(TELEM_FMT, data)

    t_now = time.monotonic() - _t0

    # Thread-safe: deque.append is atomic for CPython
    _data["t"].append(t_now)
    _data["bpm"].append(bpm)
    _data["hrv"].append(hrv)
    _data["vr"].append(vr)
    _data["cough"].append(cough)
    _data["mv"].append(mv)

    label, color = HEALTH_LABELS.get(int(health), ("UNKNOWN", "grey"))
    _meta["health"]       = int(health)
    _meta["health_label"] = label
    _meta["health_color"] = color
    _meta["species"]      = int(species)
    _meta["species_name"] = SPECIES_NAMES.get(int(species), f"ID={species}")
    _meta["last_rx"]      = t_now
    _meta["rx_count"]    += 1


# ─── BLE async tasks ──────────────────────────────────────────────────────────

async def _ble_write(uuid: str, payload: bytes, response: bool = True) -> None:
    """Send a GATT write to the connected peripheral."""
    if _ble_client and _ble_client.is_connected:
        try:
            await _ble_client.write_gatt_char(uuid, payload, response=response)
        except BleakError as exc:
            print(f"\n[BLE] Write error: {exc}", flush=True)


async def _command_dispatcher() -> None:
    """
    Consume commands from _cmd_queue (posted by the input thread) and
    execute them against the connected BLE peripheral.
    """
    while not _stop_event.is_set():
        try:
            cmd: str = await asyncio.wait_for(_cmd_queue.get(), timeout=0.5)
        except asyncio.TimeoutError:
            continue

        parts = cmd.strip().lower().split()
        if not parts:
            continue

        verb = parts[0]

        if verb in ("quit", "q"):
            _stop_event.set()

        elif verb == "start":
            await _ble_write(ACQ_CTRL_UUID, bytes([0x01]))
            print("[CMD] Acquisition START sent", flush=True)

        elif verb == "stop":
            await _ble_write(ACQ_CTRL_UUID, bytes([0x00]))
            print("[CMD] Acquisition STOP sent", flush=True)

        elif verb == "species" and len(parts) == 2:
            try:
                sid = int(parts[1])
                assert 1 <= sid <= 4
                await _ble_write(SPECIES_UUID, bytes([sid]))
                print(f"[CMD] Species switch → {SPECIES_NAMES.get(sid, '?')} (ID={sid})",
                      flush=True)
            except (ValueError, AssertionError):
                print("[CMD] Usage: species <1|2|3|4>", flush=True)

        else:
            print(f"[CMD] Unknown command: '{cmd.strip()}'", flush=True)


async def _scan_and_connect(address: Optional[str]) -> Optional[BleakClient]:
    """
    Scan for the AquaBioMonitor peripheral (or connect directly if address given).
    Returns a connected BleakClient or None on failure.
    """
    if address:
        print(f"[BLE] Connecting to {address} ...", flush=True)
        client = BleakClient(address, disconnected_callback=_on_disconnected)
    else:
        print(f"[BLE] Scanning for '{DEVICE_NAME}' ...", flush=True)
        device = await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=15.0)
        if device is None:
            print(f"[BLE] '{DEVICE_NAME}' not found in scan.", flush=True)
            return None
        print(f"[BLE] Found: {device.name} ({device.address})", flush=True)
        client = BleakClient(device, disconnected_callback=_on_disconnected)

    await client.connect()
    return client


def _on_disconnected(client: BleakClient) -> None:
    """Callback fired by bleak when the peripheral disconnects."""
    _meta["connected"] = False
    print("\n[BLE] Disconnected.", flush=True)
    _stop_event.set()


async def _ble_main(address: Optional[str]) -> None:
    """
    Top-level BLE coroutine:
      1. Scan / connect
      2. Subscribe to telemetry notifications
      3. Auto-send acquisition start
      4. Run command dispatcher until stop event
    """
    global _ble_client, _cmd_queue
    _cmd_queue = asyncio.Queue()

    client = await _scan_and_connect(address)
    if client is None:
        _stop_event.set()
        return

    _ble_client = client
    _meta["connected"] = True

    print(f"[BLE] Connected ✓  (MTU={client.mtu_size})", flush=True)
    print("[BLE] Subscribing to telemetry notifications ...", flush=True)

    await client.start_notify(TELEM_UUID, _on_telem_notify)

    # Auto-start acquisition on connect
    await asyncio.sleep(0.5)   # allow CCC subscription to be acknowledged
    await _ble_write(ACQ_CTRL_UUID, bytes([0x01]))
    print("[BLE] Acquisition started automatically.", flush=True)
    print_help()

    # Run command loop
    await _command_dispatcher()

    # Clean shutdown
    await _ble_write(ACQ_CTRL_UUID, bytes([0x00]))   # stop acquisition
    await client.stop_notify(TELEM_UUID)
    await client.disconnect()
    print("[BLE] Clean disconnect.", flush=True)


def _run_ble_thread(address: Optional[str]) -> None:
    """Entry point for the BLE daemon thread."""
    global _ble_loop
    _ble_loop = asyncio.new_event_loop()
    asyncio.set_event_loop(_ble_loop)
    try:
        _ble_loop.run_until_complete(_ble_main(address))
    finally:
        _ble_loop.close()


# ─── Keyboard input thread ────────────────────────────────────────────────────

def _input_thread() -> None:
    """Reads stdin commands and pushes them onto the async command queue."""
    while not _stop_event.is_set():
        try:
            line = input()
        except EOFError:
            break
        if _ble_loop and _cmd_queue:
            asyncio.run_coroutine_threadsafe(
                _cmd_queue.put(line), _ble_loop
            )


def print_help() -> None:
    print(
        "\n┌──────────────────────────────────────────────────────┐\n"
        "│  AquaBioMonitor — Interactive Commands               │\n"
        "│  start          → begin acquisition                  │\n"
        "│  stop           → halt acquisition                   │\n"
        "│  species 1-4    → switch species profile             │\n"
        "│  quit / q       → disconnect and exit                │\n"
        "└──────────────────────────────────────────────────────┘",
        flush=True
    )


# ─── Matplotlib live plot ─────────────────────────────────────────────────────

def _build_figure() -> tuple:
    """Create and configure the live dashboard figure."""
    fig = plt.figure(figsize=(14, 9), facecolor="#111111")
    fig.suptitle("AquaBioMonitor — Live Telemetry Dashboard",
                 color="white", fontsize=13, y=0.98)

    gs = GridSpec(3, 2, figure=fig,
                  hspace=0.45, wspace=0.30,
                  left=0.07, right=0.97, top=0.93, bottom=0.08)

    # Axes: BPM, HRV, VR, Cough, Raw mV waveform (spanning bottom row)
    ax_bpm   = fig.add_subplot(gs[0, 0])
    ax_hrv   = fig.add_subplot(gs[0, 1])
    ax_vr    = fig.add_subplot(gs[1, 0])
    ax_cough = fig.add_subplot(gs[1, 1])
    ax_mv    = fig.add_subplot(gs[2, :])

    axes = [ax_bpm, ax_hrv, ax_vr, ax_cough, ax_mv]

    for ax in axes:
        ax.set_facecolor("#1a1a2e")
        ax.tick_params(colors="white", labelsize=8)
        ax.spines[:].set_color("#444466")
        for label in ax.get_xticklabels() + ax.get_yticklabels():
            label.set_color("white")

    ax_bpm.set_title("Heart Rate (BPM)",     color="white", fontsize=9)
    ax_hrv.set_title("HRV RMSSD (ms)",       color="white", fontsize=9)
    ax_vr.set_title("Ventilation Rate (br/min)", color="white", fontsize=9)
    ax_cough.set_title("Cough Rate (/min)",  color="white", fontsize=9)
    ax_mv.set_title("Filtered Waveform (mV)", color="white", fontsize=9)

    # Colour-coded lines
    line_bpm,   = ax_bpm.plot(  [], [], color="#00e5ff",  linewidth=1.2)
    line_hrv,   = ax_hrv.plot(  [], [], color="#76ff03",  linewidth=1.2)
    line_vr,    = ax_vr.plot(   [], [], color="#ffea00",  linewidth=1.2)
    line_cough, = ax_cough.plot([], [], color="#ff6d00",  linewidth=1.2)
    line_mv,    = ax_mv.plot(   [], [], color="#e040fb",  linewidth=0.8)

    # Status text annotations
    status_text = fig.text(
        0.50, 0.01,
        "Health: --- | Species: --- | Rx: 0 pkts",
        ha="center", va="bottom", color="white", fontsize=8,
        transform=fig.transFigure
    )

    lines = [line_bpm, line_hrv, line_vr, line_cough, line_mv]
    plot_axes = [ax_bpm, ax_hrv, ax_vr, ax_cough, ax_mv]
    return fig, lines, plot_axes, status_text


def _animate(frame: int,
             lines: list,
             plot_axes: list,
             status_text,
             window_sec: float) -> list:
    """FuncAnimation update function — called on each plot refresh tick."""
    # Snapshot shared deques (CPython GIL makes this safe)
    t_arr    = np.array(_data["t"])
    bpm_arr  = np.array(_data["bpm"])
    hrv_arr  = np.array(_data["hrv"])
    vr_arr   = np.array(_data["vr"])
    cough_arr= np.array(_data["cough"])
    mv_arr   = np.array(_data["mv"])

    datasets = [bpm_arr, hrv_arr, vr_arr, cough_arr, mv_arr]

    if len(t_arr) < 2:
        return lines

    # Rolling window
    t_now = t_arr[-1]
    t_min = max(t_arr[0], t_now - window_sec)
    mask  = t_arr >= t_min

    t_win = t_arr[mask]

    for line, ax, data_arr in zip(lines, plot_axes, datasets):
        y_win = data_arr[mask]
        if len(y_win) == 0:
            continue
        line.set_data(t_win, y_win)
        ax.set_xlim(t_win[0], t_win[-1] + 0.5)
        y_range = y_win.max() - y_win.min()
        pad = max(y_range * 0.1, 0.5)
        ax.set_ylim(y_win.min() - pad, y_win.max() + pad)

    # Status bar
    h_label = _meta["health_label"]
    h_color = _meta["health_color"]
    sp_name = _meta["species_name"]
    rx      = _meta["rx_count"]
    conn    = "● CONNECTED" if _meta["connected"] else "○ disconnected"
    status_text.set_text(
        f"{conn}  |  Health: {h_label}  |  Species: {sp_name}  |  Rx: {rx} pkts"
    )
    status_text.set_color(h_color)

    return lines


def _run_plot(window_sec: float) -> None:
    """Build and run the matplotlib live dashboard (main thread)."""
    plt.style.use("dark_background")
    fig, lines, axes, status_text = _build_figure()

    ani = animation.FuncAnimation(
        fig,
        _animate,
        fargs=(lines, axes, status_text, window_sec),
        interval=250,          # refresh every 250 ms
        blit=False,
        cache_frame_data=False,
    )

    plt.show(block=True)      # blocks until window is closed
    _stop_event.set()         # signal BLE thread to disconnect


# ─── Entry point ─────────────────────────────────────────────────────────────

def main() -> None:
    parser = argparse.ArgumentParser(
        description="Live BLE telemetry monitor for AquaBioMonitor",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument(
        "--address", metavar="BLE_ADDR",
        help="BLE MAC address to connect directly (e.g. AA:BB:CC:DD:EE:FF). "
             "Omit to auto-scan by device name.",
    )
    parser.add_argument(
        "--window", type=float, default=60.0, metavar="SECONDS",
        help="Rolling time window shown in plots (default: 60 s)",
    )
    args = parser.parse_args()

    print("═" * 56)
    print("  AquaBioMonitor — BLE Live Monitor")
    print(f"  Window: {args.window} s  |  Packet size: {TELEM_SIZE} bytes")
    print("═" * 56)

    # Launch BLE thread (daemon so it dies when main exits)
    ble_thread = threading.Thread(
        target=_run_ble_thread,
        args=(args.address,),
        daemon=True,
        name="ble-worker",
    )
    ble_thread.start()

    # Launch stdin input thread
    input_thread = threading.Thread(
        target=_input_thread,
        daemon=True,
        name="input-worker",
    )
    input_thread.start()

    # Run matplotlib on the main thread (required on macOS/Windows)
    _run_plot(window_sec=args.window)

    # Wait for BLE thread to finish its cleanup
    ble_thread.join(timeout=5.0)
    print("Bye.")


if __name__ == "__main__":
    main()
