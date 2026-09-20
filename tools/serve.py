#!/usr/bin/env python3
"""Tile server for development: HTTP/1.1 keep-alive (Python's default http.server is
HTTP/1.0 and closes after every tile, which costs the ESP32 a TCP handshake per tile).

    .venv/bin/python serve.py [--port 8000] [--dir out]

Serves <dir>/<region>/<z>/<x>/<y>.bin; anything missing is a fast 404.
"""
import argparse, os, sys
from http.server import ThreadingHTTPServer, SimpleHTTPRequestHandler

class Handler(SimpleHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    timeout = 15                       # drop idle keep-alive connections; a stuck client must not pin a thread forever
    def log_message(self, fmt, *args):
        if os.environ.get("TILE_LOG"): sys.stderr.write("%s %s\n" % (self.address_string(), fmt % args))
    def end_headers(self):
        self.send_header("Cache-Control", "public, max-age=86400")
        super().end_headers()

ap = argparse.ArgumentParser(); ap.add_argument("--port", type=int, default=8000); ap.add_argument("--dir", default="out")
a = ap.parse_args()
os.chdir(a.dir)
ThreadingHTTPServer.daemon_threads = True
ThreadingHTTPServer.allow_reuse_address = True
srv = ThreadingHTTPServer(("0.0.0.0", a.port), Handler)
print(f"serving {os.getcwd()} on :{a.port} (HTTP/1.1 keep-alive)", file=sys.stderr)
srv.serve_forever()
