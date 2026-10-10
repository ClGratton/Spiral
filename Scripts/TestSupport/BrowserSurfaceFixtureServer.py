#!/usr/bin/env python3
"""Local HTTPS fixture for Scripts/TestBrowserSurface.sh.

Serves deterministic pages on an ephemeral 127.0.0.1 port, appends one JSON line
per request (path, query, Cookie header) to --log so the script can use the
server as an independent oracle, and writes the SHA-256 of the downloadable
archive to --expected-hash. Nothing here talks to the network beyond loopback.
"""
import argparse
import hashlib
import http.server
import json
import socketserver
import ssl
import threading
import time
import urllib.parse

ARCHIVE = bytes((i * 31 + 7) & 0xFF for i in range(3 * 1024 * 1024))
TOKEN = "SECRETTOKEN123"

INDICATORS = "".join(
    '<div id="i%d" style="position:fixed;left:%dpx;top:10px;width:40px;height:40px;background:#808080"></div>' % (i, 300 + i * 50)
    for i in range(6)
)

PAGES = {
    "/paint": '<title>paint</title><body style="margin:0;background:#c8640a">',
    "/idle": '<title>idle</title><body style="margin:0;background:#223344">',
    "/dlpage": (
        '<title>dl</title><body style="margin:0;background:#304050">'
        '<a href="/dl.zip?token=%s" style="position:absolute;left:10px;top:10px;width:300px;height:80px;background:#ccc;display:block">zip</a>'
        '<a href="/evil.exe?token=%s" style="position:absolute;left:10px;top:120px;width:300px;height:80px;background:#ccc;display:block">exe</a>'
    ) % (TOKEN, TOKEN),
    "/nav": (
        '<title>nav</title><body style="margin:0;background:#405030">'
        '<button style="position:absolute;left:10px;top:10px;width:300px;height:60px" '
        'onclick="window.open(\'https://popup.spiral.test/idle\',\'_blank\')">popup</button>'
        '<button style="position:absolute;left:10px;top:100px;width:300px;height:60px" '
        'onclick="location.href=\'https://example.com/offsite\'">offsite</button>'
        '<a href="https://example.org/x" target="_blank" '
        'style="position:absolute;left:10px;top:200px;width:300px;height:60px;display:block;background:#ccc">blank</a>'
    ),
    "/popup-ok": (
        '<title>popup-ok</title><body style="margin:0;background:#405030">'
        '<button style="position:absolute;left:10px;top:10px;width:300px;height:60px" '
        'onclick="window.open(\'https://fixture.spiral.test/paint\',\'_blank\')">popup</button>'
    ),
    "/consent": (
        '<title>consent</title><body style="margin:0;background:#304050">'
        '<button style="position:absolute;left:10px;top:10px;width:300px;height:60px" '
        'onclick="location.href=\'https://consent.spiral.test/paint?step=1\'">sign in</button>'
    ),
    "/screen": (
        '<title>screen</title><body style="margin:0;background:#808080">'
        '<script>var e=location.search.slice(3).split(",").map(Number);'
        'var v=[screen.width,screen.height,screen.availWidth,screen.availHeight];'
        'for(var i=0;i<v.length;i++){var d=document.createElement("div");'
        'd.style.cssText="position:fixed;top:10px;width:40px;height:40px;left:"+(10+i*50)+"px;background:"+(v[i]==e[i]?"#00ff00":"#ff0000");'
        'document.body.appendChild(d)}</script>'
    ),
    "/tp": (
        '<title>tp</title><body style="margin:0;background:#808080">'
        '<iframe src="https://third.party.test/tpframe" style="border:0;width:640px;height:360px"></iframe>'
    ),
    "/lang": (
        '<title>lang</title><body style="margin:0;background:#808080">'
        '<script>var want=decodeURIComponent(location.search.slice(3));'
        'document.body.style.background=navigator.languages.join(",")==want?"#00ff00":"#ff0000"</script>'
    ),
    "/input": (
        '<title>input</title><body style="margin:0;height:9000px;background:linear-gradient(#ff0000 0,#ff0000 400px,#0000ff 400px)">'
        + INDICATORS
        + '<button id="b" style="position:fixed;left:10px;top:10px;width:200px;height:60px">click</button>'
        '<script>function on(i){document.getElementById("i"+i).style.background="#00ff00"}'
        'document.getElementById("b").onclick=function(){on(0)};'
        'document.onkeydown=function(e){if(e.keyCode==65)on(1);if(e.keyCode==13)on(3);if(e.keyCode==8)on(5)};'
        'document.onkeypress=function(e){if(e.charCode==122)on(2);if(e.charCode==13)on(4)};</script>'
    ),
    "/clip": (
        '<title>clip</title><body style="margin:0;background:#ffffff">'
        '<input id="t" style="position:absolute;left:10px;top:10px;width:300px;height:40px;font-size:24px" autofocus>'
        '<div id="o" style="position:fixed;left:400px;top:10px;width:100px;height:100px;background:#808080"></div>'
        '<script>var t=document.getElementById("t"),o=document.getElementById("o");'
        'function u(){var v=t.value;o.style.background=v==""?"#808080":v=="ab"?"#00ff00":v=="abab"?"#0000ff":v=="a"?"#ffff00":v=="\\u00e9"?"#ff00ff":"#ff0000"}'
        't.oninput=u;u();</script>'
    ),
    "/anim": (
        '<title>anim</title><body style="margin:0"><canvas id="c" width="640" height="360"></canvas><script>'
        'var c=document.getElementById("c").getContext("2d"),t=0;'
        'function f(){t++;c.fillStyle="hsl("+(t*3%360)+",70%,50%)";c.fillRect(0,0,640,360);requestAnimationFrame(f)}f()</script>'
    ),
    "/select": (
        '<title>select</title><body style="margin:0;background:#123456">'
        '<select id="s" style="position:absolute;left:10px;top:10px;width:200px;height:40px;font-size:20px">'
        '<option>one</option><option>two</option><option>three</option><option>four</option><option>five</option></select>'
    ),
}


