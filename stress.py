#!/usr/bin/env python3
import argparse
import socket
import struct
from concurrent.futures import ThreadPoolExecutor, as_completed

def one_request(host: str, port: int, path: str, mode: str, peek: int, timeout: float) -> bool:
    """
    mode:
      - 'fin' -> orderly close (FIN) => server likely sees SIGPIPE/EPIPE on write
      - 'rst' -> abortive close (RST via SO_LINGER) => server sees ECONNRESET
    """
    s = None
    try:
        s = socket.create_connection((host, port), timeout=timeout)
        s.settimeout(timeout)

        req = f"GET {path} HTTP/1.0\r\nHost: {host}\r\n\r\n".encode()
        s.sendall(req)

        # Optionally read a few bytes so the server definitely starts sending the body.
        if peek > 0:
            try:
                _ = s.recv(peek)
            except Exception:
                pass

        if mode == "rst":
            # SO_LINGER: onoff=1, linger=0 -> send RST on close
            s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))

        s.close()
        return True
    except Exception:
        try:
            if s is not None:
                s.close()
        except Exception:
            pass
        return False

def main():
    ap = argparse.ArgumentParser(description="Trigger FIN (EPIPE/SIGPIPE) or RST (ECONNRESET)")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=80)
    ap.add_argument("--path", default="/static/resume.pdf", help="Use a large file so the server enters send_chunk().")
    ap.add_argument("--mode", choices=["fin", "rst", "mix"], default="fin",
                    help="'fin' for EPIPE/SIGPIPE; 'rst' for ECONNRESET; 'mix' alternates per request.")
    ap.add_argument("-n", "--requests", type=int, default=2000)
    ap.add_argument("-c", "--concurrency", type=int, default=64)
    ap.add_argument("--peek", type=int, default=16, help="Bytes to read before closing (0..256).")
    ap.add_argument("--timeout", type=float, default=1.0)
    args = ap.parse_args()

    # Build the per-request mode list
    if args.mode == "mix":
        modes = ["fin" if i % 2 == 0 else "rst" for i in range(args.requests)]
    else:
        modes = [args.mode] * args.requests

    ok = err = 0
    with ThreadPoolExecutor(max_workers=args.concurrency) as ex:
        futs = [
            ex.submit(one_request, args.host, args.port, args.path, m, args.peek, args.timeout)
            for m in modes
        ]
        for f in as_completed(futs):
            (ok := ok + 1) if f.result() else (err := err + 1)

    print(f"done: ok={ok} err={err}")

if __name__ == "__main__":
    main()
