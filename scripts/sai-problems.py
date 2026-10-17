#!/usr/bin/env python3
"""Ask a sai-web what went wrong in an event, see READMEs/README-problems.md.

usage: sai-problems.py [--json] <site> <command> [args]

  <site>   the host sai-web is mounted on, eg, libwebsockets.org

commands:

  events <project> [<branch>]
      the project's latest events, newest first: event uuid, git hash,
      state and summary

  problems <event>
      what the problems pane shows for the event: each group of problems,
      its builds, how often it failed, its history in the repo's other
      events, and where it was seen: the uid of each occurrence, and its
      task, try and line in the task's log

  excerpts <event> <uid>[,<uid>...]
      the start of what the log said for those occurrences

  tasks <event>
      the event's tasks: task uuid, state, task name and platform

  log <task> [<try>]
      the whole log of the task's latest try, or of try <try> counting
      from 0, as the builder sent it

<event> is the 32-hex event uuid; for problems, excerpts and tasks it may
also be "latest:<project>[:<branch>]", the newest event.  <task> is the
64-hex task uuid.

--json prints sai-web's own JSON for problems, excerpts and tasks instead of
the summaries.

Everything here is what the web UI shows anyone, so no account is needed.
It's asked for over the same websocket the UI uses.
"""

import base64
import json
import os
import socket
import ssl
import struct
import sys
import time
import urllib.request

JS_API_VERSION = 4          # SAI_JS_API_VERSION in assets/sai.js
TIMEOUT = 60

SEV = {0: "warning", 1: "flaky", 2: "error"}
PHASE = {0: "config", 1: "build", 2: "link", 3: "test", 4: "other"}
STATE = {0: "waiting", 1: "passed-to-builder", 2: "building", 3: "success",
         4: "FAIL", 5: "cancelled", 6: "building-has-failures",
         7: "deleted", 8: "not-ready", 9: "step-success", 10: "paused",
         11: "yielded"}
FINAL = (3, 4, 5)           # SAIES_SUCCESS, SAIES_FAIL, SAIES_CANCELLED
LOG_IDLE = 5


def usage(why=None):
    if why:
        sys.stderr.write("sai-problems.py: %s\n" % why)
    sys.stderr.write(__doc__)
    sys.exit(1)


class Ws:
    """The least websocket client that talks to sai-web's browse protocol"""

    def __init__(self, site):
        s = socket.create_connection((site, 443), timeout=TIMEOUT)
        self.s = ssl.create_default_context().wrap_socket(
            s, server_hostname=site)
        key = base64.b64encode(os.urandom(16)).decode()
        self.s.sendall((
            "GET /sai/browse?client=sai HTTP/1.1\r\n"
            "Host: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
            "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n"
            "Sec-WebSocket-Protocol: com-warmcat-sai\r\n"
            "Origin: https://%s\r\n\r\n" % (site, key, site)).encode())
        self.buf = b""
        while b"\r\n\r\n" not in self.buf:
            d = self.s.recv(4096)
            if not d:
                sys.exit("%s: closed during the upgrade" % site)
            self.buf += d
        hdr, self.buf = self.buf.split(b"\r\n\r\n", 1)
        status = hdr.split(b"\r\n", 1)[0].decode(errors="replace")
        if " 101 " not in status:
            sys.exit("%s: %s" % (site, status))

    def _send(self, op, p):
        m = os.urandom(4)
        n = len(p)
        if n < 126:
            h = struct.pack("!BB", 0x80 | op, 0x80 | n)
        elif n < 65536:
            h = struct.pack("!BBH", 0x80 | op, 0xfe, n)
        else:
            h = struct.pack("!BBQ", 0x80 | op, 0xff, n)
        self.s.sendall(h + m + bytes(b ^ m[i & 3] for i, b in enumerate(p)))

    def send(self, o):
        self._send(1, json.dumps(o).encode())

    def _need(self, n):
        while len(self.buf) < n:
            d = self.s.recv(65536)
            if not d:
                raise EOFError
            self.buf += d
        r, self.buf = self.buf[:n], self.buf[n:]
        return r

    def recv(self):
        """the next whole JSON message, or None if the server closed"""
        msg = b""
        while True:
            b0, b1 = self._need(2)
            n = b1 & 0x7f
            if n == 126:
                n = struct.unpack("!H", self._need(2))[0]
            elif n == 127:
                n = struct.unpack("!Q", self._need(8))[0]
            p = self._need(n)
            op = b0 & 0xf
            if op == 8:
                return None
            if op == 9:
                self._send(10, p)
                continue
            if op == 10:
                continue
            msg += p
            if b0 & 0x80:
                try:
                    return json.loads(msg)
                except ValueError:
                    msg = b""

    def wait(self, pred):
        """messages until pred says one is what we want, which it returns"""
        end = time.time() + TIMEOUT
        while time.time() < end:
            try:
                j = self.recv()
            except (EOFError, socket.timeout):
                break
            if j is None:
                break
            r = pred(j)
            if r is not None:
                return r
        sys.exit("no answer from sai-web")


