"""Post to and read the team's agent channel on Matrix (#agent-updates:magicbean.dev): claims, progress, pull requests and
requests for help, the same trail docs/workflow.md asks for, in a place both people and agents read.

    python -I scripts/team_chat.py login [--server magicbean.dev] [--access-token-stdin] [--no-browser]
    python -I scripts/team_chat.py whoami
    python -I scripts/team_chat.py status [--room ALIAS]
    python -I scripts/team_chat.py read [--limit 20] [--room ALIAS]
    python -I scripts/team_chat.py post "text" [--force] [--room ALIAS]     (or the text on standard input)
    python -I scripts/team_chat.py logout

login signs in with Discord: it prints a link (and opens it unless --no-browser), the browser comes back to a one-time
listener on 127.0.0.1, and the session is saved to the config folder (Linux: $XDG_CONFIG_HOME/hookshot, default
~/.config/hookshot; Windows: %APPDATA%\\Hookshot; HOOKSHOT_TEAM_CHAT_DIR overrides it) as team_chat.json, readable by
you only. The first sign-in creates the account. --access-token-stdin saves a token copied
from Element (Settings, Help & About, Access token) instead.

post refuses, with exit code 3, a message sent less than the minimum interval after the last one (60 minutes; the
config's min_interval_minutes changes it) and the same text twice in a row. --force skips the interval only: use it
when the person you work for asked for the update. status says whether a post would go out now. Messages go out as the
signed-in account, prefixed with a robot so readers can tell an agent's post from a person's.
Standard library only; the guide is .claude/skills/team-updates/SKILL.md.
"""
import argparse
import hashlib
import http.server
import json
import os
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
import webbrowser

SERVER = "magicbean.dev"
ROOM = "#agent-updates:magicbean.dev"
MIN_INTERVAL_MINUTES = 60
PREFIX = "\N{ROBOT FACE} "
DEVICE_NAME = "Hookshot agent"
USER_AGENT = "hookshot-team-chat/1"
LOGIN_TIMEOUT = 300
EXIT_REFUSED = 3


class ChatError(Exception):
    pass


def config_dir():
    if os.environ.get("HOOKSHOT_TEAM_CHAT_DIR"):
        return os.environ["HOOKSHOT_TEAM_CHAT_DIR"]
    if os.name == "nt":
        return os.path.join(os.environ.get("APPDATA") or os.path.expanduser("~"), "Hookshot")
    base = os.environ.get("XDG_CONFIG_HOME") or os.path.join(os.path.expanduser("~"), ".config")
    return os.path.join(base, "hookshot")


def config_path():
    return os.path.join(config_dir(), "team_chat.json")


def load():
    try:
        with open(config_path(), encoding="utf-8") as f:
            return json.load(f)
    except FileNotFoundError:
        return {}


def save(config):
    """Written whole to a file only the user can read, then moved over the old one."""
    os.makedirs(config_dir(), exist_ok=True)
    path = config_path()
    tmp = path + ".tmp"
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w", encoding="utf-8") as f:
        json.dump(config, f, indent=2)
        f.write("\n")
    os.replace(tmp, path)
    if os.name != "nt":
        os.chmod(path, 0o600)


def request(method, url, token=None, body=None, timeout=30):
    data = None if body is None else json.dumps(body).encode("utf-8")
    req = urllib.request.Request(url, data=data, method=method)
    req.add_header("Accept", "application/json")
    # Cloudflare, in front of the server, answers 403 to urllib's default "Python-urllib/3.x"
    req.add_header("User-Agent", USER_AGENT)
    if data is not None:
        req.add_header("Content-Type", "application/json")
    if token:
        req.add_header("Authorization", "Bearer " + token)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            raw = r.read()
    except urllib.error.HTTPError as error:
        try:
            detail = json.loads(error.read().decode("utf-8"))
        except ValueError:
            detail = {}
        code = detail.get("errcode", "HTTP %d" % error.code)
        if error.code == 401 or code in ("M_UNKNOWN_TOKEN", "M_MISSING_TOKEN"):
            raise ChatError("the saved session is no longer valid: run `team_chat.py login` again") from None
        raise ChatError("%s %s: %s %s" % (method, url.split("?")[0], code, detail.get("error", ""))) from None
    except urllib.error.URLError as error:
        raise ChatError("cannot reach %s: %s" % (url.split("/_matrix")[0], error.reason)) from None
    return json.loads(raw.decode("utf-8")) if raw else {}


