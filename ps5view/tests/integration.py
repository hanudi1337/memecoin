# SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
"""
Integration tests: run the real ps5view binary against a fake PS5 and a fake
Sony OAuth endpoint (both on 127.0.0.1) and check exit codes and messages.

Usage: python3 integration.py <path to ps5view executable>
"""

import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from fake_ps5 import FakePS5  # noqa: E402
from fake_psn_auth import FakePSNAuth, expected_account_id  # noqa: E402

# readable CI logs on Windows (default would be the ANSI code page)
for stream in (sys.stdout, sys.stderr):
    try:
        stream.reconfigure(encoding="utf-8", errors="replace")
    except AttributeError:
        pass

EXE = os.path.abspath(sys.argv[1])
TMP = tempfile.mkdtemp(prefix="ps5view-it-")
CONFIG = os.path.join(TMP, "config.json")

ENV = dict(os.environ)
ENV.update({
    "HOME": TMP, "APPDATA": TMP, "LOCALAPPDATA": TMP, "XDG_CONFIG_HOME": TMP, "XDG_STATE_HOME": TMP,
    "SDL_VIDEODRIVER": "dummy", "SDL_AUDIODRIVER": "dummy",
})

failures = []


def run(*args, stdin=None, env=None, timeout=120):
    t = time.time()
    p = subprocess.run([EXE, "--no-log-file", "--config", CONFIG, *args], env=env or ENV, input=stdin,
                       capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=timeout)
    p.elapsed = time.time() - t
    p.out = p.stdout + p.stderr
    return p


def check(name, cond, p=None):
    if cond:
        print("ok   - %s" % name)
    else:
        print("FAIL - %s" % name)
        if p is not None:
            print("       rc=%s\n%s" % (p.returncode, "\n".join("       | " + l for l in p.out.splitlines())))
        failures.append(name)


def write_profile(**stream):
    cfg = {
        "format_version": 1,
        "default_profile": "fake",
        "profiles": [{
            "name": "fake", "host": "127.0.0.1", "console": "ps5", "target": 1000100, "nickname": "PS5-FAKE",
            "account_id": "ypOR0bknzlo=", "regist_key": "61626364313233340000000000000000",
            "rp_key": "00112233445566778899aabbccddeeff", "rp_key_type": 0, "stream": stream,
        }],
    }
    with open(CONFIG, "w", encoding="utf-8") as f:
        json.dump(cfg, f)


