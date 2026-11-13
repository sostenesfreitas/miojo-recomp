"""scripts/team_chat.py, game-free and offline: against a stand-in Matrix homeserver on 127.0.0.1, single sign-on comes
back to the one-time listener (keeping the token when the browser closes the tab first) and the session is saved
readable by the user only; the room is joined once; a post carries the robot prefix once (also when the text starts
with one); a second post within the minimum interval and the same text twice are refused with exit code 3 (--force
skips the interval, never the repeat); read prints the room oldest first; a revoked session asks for a new login;
logout forgets the token.

    python -I -B tests/test_team_chat.py
"""
import contextlib
import http.server
import importlib.util
import io
import json
import os
import shutil
import sys
import tempfile
import threading
import urllib.parse
import urllib.request
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPT = os.path.join(ROOT, "scripts", "team_chat.py")
ALIAS = "#agent-updates:magicbean.dev"


class Homeserver:
    """The few client-server API endpoints the script uses, with one account and one room."""

    def __init__(self):
        self.messages, self.joins, self.revoked, self.default_agent = [], 0, False, False
        outer = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def reply(self, code, body, headers=()):
                data = json.dumps(body).encode("utf-8")
                self.send_response(code)
                for name, value in headers:
                    self.send_header(name, value)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def body(self):
                size = int(self.headers.get("Content-Length") or 0)
                return json.loads(self.rfile.read(size) or b"{}")

            def parse_request(self):
                # like Cloudflare in front of the real server: urllib's default User-Agent is refused
                ok = super().parse_request()
                if ok and self.headers.get("User-Agent", "").startswith("Python-urllib") \
                        and "/login/sso/redirect" not in self.path:  # (that one is the test's "browser")
                    outer.default_agent = True
                return ok

            def authorized(self):
                if outer.revoked or self.headers.get("Authorization") != "Bearer secret-token":
                    self.reply(401, {"errcode": "M_UNKNOWN_TOKEN", "error": "Unknown access token"})
                    return False
                return True

            def do_GET(self):
                url = urllib.parse.urlsplit(self.path)
                path = urllib.parse.unquote(url.path)
                if path == "/_matrix/client/v3/login":
                    return self.reply(200, {"flows": [{"type": "m.login.sso"}, {"type": "m.login.token"}]})
                if path == "/_matrix/client/v3/login/sso/redirect":
                    target = urllib.parse.parse_qs(url.query)["redirectUrl"][0]
                    assert target.startswith("http://127.0.0.1:"), target
                    return self.reply(302, {}, [("Location", target + "?loginToken=one-time")])
                if path == "/_matrix/client/v3/account/whoami" and self.authorized():
                    return self.reply(200, {"user_id": "@alice:magicbean.dev", "device_id": "DEV"})
                if path == "/_matrix/client/v3/rooms/!room:magicbean.dev/messages" and self.authorized():
                    query = urllib.parse.parse_qs(url.query)
                    assert query["dir"] == ["b"]
                    chunk = [{"type": "m.room.message", "sender": "@alice:magicbean.dev", "origin_server_ts": 0,
                              "content": {"msgtype": "m.text", "body": m}} for m in reversed(outer.messages)]
                    chunk.insert(0, {"type": "m.room.member", "content": {"membership": "join"}})
                    return self.reply(200, {"chunk": chunk})

            def do_POST(self):
                path = urllib.parse.unquote(urllib.parse.urlsplit(self.path).path)
                body = self.body()
                if path == "/_matrix/client/v3/login":
                    assert body == {"type": "m.login.token", "token": "one-time",
                                    "initial_device_display_name": "Hookshot agent"}, body
                    return self.reply(200, {"access_token": "secret-token", "user_id": "@alice:magicbean.dev",
                                            "device_id": "DEV"})
                if path == "/_matrix/client/v3/join/" + ALIAS and self.authorized():
                    outer.joins += 1
                    return self.reply(200, {"room_id": "!room:magicbean.dev"})
                if path == "/_matrix/client/v3/logout" and self.authorized():
                    return self.reply(200, {})
                self.reply(404, {"errcode": "M_UNRECOGNIZED"})

            def do_PUT(self):
                path = urllib.parse.unquote(urllib.parse.urlsplit(self.path).path)
                if path.startswith("/_matrix/client/v3/rooms/!room:magicbean.dev/send/m.room.message/"):
                    if self.authorized():
                        body = self.body()
                        assert body["msgtype"] == "m.text"
                        outer.messages.append(body["body"])
                        self.reply(200, {"event_id": "$%d" % len(outer.messages)})
                    return
                self.reply(404, {"errcode": "M_UNRECOGNIZED"})

            def log_message(self, *args):
                pass

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.url = "http://127.0.0.1:%d" % self.server.server_address[1]
        threading.Thread(target=self.server.serve_forever, daemon=True).start()


def run(chat, *argv, stdin=""):
    out, err = io.StringIO(), io.StringIO()
    with mock.patch("sys.stdin", io.StringIO(stdin)), contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = chat.main(list(argv))
    return code, out.getvalue(), err.getvalue()