class Handler(http.server.BaseHTTPRequestHandler):
    log_file = None

    def log_message(self, *args):
        pass

    def respond(self, code, content_type, body, extra=()):
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        for key, value in extra:
            self.send_header(key, value)
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        parsed = urllib.parse.urlsplit(self.path)
        Handler.log_file.write(json.dumps({
            "t": round(time.time(), 3), "path": parsed.path, "query": parsed.query, "cookie": self.headers.get("Cookie"),
            "ua": self.headers.get("User-Agent"), "lang": self.headers.get("Accept-Language"),
        }) + "\n")
        if parsed.path == "/":
            self.respond(200, "text/html", b'<title>index</title><body style="margin:0;background:#204060">', [
                ("Set-Cookie", "spiral_persist=abc123; Max-Age=86400; Path=/; HttpOnly"),
                ("Set-Cookie", "spiral_session=sess456; Path=/"),
            ])
        elif parsed.path == "/tpframe":
            # Cross-site frame: a cookie set by header, one set by script, and a page that
            # reports whether the header cookie is readable in this third-party context.
            self.respond(200, "text/html", (
                '<title>tpframe</title><body style="margin:0;background:#ff0000"><script>'
                'var c=document.cookie;document.cookie="tp_js=1; SameSite=None; Secure; Path=/; Max-Age=3600";'
                'document.body.style.background=c.indexOf("tp_header=1")>=0?"#00ff00":"#ff0000"</script>'
            ).encode(), [("Set-Cookie", "tp_header=1; SameSite=None; Secure; Path=/; Max-Age=3600")])
        elif parsed.path == "/whoami":
            self.respond(200, "text/html", b'<title>whoami</title><body style="margin:0;background:#602040">')
        elif parsed.path == "/dl.zip":
            self.respond(200, "application/octet-stream", ARCHIVE,
                         [("Content-Disposition", 'attachment; filename="spiral-test.zip"')])
        elif parsed.path == "/evil.exe":
            self.respond(200, "application/octet-stream", b"MZ" + bytes(1024),
                         [("Content-Disposition", 'attachment; filename="evil.exe"')])
        elif parsed.path in ("/status/403", "/status/503"):
            # What a managed security check answers with: the Fab panel only reads the number.
            self.respond(int(parsed.path.rsplit("/", 1)[1]), "text/html",
                         b'<title>status</title><body style="margin:0;background:#112233">')
        elif parsed.path in PAGES:
            self.respond(200, "text/html", PAGES[parsed.path].encode())
        else:
            self.respond(404, "text/plain", b"not found")


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port-file", required=True)
    parser.add_argument("--log", required=True)
    parser.add_argument("--cert", required=True)
    parser.add_argument("--key", required=True)
    parser.add_argument("--expected-hash", required=True)
    args = parser.parse_args()

    Handler.log_file = open(args.log, "a", buffering=1)
    with open(args.expected_hash, "w") as hash_file:
        hash_file.write(hashlib.sha256(ARCHIVE).hexdigest() + "\n")
    server = Server(("127.0.0.1", 0), Handler)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(args.cert, args.key)
    server.socket = context.wrap_socket(server.socket, server_side=True, do_handshake_on_connect=False)
    with open(args.port_file, "w") as port_file:
        port_file.write(str(server.server_address[1]) + "\n")
    threading.Thread(target=server.serve_forever, daemon=True).start()
    while True:
        time.sleep(3600)


if __name__ == "__main__":
    main()