def main():
    # ---- general ----
    p = run("--version")
    check("version", p.returncode == 0 and "chiaki-ng" in p.out, p)
    p = run("nonsense")
    check("unknown command -> usage error", p.returncode == 1, p)
    p = run("stream", "--bogus")
    check("unknown option -> usage error", p.returncode == 1 and "--bogus" in p.out, p)
    p = run("register", "--help")
    check("output is UTF-8 (umlauts)", p.returncode == 0 and "Schlüssel" in p.out, p)

    # ---- discover ----
    with FakePS5(state="standby"):
        # also queries the PS4 port 987 (nothing listens there -> ICMP unreachable,
        # which must not kill discovery on Windows)
        p = run("discover", "--host", "127.0.0.1", "--timeout", "2")
        check("discover finds fake console in standby", p.returncode == 0 and "PS5-FAKE" in p.out and "Ruhemodus" in p.out, p)
    p = run("discover", "--host", "127.0.0.1", "--timeout", "1")
    check("discover without console -> network error + hint", p.returncode == 3 and "Was du tun kannst" in p.out, p)

    # ---- account-id ----
    with FakePSNAuth() as auth:
        env = dict(ENV, PS5VIEW_PSN_TOKEN_URL=auth.token_url)
        p = run("account-id", "--redirect-url",
                "https://remoteplay.dl.playstation.net/remoteplay/redirect?code=good&cid=1", env=env)
        check("account-id success", p.returncode == 0 and expected_account_id() in p.out, p)
        p = run("account-id", "--no-browser", env=env,
                stdin="https://remoteplay.dl.playstation.net/remoteplay/redirect?code=good\n")
        check("account-id via stdin", p.returncode == 0 and expected_account_id() in p.out, p)
        p = run("account-id", "--redirect-url", "expired", env=env)
        check("account-id expired code", p.returncode == 4 and "abgelaufen" in p.out, p)
        p = run("account-id", "--redirect-url", "toomany", env=env)
        check("account-id too_many_requests 4177", p.returncode == 4 and "4177" in p.out and "too_many_requests" in p.out, p)
        p = run("account-id", "--redirect-url", "https://x/redirect?error=access_denied", env=env)
        check("account-id error parameter", p.returncode == 4 and "access_denied" in p.out, p)

    # ---- register ----
    if os.path.exists(CONFIG):
        os.remove(CONFIG)
    reg = ["register", "--host", "127.0.0.1", "--account-id", "ypOR0bknzlo=", "--pin", "12345678"]
    with FakePS5(regist_reason="80108b09"):
        p = run(*reg)
        check("register wrong PIN (0x80108b09)", p.returncode == 4 and "0x80108b09" in p.out and "PIN" in p.out, p)
    with FakePS5(regist_reason="80108b02"):
        p = run(*reg)
        check("register wrong account (0x80108b02)", p.returncode == 4 and "Account-ID" in p.out, p)
    with FakePS5(regist_answer_search=False):
        p = run(*reg)
        check("register: no answer to search", p.returncode == 3 and "UDP 9295" in p.out, p)
    with FakePS5(state="standby"):
        p = run(*reg)
        check("register: console in standby", p.returncode == 5 and "Ruhemodus" in p.out, p)
    p = run("register", "--host", "127.0.0.1", "--account-id", "nope", "--pin", "1")
    check("register: input validation", p.returncode == 1 and "Account-ID" in p.out and "8 Ziffern" in p.out, p)
    check("register failures do not create a config", not os.path.exists(CONFIG))

    # ---- stream ----
    write_profile()
    with FakePS5(session_reason="80108b12") as fake:
        p = run("stream", "--no-audio")
        check("stream 0x80108b12 -> Remote Play disabled, no retry",
              p.returncode == 5 and "0x80108b12" in p.out and "Remote Play aktivieren" in p.out
              and len(fake.session_requests) == 1, p)
        req = fake.session_requests[0] if fake.session_requests else ""
        check("session request carries the regist key", "RP-Registkey: 6162636431323334" in req)
    with FakePS5(state="standby"):
        p = run("stream", "--no-audio")
        check("stream: standby without --wake", p.returncode == 5 and "--wake" in p.out, p)
    with FakePS5(state="standby", session_reason="80108b12", wake_delay=1.0, credential=0xabcd1234) as fake:
        p = run("stream", "--no-audio", "--wake")
        check("stream --wake wakes the console with the right credential",
              fake.wakeups >= 1 and fake.wakeup_credentials[0] == str(0xabcd1234) and "0x80108b12" in p.out, p)
    with FakePS5(session_reason="80108b10") as fake:
        p = run("stream", "--no-audio", "--retries", "2")
        check("stream: in use -> slow retry, then give up",
              p.returncode == 5 and len(fake.session_requests) == 2 and p.elapsed >= 9 and "Aufgegeben" in p.out, p)
    p = run("stream", "--no-audio", "--retries", "2")
    check("stream: nothing listening -> refused, bounded retries",
          p.returncode == 3 and "9295" in p.out and "Aufgegeben nach 2" in p.out, p)
    # ---- audio ----
    p = run("list-audio")
    check("list-audio lists the dummy device", p.returncode == 0 and "System audio output device" in p.out, p)
    p = run("list-audio", "--test-tone", "system audio", "--seconds", "1")
    check("list-audio --test-tone plays", p.returncode == 0 and "Testton beendet" in p.out, p)
    p = run("stream", "--audio-device", "CABLE Input")
    check("stream: missing audio device -> no fallback, device error",
          p.returncode == 6 and "NICHT auf ein anderes" in p.out and "System audio output device" in p.out, p)
    with FakePS5(session_reason="80108b12"):
        p = run("stream", "--audio-device", "system", "--save")
    with open(CONFIG, encoding="utf-8") as f:
        saved = json.load(f)["profiles"][0]["stream"]
    check("stream --save stores the exact audio device name",
          saved.get("audio_device") == "System audio output device" and "0x80108b12" in p.out, p)

    # ---- check ----
    with FakePS5():
        p = run("check")
        check("check with reachable fake console",
              p.returncode == 0 and "TCP 9295 ist erreichbar" in p.out and "PS5-FAKE" in p.out, p)
    p = run("check")
    check("check without console reports the failure", p.returncode == 3 and "[FAIL]" in p.out, p)

    # ---- controller modes ----
    with FakePS5(session_reason="80108b12"):
        p = run("stream", "--no-audio", "--controller", "none")
        check("stream --controller none runs (experimental view-only mode)",
              p.returncode == 5 and "Experiment --controller none" in p.out and "0x80108b12" in p.out, p)
    p = run("stream", "--controller", "xbox")
    check("stream --controller invalid -> usage error", p.returncode == 1 and "ds4, dualsense oder none" in p.out, p)

    p = run("stream", "--profile", "doesnotexist")
    check("stream: unknown profile", p.returncode == 2 and "fake" in p.out, p)
    p = run("stream", "--fps", "45")
    check("stream: invalid fps", p.returncode == 1, p)

    shutil.rmtree(TMP, ignore_errors=True)
    print("\n%d failure(s)" % len(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