def events(site, project, branch=None):
    url = "https://%s/sai/rss.json?project=%s" % (site, project)
    if branch:
        url += "&branch=" + branch
    with urllib.request.urlopen(url, timeout=TIMEOUT) as r:
        return json.load(r).get("items", [])


def event_uuid(site, e):
    if not e.startswith("latest:"):
        return e.upper()
    a = e.split(":")
    if len(a) < 2 or not a[1]:
        usage("latest:<project>[:<branch>]")
    ev = events(site, a[1], a[2] if len(a) > 2 else None)
    if not ev:
        sys.exit("no events for %s" % e[7:])
    return ev[0]["uuid"]


def ask_problems(ws, ev, schema, uids=None):
    o = {"schema": schema, "event_hash": ev}
    if uids:
        o["uids"] = uids
    ws.send(o)
    return ws.wait(lambda j: j if j.get("schema") == schema and
                   j.get("event_hash") == ev else None)


def when(t):
    return time.strftime("%Y-%m-%d", time.gmtime(t)) if t else "?"


def print_problems(j):
    print("event %s: %d builds, %d tries read, %d clean, "
          "%d errors, %d flaky, %d warnings" % (
              j["event_hash"], j.get("builds", 0), j.get("runs", 0),
              j.get("clean", 0), j.get("errors", 0), j.get("flaky", 0),
              j.get("warnings", 0)))
    for g in j.get("groups", []):
        where = g.get("path") or ""
        if g.get("lines"):
            where += ":" + ",".join(str(x) for x in g["lines"])
        print()
        print("%s %s %s %s%s" % (
            g["id"], SEV.get(g.get("sev"), "?"),
            PHASE.get(g.get("phase"), "?"), g.get("code") or "",
            ("  " + where) if where else ""))
        for m in g.get("msgs", []):
            print("    %s" % m)
        s = "    %s" % g.get("scope", "")
        if g.get("att"):
            s += ", failed %d of %d runs" % (g.get("fails", 0), g["att"])
        print(s)
        if g.get("new"):
            print("    new: in the last %d events of the branch in a row, "
                  "none before" % g["new"])
        h = g.get("hist")
        if h:
            print("    also in %d of the last %d events, %s to %s" % (
                h.get("ev", 0), h.get("of", 0), when(h.get("first")),
                when(h.get("last"))))
        for o in g.get("occ", []):
            print("    uid %-4d %s try %d, log line %d (+%d)  %s %s" % (
                o["uid"], o["task"], o.get("run", 0), o.get("ll", 0),
                o.get("lls", 0), o.get("cfg", ""), o.get("plat", "")))
        if g.get("more"):
            print("    ... and %d more" % g["more"])


