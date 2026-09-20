#!/usr/bin/env python3
"""Throwaway test client for the phone-companion-spike WS server
(DUSK_PHONE_SPIKE). Connects, receives N binary (PNG) frames, verifies each
is a real PNG, and reports inter-frame timing. Also logs any interleaved
TEXT frames (DUSK_PHONE_SPIKE_STATE's "hud_state" JSON messages, or any
other future text message type) with a timestamp as they arrive, without
counting them toward the binary-frame total — this is how Phase 1 of the
state-streaming design (see docs/phone-companion-design.md) gets verified:
confirm a hud_state message lands within a frame or two of an in-game value
actually changing, and confirm none arrive while nothing is changing. Not
part of the shipped feature — delete along with the rest of the spike once
it's served its purpose."""
import asyncio
import json
import sys
import time

import websockets

PNG_MAGIC = b"\x89PNG\r\n\x1a\n"


async def main():
    uri = "ws://127.0.0.1:8765/"
    n_frames = int(sys.argv[1]) if len(sys.argv) > 1 else 20
    print(f"connecting to {uri} ...")
    async with websockets.connect(uri, max_size=None) as ws:
        print("connected, waiting for frames...")
        last_t = None
        intervals = []
        sizes = []
        binary_count = 0
        while binary_count < n_frames:
            data = await ws.recv()
            now = time.monotonic()
            if isinstance(data, str):
                try:
                    msg = json.loads(data)
                except ValueError:
                    print(f"[{now:.3f}] text frame (not valid JSON): {data!r}")
                    continue
                print(f"[{now:.3f}] {msg.get('type', '?')}: {msg}")
                continue
            ok = data[:8] == PNG_MAGIC
            if last_t is not None:
                intervals.append((now - last_t) * 1000.0)
            last_t = now
            sizes.append(len(data))
            print(f"frame {binary_count}: {len(data)} bytes, valid_png={ok}" +
                  (f", +{intervals[-1]:.1f}ms since last" if intervals else ""))
            if not ok:
                print("  !! not a valid PNG frame")
            binary_count += 1
        if intervals:
            avg = sum(intervals) / len(intervals)
            print(f"\n{len(intervals)} intervals, avg {avg:.1f}ms ({1000/avg:.1f} fps), "
                  f"min {min(intervals):.1f}ms, max {max(intervals):.1f}ms")
        if sizes:
            print(f"frame size: avg {sum(sizes)//len(sizes)} bytes, "
                  f"min {min(sizes)}, max {max(sizes)}")


if __name__ == "__main__":
    asyncio.run(main())
