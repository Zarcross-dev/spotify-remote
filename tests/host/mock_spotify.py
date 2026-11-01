#!/usr/bin/env python3
"""Mock of the Spotify Web API / token endpoint used by the host tests."""

import base64
import json
import sys
import threading
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

CLIENT_ID, CLIENT_SECRET = "0123456789abcdef0123456789abcdef", "fedcba9876543210fedcba9876543210"
lock = threading.Lock()
log = []

TRACKS = [
    {"type": "track", "uri": f"spotify:track:t{i}", "name": f"Song {i}", "duration_ms": 200000 + i,
     "artists": [{"name": f"Artist {i}"}, {"name": "Feat"}], "album": {"name": f"Album {i}"}}
    for i in range(5)
]
DEVICES = [
    {"id": "dev1", "name": "Kitchen", "type": "Speaker", "is_active": True, "is_restricted": False,
     "supports_volume": True, "volume_percent": 40},
    {"id": "dev2", "name": "PC", "type": "Computer", "is_active": False, "is_restricted": False,
     "supports_volume": True, "volume_percent": 70},
]

state = {
    "active": True, "device": "dev1", "playing": True, "shuffle": False, "repeat": "off",
    "track": 0, "progress": 1000, "volume": 40, "saved": set(),
    "tokens_issued": 0, "fail_next_401": True, "access": set(),
}


