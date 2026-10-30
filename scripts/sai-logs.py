#!/usr/bin/env python3
"""Query a sai-web's fleet logs, see READMEs/README-logs.md "Query".

usage: sai-logs.py [--creds FILE] [--auth URL] <site> <query>

  <site>   the host sai-web is mounted on, eg, libwebsockets.org
  <query>  the query string of GET /sai/logs, eg 'hash=4b9f390f', or of
           its sync or pane queries, as 'sync?task=<uuid>' or
           'pane?pane=task&task=<uuid>&at=<us>'; bare 'sync' says if the
           account may query

The JSON answer goes to stdout.  The account needs the fleet log grant.

--creds FILE, or $SAI_LOGS_CREDS, is a file of the account's username on its
first line, its password on its second, and its base32 TOTP secret on a later
line, either alone or after ": ".  None of it is printed.

--auth URL, or $SAI_LOGS_AUTH, is the auth server the site's lws-login uses,
https://auth.warmcat.com by default.

The site's session cookie is kept in ~/.cache/sai-logs/, readable only by
you, and reused until it stops working.  Only then does this log in again,
once, stopping at the first refusal, since the auth server counts failed
logins against the account.
"""

import base64
import hashlib
import errno
import hmac
import http.client
import http.cookiejar
import json
import os
import select
import socket
import struct
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

SERVICE = "sai-logs"
STAGGER = 0.25             # start on the next address after this
CONNECT_LIMIT = 60         # connecting, when there's no timeout
IN_FLIGHT = (errno.EINPROGRESS, errno.EWOULDBLOCK,
             getattr(errno, "WSAEWOULDBLOCK", errno.EWOULDBLOCK))


def usage(why=None):
    if why:
        sys.stderr.write("sai-logs.py: %s\n" % why)
    sys.stderr.write(__doc__)
    sys.exit(1)


def creds(path):
    with open(os.path.expanduser(path)) as f:
        ls = [l.rstrip("\r\n") for l in f]
    if len(ls) < 3:
        usage("%s: wants username, password and TOTP secret lines" % path)
    sec = ls[2:]
    sec = [l.rsplit(": ", 1)[-1].strip() for l in sec if l.strip()]
    if not sec:
        usage("%s: no TOTP secret line" % path)
    return ls[0].strip(), ls[1], sec[-1]


def totp(secret):
    k = base64.b32decode(secret.upper().replace(" ", "") +
                         "=" * (-len(secret) % 8))
    h = hmac.new(k, struct.pack(">Q", int(time.time()) // 30),
                 hashlib.sha1).digest()
    o = h[-1] & 15
    return "%06d" % ((struct.unpack(">I", h[o:o + 4])[0] & 0x7fffffff) %
                     1000000)


def connect(host, port, timeout):
    """
    A TCP connection to the first of host's addresses that answers, with
    timeout set on it for io.  socket.create_connection() tries them one
    after another, waiting out the whole timeout on each, so an address with
    no route to it, eg, an IPv6 one advertised where IPv6 goes nowhere, costs
    that long every time.  Like a browser (RFC 8305), start on the next
    address if one hasn't connected after STAGGER seconds, and take
    whichever connects first.
    """
    if not isinstance(timeout, (int, float)):
        timeout = None          # urllib's "the default", no timeout
    end = time.monotonic() + (timeout or CONNECT_LIMIT)
    ais = socket.getaddrinfo(host, port, type=socket.SOCK_STREAM)
    pending = []
    err = None
    try:
        while ais or pending:
            if ais:
                af, st, pr, _, sa = ais.pop(0)
                s = socket.socket(af, st, pr)
                s.setblocking(False)
                e = s.connect_ex(sa)
                if e and e not in IN_FLIGHT:
                    s.close()
                    err = OSError(e, os.strerror(e))
                    continue
                pending.append(s)
            left = end - time.monotonic()
            if left <= 0:
                break
            # a failed connect is writable on POSIX, exceptional on Windows
            _, w, x = select.select([], pending, pending,
                                    min(STAGGER, left) if ais else left)
            for s in set(w + x):
                pending.remove(s)
                e = s.getsockopt(socket.SOL_SOCKET, socket.SO_ERROR)
                if not e:
                    s.settimeout(timeout)
                    return s
                s.close()
                err = OSError(e, os.strerror(e))
    finally:
        for s in pending:
            s.close()
    raise err or socket.timeout("%s: no address answered" % host)


class HttpsConn(http.client.HTTPSConnection):
    """urllib's https, making its connections with connect()"""

    def __init__(self, *a, **k):
        super().__init__(*a, **k)
        # HTTPConnection's own seam for what makes its socket
        self._create_connection = \
            lambda addr, timeout, *_: connect(addr[0], addr[1], timeout)


class Https(urllib.request.HTTPSHandler):
    def https_open(self, req):
        return self.do_open(HttpsConn, req)


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *a, **k):
        return None


