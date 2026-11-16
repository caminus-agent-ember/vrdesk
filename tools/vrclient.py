#!/usr/bin/env python3
"""Minimal vrdesk client: connects to the bridge, receives frames, sends input.

Used by the end-to-end test and as a debugging tool for the headset protocol.
"""
import argparse
import socket
import struct
import sys
import time
import zlib

MSG_FRAME, MSG_POINTER, MSG_KEY, MSG_SCROLL = 1, 2, 3, 4
FRAME_FLAG_ZLIB = 0x01

# Per-connection decode stats, printed by --stats so the e2e test can assert that
# compression is actually on the wire and that it round-trips to the raw bytes.
STATS = {"frames": 0, "zlib_frames": 0, "raw_frames": 0,
         "wire_bytes": 0, "raw_bytes": 0, "inflate_errors": 0}


def recv_exact(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("bridge closed the connection")
        buf += chunk
    return buf


def recv_frame(sock):
    """Return (output_id, width, height, stride, payload) or None.

    Header: u8 type | u16 oid | u32 w | u32 h | u32 stride | u8 flags | u32 payload_len.
    flags bit0 = payload is zlib-deflated; the payload always inflates to stride*height
    bytes of BGRA rows, so callers see the same buffer either way.
    """
    hdr = recv_exact(sock, 1)
    if hdr[0] != MSG_FRAME:
        raise ValueError(f"unexpected message type {hdr[0]}")
    oid, w, h, stride, flags, plen = struct.unpack("<HIIIBI", recv_exact(sock, 19))
    payload = recv_exact(sock, plen)
    STATS["frames"] += 1
    STATS["wire_bytes"] += plen
    want = stride * h
    if flags & FRAME_FLAG_ZLIB:
        STATS["zlib_frames"] += 1
        try:
            payload = zlib.decompress(payload)
        except zlib.error as e:
            STATS["inflate_errors"] += 1
            raise ValueError(f"zlib decode failed: {e}") from e
        if len(payload) != want:
            STATS["inflate_errors"] += 1
            raise ValueError(f"inflated {len(payload)} bytes, expected {want}")
    else:
        STATS["raw_frames"] += 1
        if len(payload) != want:
            raise ValueError(f"raw payload {len(payload)} bytes, expected {want}")
    STATS["raw_bytes"] += len(payload)
    return oid, w, h, stride, payload


def send_pointer(sock, oid, x, y, buttons=0, action=0):
    sock.sendall(struct.pack("<BHffBB", MSG_POINTER, oid, x, y, buttons, action))


def send_scroll(sock, oid, dx, dy):
    sock.sendall(struct.pack("<BHff", MSG_SCROLL, oid, dx, dy))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=5000)
    ap.add_argument("--frames", type=int, default=3, help="frames to receive then exit")
    ap.add_argument("--save", help="save a frame as PPM")
    ap.add_argument("--save-output", type=int, default=None,
                    help="only save frames from this output id (default: any frame)")
    ap.add_argument("--click", help="oid:x:y to click after the first frame")
    ap.add_argument("--timeout", type=float, default=10.0)
    ap.add_argument("--stats", action="store_true",
                    help="print 'CLIENT STATS <k=v ...>' line after the run (used by run_e2e.sh)")
    args = ap.parse_args()

    sock = socket.create_connection((args.host, args.port), timeout=args.timeout)
    sock.settimeout(args.timeout)
    got = []
    last = {}
    for i in range(args.frames):
        try:
            oid, w, h, stride, payload = recv_frame(sock)
        except (ConnectionError, OSError) as e:
            # The bridge drops the connection once it has served us; if we already have the
            # frame we came for, that is a normal end of stream, not a failure.
            if last:
                print(f"stream ended after {len(got)} frames ({e})")
                break
            raise
        got.append((oid, w, h, stride, len(payload)))
        print(f"frame {i}: output={oid} {w}x{h} stride={stride} bytes={len(payload)}")
        if i == 0 and args.click:
            o, x, y = args.click.split(":")
            send_pointer(sock, int(o), float(x), float(y), buttons=1, action=1)
            time.sleep(0.05)
            send_pointer(sock, int(o), float(x), float(y), buttons=0, action=2)
            print(f"sent click at output {o} ({x},{y})")
        # Keep the LAST frame seen for each output, then write the wanted one after
        # the loop. Saving inline meant a later frame from the other monitor could
        # overwrite (or be) the only file, which made the test flaky.
        if args.save and (args.save_output is None or oid == args.save_output):
            last[oid] = (w, h, stride, payload)
    sock.close()

    for oid, (w, h, stride, payload) in last.items():
        if True:
            # BGRA -> RGB PPM
            rgb = bytearray(w * h * 3)
            for row in range(h):
                src = row * stride
                dst = row * w * 3
                for col in range(w):
                    b, g, r = payload[src + col * 4], payload[src + col * 4 + 1], payload[src + col * 4 + 2]
                    rgb[dst + col * 3] = r
                    rgb[dst + col * 3 + 1] = g
                    rgb[dst + col * 3 + 2] = b
            path = args.save if args.save_output is None else f"{args.save}.out{oid}.ppm"
            with open(path, "wb") as f:
                f.write(b"P6\n%d %d\n255\n" % (w, h))
                f.write(bytes(rgb))
            print(f"saved {path}")
    if args.stats:
        ratio = (STATS["raw_bytes"] / STATS["wire_bytes"]) if STATS["wire_bytes"] else 0.0
        print("CLIENT STATS " + " ".join(f"{k}={v}" for k, v in STATS.items()) + f" ratio={ratio:.2f}")
    sock.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
