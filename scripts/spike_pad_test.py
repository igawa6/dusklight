#!/usr/bin/env python3
"""Throwaway test client for phone-gamepad passthrough (DUSK_PHONE_SPIKE
phase 5). Sends continuous "pad" state messages, holding whatever fields
are passed for a duration. Not part of the shipped feature."""
import asyncio
import json
import sys
import time

import websockets

NEUTRAL = {
    "type": "pad", "up": False, "down": False, "left": False, "right": False,
    "a": False, "b": False, "x": False, "y": False, "start": False,
    "l": False, "r": False, "z": False,
    "leftStickX": 0.0, "leftStickY": 0.0, "rightStickX": 0.0, "rightStickY": 0.0,
    "leftTrigger": 0.0, "rightTrigger": 0.0,
}


async def drain(ws):
    # The server keeps pushing companion frames the whole time this
    # connection is open (see phone_spike_ws.cpp's send_binary_frame) —
    # not reading them backs up the socket and gets this connection
    # dropped ("frame payload send failed/timed out"), which silently
    # kills passthrough mid-test. Must run concurrently with hold().
    try:
        async for _ in ws:
            pass
    except websockets.exceptions.ConnectionClosed:
        pass


async def hold(ws, overrides, seconds, hz=30):
    state = dict(NEUTRAL)
    state.update(overrides)
    payload = json.dumps(state)
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        await ws.send(payload)
        await asyncio.sleep(1.0 / hz)


async def neutral(ws):
    await ws.send(json.dumps(NEUTRAL))


async def main():
    uri = sys.argv[1]
    async with websockets.connect(uri, max_size=None) as ws:
        overrides = json.loads(sys.argv[2])
        seconds = float(sys.argv[3]) if len(sys.argv) > 3 else 1.0
        print(f"holding {overrides} for {seconds}s")
        drain_task = asyncio.create_task(drain(ws))
        await hold(ws, overrides, seconds)
        await neutral(ws)
        await asyncio.sleep(0.1)
        drain_task.cancel()
        print("done, released to neutral")


if __name__ == "__main__":
    asyncio.run(main())