def print_excerpts(j):
    for e in j.get("ex", []):
        print("===== uid %d" % e.get("uid", 0))
        print(e.get("text") or "")


def tasks(ws, ev):
    ws.send({"schema": "com.warmcat.sai.eventinfo",
             "js_api_version": JS_API_VERSION, "event_hash": ev})

    def pred(j):
        if j.get("schema") != "sai.warmcat.com.overview":
            return None
        for o in j.get("overview", []):
            if o.get("e", {}).get("uuid", "").upper() == ev:
                return o
        return None

    return ws.wait(pred)


def log(ws, task, run):
    """
    sai-web sends what it has of the log, then more as it comes.  Nothing
    marks the end of a finished task's log (a row's "finished" is the end of
    a step), so it's the end when nothing more comes for LOG_IDLE seconds
    """
    ws.send({"schema": "com.warmcat.sai.taskinfo",
             "js_api_version": JS_API_VERSION, "logs": 1, "run": run,
             "last_log_ts": 0, "last_log_uid": 0, "task_hash": task})
    out = sys.stdout.buffer
    state = None
    rows = 0
    while True:
        try:
            j = ws.recv()
        except (EOFError, socket.timeout):
            break
        if j is None:
            break
        s = j.get("schema")
        if s == "com.warmcat.sai.task_gone" and \
           j.get("task_hash", "").upper() == task:
            sys.exit("no such task or try")
        if s == "com.warmcat.sai.taskinfo" and \
           j.get("t", {}).get("uuid", "").upper() == task:
            state = j["t"].get("state")
            continue
        if s != "com-warmcat-sai-logs" or \
           j.get("task_uuid", "").upper() != task:
            continue
        out.write(base64.b64decode(j.get("log") or ""))
        rows += 1
        ws.s.settimeout(LOG_IDLE)
    out.flush()
    if not rows:
        sys.exit("no log came")
    if state not in FINAL:
        sys.stderr.write("the task is %s, this is its log so far\n" %
                         STATE.get(state, "in an unknown state"))


def main():
    a = sys.argv[1:]
    raw = False
    while a and a[0].startswith("--"):
        if a[0] != "--json":
            usage()
        raw = True
        a = a[1:]
    if len(a) < 3:
        usage()
    site, cmd, args = a[0], a[1], a[2:]

    if cmd == "events":
        if len(args) > 2:
            usage()
        for e in events(site, *args):
            print("%s %s %-9s %s" % (e["uuid"], e["hash"][:9],
                                     e.get("state_name", ""),
                                     e.get("summary", "")))
        return

    if cmd == "problems" and len(args) == 1:
        ws = Ws(site)
        j = ask_problems(ws, event_uuid(site, args[0]),
                         "com.warmcat.sai.problems")
        print(json.dumps(j)) if raw else print_problems(j)
        return

    if cmd == "excerpts" and len(args) == 2:
        ws = Ws(site)
        j = ask_problems(ws, event_uuid(site, args[0]),
                         "com.warmcat.sai.problem_excerpts", args[1])
        print(json.dumps(j)) if raw else print_excerpts(j)
        return

    if cmd == "tasks" and len(args) == 1:
        ws = Ws(site)
        o = tasks(ws, event_uuid(site, args[0]))
        if raw:
            print(json.dumps(o))
            return
        for t in sorted(o.get("t", []), key=lambda t: (t.get("taskname", ""),
                                                       t.get("platform", ""))):
            print("%s %-9s %s %s" % (t["uuid"], STATE.get(t.get("state"), "?"),
                                     t.get("taskname", ""),
                                     t.get("platform", "")))
        return

    if cmd == "log" and len(args) in (1, 2):
        try:
            run = int(args[1]) if len(args) == 2 else -1
        except ValueError:
            usage("<try> is a number")
        ws = Ws(site)
        log(ws, args[0].upper(), run)
        return

    usage()


main()
