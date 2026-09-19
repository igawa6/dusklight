#!/usr/bin/env python3
"""Throwaway test client for the phone-companion-spike WS server
(DUSK_PHONE_SPIKE). Connects, receives N binary frames, verifies each is a
real PNG, and reports inter-frame timing. Not part of the shipped feature —
delete along with the rest of the spike once it's served its purpose."""
import asyncio
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
        for i in range(n_frames):
            data = await ws.recv()
            now = time.monotonic()
            ok = isinstance(data, (bytes, bytearray)) and data[:8] == PNG_MAGIC
            if last_t is not None:
                intervals.append((now - last_t) * 1000.0)
            last_t = now
            sizes.append(len(data))
            print(f"frame {i}: {len(data)} bytes, valid_png={ok}" +
                  (f", +{intervals[-1]:.1f}ms since last" if intervals else ""))
            if not ok:
                print("  !! not a valid PNG frame")
        if intervals:
            avg = sum(intervals) / len(intervals)
            print(f"\n{len(intervals)} intervals, avg {avg:.1f}ms ({1000/avg:.1f} fps), "
                  f"min {min(intervals):.1f}ms, max {max(intervals):.1f}ms")
        if sizes:
            print(f"frame size: avg {sum(sizes)//len(sizes)} bytes, "
                  f"min {min(sizes)}, max {max(sizes)}")


if __name__ == "__main__":
    asyncio.run(main())