def opener(jar, follow=True):
    hs = [Https(), urllib.request.HTTPCookieProcessor(jar)]
    if not follow:
        hs.append(NoRedirect())
    return urllib.request.build_opener(*hs)


def req(op, url, data=None, origin=None):
    r = urllib.request.Request(url, data=data)
    if origin:
        r.add_header("Origin", origin)
    if data is not None:
        r.add_header("Content-Type", "application/x-www-form-urlencoded")
    try:
        with op.open(r, timeout=60) as resp:
            return resp.status, resp.headers, resp.read()
    except urllib.error.HTTPError as e:
        return e.code, e.headers, e.read()


def login(jar, auth, credpath, target):
    user, pw, sec = creds(credpath)
    op = opener(jar)

    st, _, body = req(op, auth + "/api/status?service_name=" + SERVICE)
    if st != 200:
        sys.exit("%s/api/status: %d" % (auth, st))
    csrf = json.loads(body).get("csrf_token", "")

    form = urllib.parse.urlencode({
        "username": user, "password": pw, "totp": totp(sec),
        "csrf_token": csrf, "redirect_uri": target,
        "service_name": SERVICE}).encode()
    st, hd, body = req(op, auth + "/api/login", form)
    if st != 200:
        try:
            why = json.loads(body).get("error", "")
        except ValueError:
            why = ""
        sys.exit("%s/api/login: %d %s%s" % (auth, st, why,
                 " (wants TOTP)" if hd.get("X-Requires-TOTP") else ""))
    tok = json.loads(body).get("token")
    if not tok:
        sys.exit("%s/api/login: no token in the answer" % auth)

    # hand it to the site's lws-login as the auth page's form would, which
    # comes from the auth server's origin
    u = urllib.parse.urlparse(target)
    sso = "%s://%s/sai/logs/.lws-login-sso" % (u.scheme, u.netloc)
    st, _, _ = req(opener(jar, False), sso, urllib.parse.urlencode(
        {"token": tok, "target": target}).encode(), auth)
    if st not in (200, 302, 303):
        sys.exit("%s: %d" % (sso, st))


def main():
    a = sys.argv[1:]
    credpath = os.environ.get("SAI_LOGS_CREDS")
    auth = os.environ.get("SAI_LOGS_AUTH", "https://auth.warmcat.com")
    while a and a[0].startswith("--"):
        if len(a) < 2 or a[0] not in ("--creds", "--auth"):
            usage()
        if a[0] == "--creds":
            credpath = a[1]
        else:
            auth = a[1]
        a = a[2:]
    if len(a) != 2:
        usage()
    site, q = a
    auth = auth.rstrip("/")

    if q.split("?", 1)[0] in ("sync", "pane"):
        url = "https://%s/sai/logs/%s" % (site, q)
    else:
        url = "https://%s/sai/logs/?%s" % (site, q)

    d = os.path.expanduser("~/.cache/sai-logs")
    os.makedirs(d, mode=0o700, exist_ok=True)
    os.umask(0o077)
    path = os.path.join(d, "cookies-%s.txt" % site)
    jar = http.cookiejar.MozillaCookieJar(path)
    if os.path.exists(path):
        jar.load(ignore_discard=True, ignore_expires=True)

    st, _, body = req(opener(jar), url)
    if st in (401, 403):
        if not credpath:
            usage("%d, and no --creds or $SAI_LOGS_CREDS to log in" % st)
        login(jar, auth, credpath, url)
        st, _, body = req(opener(jar), url)
    jar.save(ignore_discard=True, ignore_expires=True)
    if st != 200:
        sys.exit("%s: %d %s" % (url, st, body[:300].decode(errors="replace")))
    sys.stdout.buffer.write(body)


main()
