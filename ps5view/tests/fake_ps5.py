# SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
"""
Minimal fake PS5 for testing ps5view without a console.

It implements just enough of the Remote Play handshake to exercise the error
paths of ps5view/libchiaki:

  * UDP 9302  discovery ("SRCH") -> "200 Ok" (ready) or "620 Server Standby"
              wake-up ("WAKEUP")  -> switches standby -> ready after a delay
  * UDP 9295  registration search ("SRC3") -> "RES3" (or silence)
  * TCP 9295  registration request  -> HTTP 403 with RP-Application-Reason
              session request (/sie/ps5/rp/sess/init) -> HTTP 403 with a
              configurable RP-Application-Reason (e.g. 80108b12)

Everything listens on 127.0.0.1 only. Usable standalone:
  python3 fake_ps5.py --state standby --session-reason 80108b12
"""

import argparse
import socket
import threading
import time


class FakePS5:
    def __init__(self, host="127.0.0.1", name="PS5-FAKE", state="ready",
                 session_reason="80108b12", regist_reason="80108b09",
                 regist_answer_search=True, wake_delay=1.0,
                 tcp_mode="http", credential=None):
        self.host = host
        self.name = name
        self.state = state
        self.session_reason = session_reason
        self.regist_reason = regist_reason
        self.regist_answer_search = regist_answer_search
        self.wake_delay = wake_delay
        self.tcp_mode = tcp_mode  # "http" | "refuse" (no TCP listener)
        self.credential = credential
        self.wakeups = 0
        self.wakeup_credentials = []
        self.session_requests = []
        self.regist_requests = 0
        self._stop = threading.Event()
        self._threads = []
        self._socks = []
        self.lock = threading.Lock()

    # ------------------------------------------------------------------
    def start(self):
        disc = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        disc.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        disc.bind((self.host, 9302))
        disc.settimeout(0.2)
        self._socks.append(disc)
        self._spawn(self._discovery_loop, disc)

        reg = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        reg.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        reg.bind((self.host, 9295))
        reg.settimeout(0.2)
        self._socks.append(reg)
        self._spawn(self._regist_search_loop, reg)

        if self.tcp_mode == "http":
            tcp = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            tcp.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            tcp.bind((self.host, 9295))
            tcp.listen(8)
            tcp.settimeout(0.2)
            self._socks.append(tcp)
            self._spawn(self._tcp_loop, tcp)
        return self

    def stop(self):
        self._stop.set()
        for t in self._threads:
            t.join(timeout=2)
        for s in self._socks:
            try:
                s.close()
            except OSError:
                pass

    def __enter__(self):
        return self.start()

    def __exit__(self, *exc):
        self.stop()

    def _spawn(self, fn, *args):
        t = threading.Thread(target=fn, args=args, daemon=True)
        t.start()
        self._threads.append(t)

    # ------------------------------------------------------------------
    def _discovery_loop(self, sock):
        while not self._stop.is_set():
            try:
                data, addr = sock.recvfrom(2048)
            except socket.timeout:
                continue
            except ConnectionResetError:
                # Windows reports an earlier ICMP "port unreachable" this way
                continue
            except OSError:
                return
            text = data.decode("ascii", "replace")
            if text.startswith("SRCH"):
                with self.lock:
                    state = self.state
                if state == "off":
                    continue
                status = "200 Ok" if state == "ready" else "620 Server Standby"
                resp = (
                    "HTTP/1.1 %s\n"
                    "host-id:0123456789AB\n"
                    "host-type:PS5\n"
                    "host-name:%s\n"
                    "host-request-port:997\n"
                    "device-discovery-protocol-version:00030010\n"
                    "system-version:08050001\n"
                    "running-app-name:Testspiel\n"
                    "running-app-titleid:PPSA00000\n" % (status, self.name)
                )
                sock.sendto(resp.encode("ascii") + b"\0", addr)
            elif text.startswith("WAKEUP"):
                cred = None
                for line in text.splitlines():
                    if line.startswith("user-credential:"):
                        cred = line.split(":", 1)[1].strip().strip("\0")
                with self.lock:
                    self.wakeups += 1
                    self.wakeup_credentials.append(cred)
                    accept = self.credential is None or cred == str(self.credential)
                    if accept and self.state == "standby":
                        threading.Timer(self.wake_delay, self._become_ready).start()

    def _become_ready(self):
        with self.lock:
            if self.state == "standby":
                self.state = "ready"

    def _regist_search_loop(self, sock):
        while not self._stop.is_set():
            try:
                data, addr = sock.recvfrom(2048)
            except socket.timeout:
                continue
            except ConnectionResetError:
                # Windows reports an earlier ICMP "port unreachable" this way
                continue
            except OSError:
                return
            if data.startswith(b"SRC3") and self.regist_answer_search:
                sock.sendto(b"RES3\0", addr)

    def _tcp_loop(self, sock):
        while not self._stop.is_set():
            try:
                conn, _ = sock.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            self._spawn(self._handle_tcp, conn)

    def _handle_tcp(self, conn):
        conn.settimeout(3)
        buf = b""
        try:
            while b"\r\n\r\n" not in buf and len(buf) < 65536:
                chunk = conn.recv(4096)
                if not chunk:
                    break
                buf += chunk
            head = buf.split(b"\r\n", 1)[0].decode("ascii", "replace")
            if "/rp/sess/init" in head:
                with self.lock:
                    self.session_requests.append(buf.decode("ascii", "replace"))
                reason = self.session_reason
                body = ("HTTP/1.1 403 Forbidden\r\n"
                        "RP-Application-Reason: %s\r\n"
                        "RP-Version: 1.0\r\n"
                        "Content-Length: 0\r\n\r\n" % reason)
                conn.sendall(body.encode("ascii"))
            elif "/rp/sess/rgst" in head:
                with self.lock:
                    self.regist_requests += 1
                body = ("HTTP/1.1 403 Forbidden\r\n"
                        "RP-Application-Reason: %s\r\n"
                        "Content-Length: 0\r\n\r\n" % self.regist_reason)
                conn.sendall(body.encode("ascii"))
            else:
                conn.sendall(b"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n")
        except OSError:
            pass
        finally:
            time.sleep(0.05)
            conn.close()


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--state", choices=["ready", "standby", "off"], default="ready")
    p.add_argument("--session-reason", default="80108b12")
    p.add_argument("--regist-reason", default="80108b09")
    p.add_argument("--no-regist-search", action="store_true", help="do not answer the registration search")
    p.add_argument("--refuse-tcp", action="store_true", help="no TCP listener on 9295 (connection refused)")
    args = p.parse_args()
    fake = FakePS5(state=args.state, session_reason=args.session_reason, regist_reason=args.regist_reason,
                   regist_answer_search=not args.no_regist_search,
                   tcp_mode="refuse" if args.refuse_tcp else "http")
    fake.start()
    print("fake PS5 on 127.0.0.1 (Ctrl+C to stop)")
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        pass
    fake.stop()


if __name__ == "__main__":
    main()