def main():
    spec = importlib.util.spec_from_file_location("team_chat", SCRIPT)
    chat = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(chat)
    home = Homeserver()
    folder = tempfile.mkdtemp(prefix="team_chat_")
    try:
        with mock.patch.dict(os.environ, {"HOOKSHOT_TEAM_CHAT_DIR": folder}):
            code, _, err = run(chat, "post", "too early")
            assert code == 1 and "not signed in" in err, err

            # the "browser": follows the sign-in link, whose redirect lands on the script's listener
            def browser(url):
                threading.Thread(target=lambda: urllib.request.urlopen(url, timeout=10).read(), daemon=True).start()
                return True

            with mock.patch.object(chat.webbrowser, "open", browser):
                code, out, err = run(chat, "login", "--server", home.url)
            assert code == 0 and "signed in as @alice:magicbean.dev" in out, (out, err)
            path = os.path.join(folder, "team_chat.json")
            saved = json.load(open(path, encoding="utf-8"))
            assert saved["access_token"] == "secret-token" and saved["base_url"] == home.url
            if os.name != "nt":
                assert os.stat(path).st_mode & 0o777 == 0o600, oct(os.stat(path).st_mode)
            assert not os.path.exists(path + ".tmp")

            assert run(chat, "whoami")[1].strip() == "@alice:magicbean.dev"

            # the browser may close the tab before the "Signed in" page is sent (WinError 10053 on Windows): the token
            # that came in is kept, and the listener neither prints a traceback nor waits out its timeout
            port, ready, got = [], threading.Event(), []
            aborted = ConnectionAbortedError(10053, "An established connection was aborted")
            with mock.patch.object(http.server.BaseHTTPRequestHandler, "end_headers", side_effect=aborted), \
                    contextlib.redirect_stderr(io.StringIO()) as noise:
                waiter = threading.Thread(target=lambda: got.append(chat.wait_for_login_token(port, ready, 5)))
                waiter.start()
                ready.wait(5)
                with contextlib.suppress(OSError):
                    urllib.request.urlopen("http://127.0.0.1:%d/?loginToken=abc" % port[0], timeout=5).read()
                waiter.join(10)
            assert got == ["abc"], (got, noise.getvalue())
            assert "Traceback" not in noise.getvalue(), noise.getvalue()

            now = [1_000_000.0]
            with mock.patch.object(chat.time, "time", lambda: now[0]):
                code, out, _ = run(chat, "post", "Claimed #123: walls missing on Intel Arc")
                assert code == 0 and "posted" in out
                assert home.messages == ["\N{ROBOT FACE} Claimed #123: walls missing on Intel Arc"]

                now[0] += 10 * 60
                code, _, err = run(chat, "post", "PR #124 opened")
                assert code == 3 and "next can go out in 50 minutes" in err, err
                assert "would go out" not in run(chat, "status")[1]

                code, _, err = run(chat, "post", "  Claimed #123:  walls missing on Intel Arc\n", "--force")
                assert code == 3 and "same text" in err, err

                code, _, _ = run(chat, "post", "--force", stdin="PR #124 opened, asked for by the user\n")
                assert code == 0 and len(home.messages) == 2

                now[0] += 61 * 60
                assert "would go out" in run(chat, "status")[1]
                code, _, _ = run(chat, "post", "\N{ROBOT FACE} \N{ROBOT FACE}PR #124 merged")
                assert code == 0 and home.messages[2] == "\N{ROBOT FACE} PR #124 merged", "one robot, however many sent"

                saved = json.load(open(path, encoding="utf-8"))
                saved["min_interval_minutes"] = 120
                chat.save(saved)
                now[0] += 90 * 60
                assert run(chat, "post", "next topic")[0] == 3, "min_interval_minutes is read from the config"

            assert home.joins == 1, "the room is joined once, then its ID comes from the config"

            code, out, _ = run(chat, "read", "--limit", "5")
            lines = out.strip().splitlines()
            assert code == 0 and len(lines) == 3 and lines[0].endswith("Claimed #123: walls missing on Intel Arc")
            assert lines[2].endswith("PR #124 merged") and "@alice:magicbean.dev" in lines[2], lines

            # into a pipe on Windows, stdout is cp1252, which has no robot: read used to stop with a UnicodeEncodeError
            pipe = io.TextIOWrapper(io.BytesIO(), encoding="cp1252")
            with contextlib.redirect_stdout(pipe):
                code = chat.main(["read", "--limit", "5"])
            pipe.flush()
            text = pipe.buffer.getvalue().decode("utf-8")
            assert code == 0 and pipe.encoding == "utf-8" and text.count("\N{ROBOT FACE}") == 3, text

            code, _, err = run(chat, "post", "   ")
            assert code == 1 and "nothing to post" in err

            home.revoked = True
            code, _, err = run(chat, "whoami")
            assert code == 1 and "no longer valid" in err and "login" in err, err
            home.revoked = False

            assert run(chat, "logout")[0] == 0
            assert "access_token" not in json.load(open(path, encoding="utf-8"))
            assert run(chat, "whoami")[0] == 1

            code, out, _ = run(chat, "login", "--server", home.url, "--access-token-stdin", stdin="secret-token\n")
            assert code == 0 and "@alice:magicbean.dev" in out
            assert not home.default_agent, "every request names the script: Cloudflare refuses Python-urllib"
    finally:
        home.server.shutdown()
        shutil.rmtree(folder, ignore_errors=True)
    print("team_chat: ok")


if __name__ == "__main__":
    main()