def homeserver(server):
    """The client API's base URL, from the server name's .well-known file (the server itself when it has none)."""
    if server.startswith(("http://", "https://")):
        return server.rstrip("/")
    try:
        found = request("GET", "https://%s/.well-known/matrix/client" % server)
        return found["m.homeserver"]["base_url"].rstrip("/")
    except (ChatError, KeyError, TypeError, ValueError):
        return "https://" + server


def client(base, path):
    return base + "/_matrix/client/v3" + path


def quote(text):
    return urllib.parse.quote(text, safe="")


def session():
    config = load()
    if not config.get("access_token"):
        raise ChatError("not signed in: run `python -I scripts/team_chat.py login` first")
    return config


def wait_for_login_token(port_holder, ready, timeout):
    """A listener on 127.0.0.1 for the one redirect that carries ?loginToken=; returns the token or None."""
    found = {}

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            query = urllib.parse.parse_qs(urllib.parse.urlsplit(self.path).query)
            token = (query.get("loginToken") or [None])[0]
            if token:
                found["token"] = token   # (kept even if the browser closes the tab before the page below is sent)
            text = "Signed in. You can close this tab." if token else "No login token in this request."
            try:
                self.send_response(200 if token else 400)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.end_headers()
                self.wfile.write(("<!doctype html><title>Hookshot team chat</title><p>%s</p>" % text).encode("utf-8"))
            except OSError:
                pass   # (the browser went away first, e.g. WinError 10053; the token above is what matters)

        def log_message(self, *args):
            pass

    server = http.server.HTTPServer(("127.0.0.1", 0), Handler)
    server.timeout = 1
    port_holder.append(server.server_address[1])
    ready.set()
    deadline = time.monotonic() + timeout
    try:
        while "token" not in found and time.monotonic() < deadline:
            server.handle_request()
    finally:
        server.server_close()
    return found.get("token")


