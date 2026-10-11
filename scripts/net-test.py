#!/usr/bin/env python3
"""End-to-end network test: boot Maleos in QEMU with an e1000 card on QEMU's user-mode network
and exercise it from both sides.

    usage: scripts/net-test.py [maleos.iso] [--mem 256M] [--verbose]

Host to guest goes through QEMU port forwarding to the servers that init starts (httpd on 80,
echod on TCP and UDP 7) and to `nc -l` typed into the shell. Guest to host uses the QEMU
gateway address 10.0.2.2, which reaches services on the host's loopback (ping, wget, nc).
Exit status 0 means every check passed and the guest powered off cleanly.
"""
import argparse
import hashlib
import http.server
import os
import queue
import random
import socket
import socketserver
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request

PROMPT = "$ "


def free_port(kind=socket.SOCK_STREAM):
    s = socket.socket(socket.AF_INET, kind)
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("iso", nargs="?", default="build/maleos.iso")
    ap.add_argument("--mem", default="256M")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

    http_port, echo_port, udp_port, nc_port = free_port(), free_port(), free_port(socket.SOCK_DGRAM), free_port()
    fwd = (f"hostfwd=tcp:127.0.0.1:{http_port}-:80,hostfwd=tcp:127.0.0.1:{echo_port}-:7,"
           f"hostfwd=udp:127.0.0.1:{udp_port}-:7,hostfwd=tcp:127.0.0.1:{nc_port}-:9000")
    cmd = ["qemu-system-x86_64", "-cdrom", args.iso, "-m", args.mem, "-display", "none",
           "-serial", "stdio", "-monitor", "none", "-no-reboot",
           "-netdev", f"user,id=n0,{fwd}", "-device", "e1000,netdev=n0"]
    qemu = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)

    chunks = queue.Queue()

    def reader():
        while True:
            b = qemu.stdout.read1(4096)
            if not b:
                chunks.put(None)
                return
            chunks.put(b.decode("utf-8", "replace"))

    threading.Thread(target=reader, daemon=True).start()
    log = []

    def wait_for(needle, timeout, start):
        deadline = time.time() + timeout
        while time.time() < deadline:
            text = "".join(log)
            i = text.find(needle, start)
            if i >= 0:
                return i + len(needle)
            try:
                c = chunks.get(timeout=0.2)
            except queue.Empty:
                continue
            if c is None:
                return -1
            log.append(c)
            if args.verbose:
                sys.stdout.write(c)
                sys.stdout.flush()
        return -1

    failures = 0

    def fail(msg):
        nonlocal failures
        failures += 1
        print("FAIL:", msg)

    def ok(msg):
        print("ok:  ", msg)

    pos = wait_for("Maleos shell", 120, 0)
    if pos < 0:
        fail("the shell never started")
        print("".join(log)[-3000:])
        qemu.kill()
        sys.exit(1)
    pos = wait_for(PROMPT, 10, pos)
    boot_text = "".join(log)
    for d in ("httpd", "echod"):
        if f"init: started {d}" not in boot_text:
            fail(f"init did not start {d}")

    state = {"pos": pos}

    def shell(text, want, timeout=40, avoid=()):
        qemu.stdin.write((text + "\n").encode())
        qemu.stdin.flush()
        end = wait_for(PROMPT, timeout, state["pos"])
        if end < 0:
            fail(f"'{text}': no prompt came back")
            return None
        out = "".join(log)[state["pos"]:end]
        state["pos"] = end
        bad = [w for w in want if w not in out] + [a for a in avoid if a in out]
        if bad:
            fail(f"'{text}': problem with {bad!r}\n---\n{out}\n---")
        else:
            ok(f"shell: {text}")
        return out

    # ---- guest tools ----
    shell("ifconfig", ["eth0: up", "52:54:00:12:34:56", "10.0.2.15", "255.255.255.0", "10.0.2.2"])
    shell("ping 10.0.2.2 3", ["3 sent, 3 received, 0% loss"])
    shell("ping 127.0.0.1 2", ["2 sent, 2 received"])
    shell("ping 10.0.2.99 1", ["1 sent, 0 received, 100% loss", "timed out"], timeout=60)
    shell("ping nonsense", ["usage: ping"])
    shell("ifconfig", ["received", "sent"])

    # ---- host to guest: HTTP ----
    base = f"http://127.0.0.1:{http_port}"

    def get(path, timeout=20):
        try:
            with urllib.request.urlopen(base + path, timeout=timeout) as r:
                return r.status, r.read()
        except urllib.error.HTTPError as e:
            return e.code, e.read()

    try:
        code, body = get("/")
        if code == 200 and b"Hello from Maleos" in body:
            ok("httpd serves /")
        else:
            fail(f"httpd / -> {code} {body[:80]!r}")
        code, body = get("/data.txt")
        want = open(os.path.join(root, "initrd/www/data.txt"), "rb").read()
        if code == 200 and body == want:
            ok(f"httpd serves a {len(want)} byte file intact")
        else:
            fail(f"httpd /data.txt -> {code}, {len(body)} bytes")
        code, _ = get("/missing.html")
        ok("httpd 404") if code == 404 else fail(f"missing file gave {code}")
        code, _ = get("/../etc/motd")
        ok("httpd refuses ..") if code in (403, 404) else fail(f"path traversal gave {code}")
        req = urllib.request.Request(base + "/", data=b"x", method="POST")
        try:
            urllib.request.urlopen(req, timeout=10)
            fail("POST was accepted")
        except urllib.error.HTTPError as e:
            ok("httpd 405 for POST") if e.code == 405 else fail(f"POST gave {e.code}")
        for i in range(5):  # repeated connections
            code, body = get("/index.html")
            if code != 200:
                fail(f"request {i} gave {code}")
                break
        else:
            ok("five more requests in a row")
    except Exception as e:  # noqa: BLE001
        fail(f"HTTP test raised {e!r}")

    # ---- host to guest: TCP echo with a large payload, sending and receiving at once ----
    def echo_roundtrip(size, seed):
        rnd = random.Random(seed)
        data = bytes(rnd.getrandbits(8) for _ in range(size))
        s = socket.create_connection(("127.0.0.1", echo_port), timeout=30)
        got = bytearray()

        def rx():
            while len(got) < size:
                b = s.recv(65536)
                if not b:
                    return
                got.extend(b)

        t = threading.Thread(target=rx)
        t.start()
        s.sendall(data)
        t.join(60)
        s.close()
        return hashlib.sha256(bytes(got)).digest() == hashlib.sha256(data).digest() and len(got) == size

    for size in (1, 1460, 5000, 1 << 20):
        try:
            t0 = time.time()
            if echo_roundtrip(size, size):
                ok(f"TCP echo of {size} bytes ({time.time() - t0:.1f}s)")
            else:
                fail(f"TCP echo of {size} bytes came back different")
        except Exception as e:  # noqa: BLE001
            fail(f"TCP echo of {size} bytes raised {e!r}")

    # ---- host to guest: UDP echo ----
    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    u.settimeout(5)
    for size in (1, 100, 512, 1472):
        payload = bytes(random.Random(size).getrandbits(8) for _ in range(size))
        try:
            u.sendto(payload, ("127.0.0.1", udp_port))
            back, _ = u.recvfrom(4096)
            ok(f"UDP echo of {size} bytes") if back == payload else fail(f"UDP echo of {size} bytes differs")
        except socket.timeout:
            fail(f"UDP echo of {size} bytes timed out")
    u.close()

    # ---- host to guest: a connection to `nc -l` in the shell ----
    qemu.stdin.write(b"nc -l 9000 reply-from-guest\n")
    qemu.stdin.flush()
    time.sleep(1.0)
    try:
        c = socket.create_connection(("127.0.0.1", nc_port), timeout=10)
        c.sendall(b"hello from the host\n")
        c.shutdown(socket.SHUT_WR)
        c.settimeout(10)
        reply = b""
        while True:
            b = c.recv(100)
            if not b:
                break
            reply += b
        c.close()
        ok("nc -l answered the host") if reply == b"reply-from-guest" else fail(f"nc -l replied {reply!r}")
    except Exception as e:  # noqa: BLE001
        fail(f"connecting to nc -l raised {e!r}")
    end = wait_for(PROMPT, 30, state["pos"])
    out = "".join(log)[state["pos"]:end] if end > 0 else ""
    if "hello from the host" in out:
        ok("nc -l printed what the host sent")
    else:
        fail(f"nc -l output was {out!r}")
    state["pos"] = end if end > 0 else state["pos"]

    # ---- guest to host ----
    received = {}

    class Handler(http.server.SimpleHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def translate_path(self, path):
            return os.path.join(root, "initrd/www", path.lstrip("/").split("?")[0])

    class Srv(socketserver.ThreadingMixIn, http.server.HTTPServer):
        daemon_threads = True

    hp = free_port()
    httpd = Srv(("127.0.0.1", hp), Handler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    out = shell(f"wget -O /tmp/page http://10.0.2.2:{hp}/data.txt", ["HTTP 200, 22000 bytes"], timeout=60)
    shell("stat /tmp/page", ["22000"])
    shell("wget http://10.0.2.2:%d/index.html" % hp, ["Hello from Maleos", "HTTP 200"], timeout=60)
    shell("wget http://10.0.2.2:%d/nothing" % hp, ["HTTP 404"], timeout=60)
    shell("wget http://10.0.2.2:1/x", ["connection refused"], timeout=60)
    shell("wget http://example.com/", ["only IPv4"])
    httpd.shutdown()

    def tcp_server(port_holder, ready):
        ls = socket.socket()
        ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        ls.bind(("127.0.0.1", 0))
        ls.listen(1)
        port_holder.append(ls.getsockname()[1])
        ready.set()
        ls.settimeout(60)
        conn, _ = ls.accept()
        data = b""
        conn.settimeout(10)
        while not data.endswith(b"\n"):
            b = conn.recv(100)
            if not b:
                break
            data += b
        received["tcp"] = data
        conn.sendall(b"pong from host\n")
        conn.close()
        ls.close()

    holder, ready = [], threading.Event()
    threading.Thread(target=tcp_server, args=(holder, ready), daemon=True).start()
    ready.wait(5)
    shell(f"nc 10.0.2.2 {holder[0]} hello host", ["pong from host"], timeout=60)
    time.sleep(0.2)
    ok("host received the guest's text") if received.get("tcp") == b"hello host\n" else fail(f"host got {received.get('tcp')!r}")

    us = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    us.bind(("127.0.0.1", 0))
    us.settimeout(30)

    def udp_server():
        try:
            data, addr = us.recvfrom(100)
            received["udp"] = data
            us.sendto(b"udp pong", addr)
        except socket.timeout:
            pass

    threading.Thread(target=udp_server, daemon=True).start()
    shell(f"nc -u 10.0.2.2 {us.getsockname()[1]} udp hello", ["udp pong"], timeout=60)
    time.sleep(0.2)
    ok("host received the UDP datagram") if received.get("udp") == b"udp hello\n" else fail(f"host got {received.get('udp')!r}")
    us.close()
    shell("nc 10.0.2.2 1 x", ["connection refused"], timeout=60)

    # ---- the services survived all of that ----
    code, body = get("/")
    ok("httpd still answers") if code == 200 else fail("httpd stopped answering")
    if echo_roundtrip(4000, 99):
        ok("echod still answers")
    else:
        fail("echod stopped answering")
    shell("ps", ["httpd", "echod"])
    shell("free", [])

    qemu.stdin.write(b"poweroff\n")
    qemu.stdin.flush()
    wait_for("powering off", 20, state["pos"])
    try:
        qemu.wait(timeout=20)
        ok("powered off cleanly")
    except subprocess.TimeoutExpired:
        qemu.kill()
        fail("QEMU did not power off")

    text = "".join(log)
    after_boot = text[text.find("Maleos shell"):]  # the boot self-tests kill processes on purpose
    for bad in ("KERNEL PANIC", "EXCEPTION"):
        if bad in text:
            fail(f"guest reported '{bad}'")
    for bad in ("killed:",):
        if bad in after_boot:
            fail(f"guest reported '{bad}'")
    print("PASS" if not failures else f"{failures} failure(s)")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
