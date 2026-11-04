# SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
"""
Fake Sony OAuth endpoint for testing "ps5view account-id".

POST /2.0/oauth/token with code=<code>:
  good       -> 200 {"access_token": "tok-good", ...}
  expired    -> 400 {"error": "invalid_grant", ...}
  toomany    -> 429 {"error": "too_many_requests", "error_code": 4177, ...}
  html       -> 403 HTML page
GET /2.0/oauth/token/tok-good -> 200 {"user_id": USER_ID, "online_id": "Tester"}
"""

import json
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs

USER_ID = "6543210987654321098"


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def _send(self, status, obj=None, raw=None, ctype="application/json"):
        data = raw if raw is not None else json.dumps(obj).encode()
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length).decode()
        self.server.requests.append(("POST", self.path, body, self.headers.get("Authorization", "")))
        if self.path != "/2.0/oauth/token":
            return self._send(404, {"error": "not_found"})
        q = parse_qs(body)
        code = q.get("code", [""])[0]
        if code == "good":
            return self._send(200, {"access_token": "tok-good", "token_type": "bearer", "expires_in": 3599})
        if code == "expired":
            return self._send(400, {"error": "invalid_grant", "error_description": "Invalid authorization code",
                                    "error_code": 4165})
        if code == "toomany":
            return self._send(429, {"error": "too_many_requests", "error_description": "Too many requests",
                                    "error_code": 4177})
        if code == "html":
            return self._send(403, raw=b"<html><body>Access denied</body></html>", ctype="text/html")
        return self._send(400, {"error": "invalid_request", "error_description": "bad code"})

    def do_GET(self):
        self.server.requests.append(("GET", self.path, "", self.headers.get("Authorization", "")))
        if self.path == "/2.0/oauth/token/tok-good":
            return self._send(200, {"user_id": USER_ID, "online_id": "Tester", "country_code": "DE"})
        return self._send(401, {"error": "invalid_token"})


class FakePSNAuth:
    def __init__(self):
        self.httpd = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.httpd.requests = []
        self.thread = threading.Thread(target=self.httpd.serve_forever, daemon=True)

    @property
    def token_url(self):
        return "http://127.0.0.1:%d/2.0/oauth/token" % self.httpd.server_address[1]

    @property
    def requests(self):
        return self.httpd.requests

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *exc):
        self.httpd.shutdown()
        self.httpd.server_close()


def expected_account_id():
    import base64
    return base64.b64encode(int(USER_ID).to_bytes(8, "little")).decode()


if __name__ == "__main__":
    with FakePSNAuth() as f:
        print(f.token_url, expected_account_id())
        input("Enter to stop")