def login(args):
    base = homeserver(args.server)
    config = load()
    if args.access_token_stdin:
        token = sys.stdin.readline().strip()
        if not token:
            raise ChatError("no access token on standard input")
        me = request("GET", client(base, "/account/whoami"), token)
        config.update(server=args.server, base_url=base, access_token=token, user_id=me["user_id"],
                      device_id=me.get("device_id"))
        save(config)
        print("signed in as %s" % me["user_id"])
        return
    flows = [f.get("type") for f in request("GET", client(base, "/login")).get("flows", [])]
    if "m.login.sso" not in flows:
        raise ChatError("%s offers no single sign-on (flows: %s); use --access-token-stdin" % (base, ", ".join(flows)))
    port, ready = [], threading.Event()
    result = {}
    listener = threading.Thread(target=lambda: result.update(token=wait_for_login_token(port, ready, LOGIN_TIMEOUT)))
    listener.daemon = True
    listener.start()
    ready.wait()
    url = client(base, "/login/sso/redirect?redirectUrl=" + quote("http://127.0.0.1:%d/" % port[0]))
    print("Sign in with Discord here (the link works for %d minutes):\n\n  %s\n" % (LOGIN_TIMEOUT // 60, url), flush=True)
    if not args.no_browser:
        webbrowser.open(url)
    listener.join()
    if not result.get("token"):
        raise ChatError("no sign-in came back within %d minutes" % (LOGIN_TIMEOUT // 60))
    reply = request("POST", client(base, "/login"), body={
        "type": "m.login.token", "token": result["token"], "initial_device_display_name": DEVICE_NAME})
    config.update(server=args.server, base_url=base, access_token=reply["access_token"], user_id=reply["user_id"],
                  device_id=reply.get("device_id"))
    save(config)
    print("signed in as %s; saved to %s" % (reply["user_id"], config_path()))


def whoami(args):
    config = session()
    me = request("GET", client(config["base_url"], "/account/whoami"), config["access_token"])
    print(me["user_id"])


def logout(args):
    config = load()
    if config.get("access_token"):
        try:
            request("POST", client(config["base_url"], "/logout"), config["access_token"], body={})
        except ChatError as error:
            print("warning: %s" % error, file=sys.stderr)
    for key in ("access_token", "user_id", "device_id"):
        config.pop(key, None)
    save(config)
    print("signed out")


def room_id(config, alias):
    """The room's ID, joining it the first time."""
    rooms = config.setdefault("rooms", {})
    if alias in rooms:
        return rooms[alias]
    joined = request("POST", client(config["base_url"], "/join/" + quote(alias)), config["access_token"], body={})
    rooms[alias] = joined["room_id"]
    save(config)
    return joined["room_id"]


def interval(config):
    return 60 * float(config.get("min_interval_minutes", MIN_INTERVAL_MINUTES))


def digest(text):
    return hashlib.sha256(" ".join(text.split()).encode("utf-8")).hexdigest()


def refusal(config, alias, text, force, now):
    """Why this post must not go out now, or None."""
    last = config.get("last_post", {}).get(alias)
    if not last:
        return None
    if text is not None and last.get("digest") == digest(text):
        return "this is the same text as the last post"
    wait = last["time"] + interval(config) - now
    if wait > 0 and not force:
        return ("the last post was %d minutes ago; the next can go out in %d minutes (--force only when the person "
                "you work for asked for an update)" % ((now - last["time"]) // 60, -(-wait // 60)))
    return None


def status(args):
    config = session()
    alias = args.room or config.get("room", ROOM)
    last = config.get("last_post", {}).get(alias)
    why = refusal(config, alias, None, False, time.time())
    print("signed in as %s, room %s" % (config.get("user_id"), alias))
    print("last post: %s" % (time.strftime("%Y-%m-%d %H:%M", time.localtime(last["time"])) if last else "never"))
    print("a post now: %s" % ("refused, " + why if why else "would go out"))


def post(args):
    config = session()
    alias = args.room or config.get("room", ROOM)
    text = args.text if args.text is not None else sys.stdin.read()
    text = text.strip()
    while text.startswith(PREFIX.strip()):  # the robot is added below; one written by the agent would show twice
        text = text[len(PREFIX.strip()):].lstrip()
    if not text:
        raise ChatError("nothing to post")
    now = time.time()
    why = refusal(config, alias, text, args.force, now)
    if why:
        print("not posted: " + why, file=sys.stderr)
        return EXIT_REFUSED
    rid = room_id(config, alias)
    body = {"msgtype": "m.text", "body": PREFIX + text}
    path = "/rooms/%s/send/m.room.message/%s" % (quote(rid), uuid.uuid4().hex)
    sent = request("PUT", client(config["base_url"], path), config["access_token"], body=body)
    config.setdefault("last_post", {})[alias] = {"time": now, "digest": digest(text), "event_id": sent.get("event_id")}
    save(config)
    print("posted to %s" % alias)
    return 0


def read(args):
    config = session()
    alias = args.room or config.get("room", ROOM)
    rid = room_id(config, alias)
    path = "/rooms/%s/messages?dir=b&limit=%d" % (quote(rid), max(1, min(args.limit, 200)))
    events = request("GET", client(config["base_url"], path), config["access_token"]).get("chunk", [])
    for event in reversed(events):
        content = event.get("content", {})
        if event.get("type") != "m.room.message" or "body" not in content:
            continue
        when = time.strftime("%Y-%m-%d %H:%M", time.localtime(event.get("origin_server_ts", 0) / 1000))
        print("%s %s: %s" % (when, event.get("sender"), content["body"]))


def unicode_output():
    """Messages carry the robot and whatever people wrote. Into a pipe (an agent's tool, `| more`) Windows' Python writes
    the ANSI code page, cp1252, which has no emoji: read stopped at the first post with a UnicodeEncodeError. A pipe gets
    UTF-8, as -X utf8 would give it; a console keeps its own encoding. Either way a character it cannot show is replaced
    instead of ending the command."""
    for stream in (sys.stdout, sys.stderr):
        if not hasattr(stream, "reconfigure"):   # (a StringIO, as in the test)
            continue
        if stream.isatty():
            stream.reconfigure(errors="replace")
        else:
            stream.reconfigure(encoding="utf-8", errors="replace")


def main(argv=None):
    unicode_output()
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("login", help="sign in with Discord and save the session")
    p.add_argument("--server", default=SERVER)
    p.add_argument("--access-token-stdin", action="store_true", help="save a token from Element instead")
    p.add_argument("--no-browser", action="store_true", help="only print the sign-in link")
    sub.add_parser("whoami", help="the signed-in account")
    p = sub.add_parser("status", help="whether a post would go out now")
    p.add_argument("--room")
    sub.add_parser("logout", help="end the session and forget it")
    p = sub.add_parser("read", help="the room's latest messages, oldest first")
    p.add_argument("--limit", type=int, default=20)
    p.add_argument("--room")
    p = sub.add_parser("post", help="post an update")
    p.add_argument("text", nargs="?")
    p.add_argument("--force", action="store_true", help="skip the minimum interval (asked-for updates only)")
    p.add_argument("--room")
    args = parser.parse_args(argv)
    commands = {"login": login, "whoami": whoami, "status": status, "logout": logout, "read": read, "post": post}
    try:
        return commands[args.command](args) or 0
    except ChatError as error:
        print("team_chat: %s" % error, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
