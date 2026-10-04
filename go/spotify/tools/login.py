#!/usr/bin/env python3
"""Provision a Spotify PKCE refresh token; the r2 client runs independently."""
import argparse
import base64
import hashlib
import http.server
import json
import os
from pathlib import Path
import secrets
import tempfile
import time
import urllib.parse
import urllib.request
import webbrowser


def provision(client_id, output, port, open_browser=True):
    redirect = f"http://127.0.0.1:{port}/callback"
    state = secrets.token_urlsafe(32)
    verifier = secrets.token_urlsafe(64)
    challenge = base64.urlsafe_b64encode(hashlib.sha256(verifier.encode()).digest()).rstrip(b"=").decode()
    result = {}

    class Callback(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass  # Authorization codes must not appear in logs.

        def do_GET(self):
            url = urllib.parse.urlsplit(self.path)
            query = urllib.parse.parse_qs(url.query)
            if url.path != "/callback":
                self.send_error(404)
                return
            if not secrets.compare_digest(query.get("state", [""])[0], state):
                self.send_error(400, "Invalid authorization state")
                return
            if "error" in query:
                result["error"] = "Spotify sign-in was declined."
            elif query.get("code"):
                result["code"] = query["code"][0]
            else:
                self.send_error(400, "Missing authorization code")
                return
            body = b"Sign-in received. Return to the terminal."
            self.send_response(200)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    params = dict(client_id=client_id, response_type="code", redirect_uri=redirect,
                  scope="playlist-read-private playlist-read-collaborative", state=state,
                  code_challenge_method="S256", code_challenge=challenge)
    url = "https://accounts.spotify.com/authorize?" + urllib.parse.urlencode(params)
    with http.server.HTTPServer(("127.0.0.1", port), Callback) as server:
        server.timeout = 1
        print(f"Register this exact redirect URI in your Spotify app: {redirect}")
        print(f"Open to sign in:\n{url}")
        if open_browser:
            webbrowser.open(url)
        deadline = time.monotonic() + 180
        while not result and time.monotonic() < deadline:
            server.handle_request()
    if not result:
        raise RuntimeError("Sign-in timed out after three minutes.")
    if "error" in result:
        raise RuntimeError(result["error"])
    payload = urllib.parse.urlencode(dict(grant_type="authorization_code", code=result["code"],
                        redirect_uri=redirect, client_id=client_id, code_verifier=verifier)).encode()
    request = urllib.request.Request("https://accounts.spotify.com/api/token", data=payload,
                                    headers={"Content-Type": "application/x-www-form-urlencoded"})
    with urllib.request.urlopen(request, timeout=30) as reply:
        tokens = json.load(reply)
    if not tokens.get("refresh_token"):
        raise RuntimeError("Spotify did not return a refresh token.")
    config = json.loads(output.read_text()) if output.exists() else {}
    config.update(client_id=client_id, refresh_token=tokens["refresh_token"])
    config.pop("access_token", None)
    # Keep local file mappings. Replace atomically; never print credentials.
    with tempfile.NamedTemporaryFile(mode="w", dir=output.parent, delete=False) as tmp:
        name = tmp.name
        try:
            os.chmod(name, 0o600)
            json.dump(config, tmp, indent=2)
            tmp.write("\n")
            tmp.flush()
            os.fsync(tmp.fileno())
        except BaseException:
            os.unlink(name)
            raise
    try:
        os.replace(name, output)
    finally:
        if os.path.exists(name):
            os.unlink(name)
    print(f"Saved credentials to {output}. Copy this file to /mnt/fat/SPOTIFY.CFG or the boot-media config path.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--client-id", required=True)
    parser.add_argument("--output", type=Path, default=Path("SPOTIFY.CFG"))
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--no-browser", action="store_true")
    args = parser.parse_args()
    if not 1 <= args.port <= 65535:
        parser.error("port must be in 1..65535")
    provision(args.client_id, args.output, args.port, not args.no_browser)


if __name__ == "__main__":
    main()