def player_json():
    dev = next(dict(d) for d in DEVICES if d["id"] == state["device"])
    dev["is_active"] = True
    dev["volume_percent"] = state["volume"]
    return {
        "device": dev, "is_playing": state["playing"], "shuffle_state": state["shuffle"],
        "repeat_state": state["repeat"], "progress_ms": state["progress"],
        "item": TRACKS[state["track"]], "context": {"uri": "spotify:playlist:pl1"},
    }


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def send(self, code, obj=None):
        body = b"" if obj is None else json.dumps(obj).encode()
        self.send_response(code)
        if obj is not None:
            self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def err(self, code, msg, reason=None):
        e = {"status": code, "message": msg}
        if reason:
            e["reason"] = reason
        self.send(code, {"error": e})

    def handle_any(self, method):
        u = urllib.parse.urlparse(self.path)
        q = {k: v[0] for k, v in urllib.parse.parse_qs(u.query).items()}
        length = self.headers.get("Content-Length")
        if method in ("PUT", "POST") and length is None:
            return self.send(411, {"error": "length required"})
        body = self.rfile.read(int(length or 0)).decode()

        with lock:
            log.append({"method": method, "path": u.path, "query": q, "body": body})

            # --- test control ---
            if u.path == "/_log":
                return self.send(200, log)
            if u.path == "/_state":
                s = {k: (sorted(v) if isinstance(v, set) else v) for k, v in state.items()}
                return self.send(200, s)
            if u.path == "/_set":
                for k, v in json.loads(body).items():
                    state[k] = v
                return self.send(200, {})

            # --- token endpoint ---
            if u.path == "/api/token":
                auth = self.headers.get("Authorization", "")
                if auth != "Basic " + base64.b64encode(f"{CLIENT_ID}:{CLIENT_SECRET}".encode()).decode():
                    return self.send(400, {"error": "invalid_client", "error_description": "Invalid client"})
                form = {k: v[0] for k, v in urllib.parse.parse_qs(body).items()}
                state["tokens_issued"] += 1
                tok = f"AT{state['tokens_issued']}"
                state["access"].add(tok)
                if form.get("grant_type") == "refresh_token":
                    if form.get("refresh_token") not in ("RT1", "RT2", "RT_NEW"):
                        return self.send(400, {"error": "invalid_grant", "error_description": "Invalid refresh token"})
                    resp = {"access_token": tok, "expires_in": 3600}
                    if form["refresh_token"] == "RT1":
                        resp["refresh_token"] = "RT2"   # rotation
                    return self.send(200, resp)
                if form.get("grant_type") == "authorization_code":
                    if form.get("code") != "goodcode" or form.get("redirect_uri") != "http://127.0.0.1:8888/callback":
                        return self.send(400, {"error": "invalid_grant", "error_description": "Invalid authorization code"})
                    return self.send(200, {"access_token": tok, "expires_in": 3600, "refresh_token": "RT_NEW"})
                return self.send(400, {"error": "unsupported_grant_type"})

            # --- API ---
            auth = self.headers.get("Authorization", "")
            if not auth.startswith("Bearer ") or auth[7:] not in state["access"]:
                return self.err(401, "Invalid access token")
            if state["fail_next_401"]:
                state["fail_next_401"] = False
                state["access"].discard(auth[7:])
                return self.err(401, "The access token expired")

            p = u.path
            if p == "/v1/me/player" and method == "GET":
                return self.send(200, player_json()) if state["active"] else self.send(204)
            if p == "/v1/me/player" and method == "PUT":
                data = json.loads(body)
                state["device"] = data["device_ids"][0]
                state["active"] = True
                if data.get("play"):
                    state["playing"] = True
                return self.send(204)
            if p == "/v1/me/player/devices":
                return self.send(200, {"devices": DEVICES})

            if p.startswith("/v1/me/player/") and not state["active"] and "device_id" not in q:
                return self.err(404, "Player command failed: No active device found", "NO_ACTIVE_DEVICE")

            if p == "/v1/me/player/play":
                if "device_id" in q:
                    state["device"] = q["device_id"]
                    state["active"] = True
                if body:
                    data = json.loads(body)
                    off = data.get("offset", {}).get("uri")
                    if off:
                        state["track"] = next(i for i, t in enumerate(TRACKS) if t["uri"] == off)
                    else:
                        state["track"] = 0
                    state["progress"] = 0
                state["playing"] = True
                return self.send(204)
            if p == "/v1/me/player/pause":
                state["playing"] = False
                return self.send(204)
            if p == "/v1/me/player/next":
                state["track"] = (state["track"] + 1) % len(TRACKS)
                state["progress"] = 0
                return self.send(204)
            if p == "/v1/me/player/previous":
                state["track"] = (state["track"] - 1) % len(TRACKS)
                return self.send(204)
            if p == "/v1/me/player/shuffle":
                state["shuffle"] = q["state"] == "true"
                return self.send(204)
            if p == "/v1/me/player/repeat":
                state["repeat"] = q["state"]
                return self.send(204)
            if p == "/v1/me/player/volume":
                state["volume"] = int(q["volume_percent"])
                return self.send(204)
            if p == "/v1/me/player/seek":
                state["progress"] = int(q["position_ms"])
                return self.send(204)
            if p == "/v1/me/player/queue":
                cur = state["track"]
                return self.send(200, {"currently_playing": TRACKS[cur],
                                       "queue": [TRACKS[(cur + i) % len(TRACKS)] for i in range(1, 4)]})
            if p == "/v1/me/library/contains":
                return self.send(200, [q["uris"] in state["saved"]])
            if p == "/v1/me/library":
                (state["saved"].add if method == "PUT" else state["saved"].discard)(q["uris"])
                return self.send(200)
            if p == "/v1/me/playlists":
                off, lim = int(q.get("offset", 0)), int(q.get("limit", 20))
                total = 30
                items = [{"id": f"pl{i}", "uri": f"spotify:playlist:pl{i}", "name": f"Playlist {i}",
                          "owner": {"display_name": "me"}, "items": {"total": i}}
                         for i in range(off, min(off + lim, total))]
                nxt = "x" if off + lim < total else None
                return self.send(200, {"items": items, "next": nxt, "total": total})
            if p.startswith("/v1/playlists/") and p.endswith("/items"):
                pid = p.split("/")[3]
                if pid == "forbidden":
                    return self.err(403, "Forbidden")
                return self.send(200, {"items": [{"item": t} for t in TRACKS[:3]] + [{"track": TRACKS[3]}],
                                       "next": None})
            return self.err(404, f"unknown endpoint {method} {p}")

    def do_GET(self):
        self.handle_any("GET")

    def do_PUT(self):
        self.handle_any("PUT")

    def do_POST(self):
        self.handle_any("POST")

    def do_DELETE(self):
        self.handle_any("DELETE")


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 18080
    ThreadingHTTPServer(("127.0.0.1", port), H).serve_forever()
