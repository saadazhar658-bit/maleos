#!/usr/bin/env python3
"""End-to-end test of the Maleos user space: boot the ISO, type commands into the shell over
the serial port, and check what comes back.

    usage: scripts/shell-test.py [maleos.iso] [--mem 256M] [--verbose]

Each step waits for the next shell prompt before it is judged, so the test is paced by the
guest instead of by sleeps. Exit status 0 means every step passed and the machine powered
itself off cleanly.
"""
import argparse
import os
import queue
import subprocess
import sys
import threading
import time

PROMPT = "$ "
# (command, substrings that must appear in its output, substrings that must not)
STEPS = [
    ("echo hello world", ["hello world"], []),
    ("uname", ["Maleos 0.6 x86_64"], []),
    ("pwd", ["\n/\r"], []),
    ("ls /", ["bin/", "etc/", "share/", "tmp/", "mnt/", "tests/"], []),
    ("cat /etc/hostname", ["maleos"], []),
    ("cat /etc/motd", ["Welcome to Maleos"], []),
    ("cd /share", [], []),
    ("pwd", ["\n/share\r"], []),
    ("ls", ["hello.txt", "pattern.bin", "hello.link"], []),
    ("cat hello.txt", ["Hello from the Maleos initrd!"], []),
    ("cat hello.link", ["Hello from the Maleos initrd!"], []),
    ("cd ..", [], []),
    ("pwd", ["\n/\r"], []),
    ("mkdir /tmp/work", [], []),
    ("echo first line > /tmp/work/f", [], []),
    ("echo second line >> /tmp/work/f", [], []),
    ("cat /tmp/work/f", ["first line", "second line"], []),
    ("ls -l /tmp/work", ["f"], []),
    ("ln -s /tmp/work/f /tmp/work/link", [], []),
    ("cat /tmp/work/link", ["first line"], []),
    ("stat /tmp/work/link", ["symbolic link", "-> /tmp/work/f"], []),
    ("rm /tmp/work/link", [], []),
    ("rm /tmp/work/f", [], []),
    ("rmdir /tmp/work", [], []),
    ("cat /tmp/work/f", ["no such file or directory"], []),
    ("cat /nonexistent", ["no such file or directory", "[exit status 1]"], []),
    ("nosuchprogram", ["no such file or directory"], []),
    ("cat /etc", ["is a directory"], []),
    ("ls /mnt", ["hello.txt", "big.bin", "many/"], []),
    ("cat /mnt/hello.txt", ["Hello from ext2 on hda!"], []),
    ("cat /mnt2/hello.txt", ["Hello from ext2 on sda!"], []),
    ("echo x > /mnt/new", ["read-only file system"], []),
    ("hello a b c", ["Hello from user space!", "a b c"], []),
    ("ps", ["init", "sh", "ps"], []),
    ("free", ["total", "free"], []),
    ("uptime", ["up "], []),
    ("echo 'quoted  words'", ["quoted  words"], []),
    ("cd /nonexistent", ["no such file or directory"], []),
    ("cd /etc/motd", ["not a directory"], []),
    ("exit 3", ["init: shell exited with status 3", "Maleos shell"], []),
    ("echo the second shell works", ["the second shell works"], []),
    ("poweroff", ["powering off"], []),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("iso", nargs="?", default="build/maleos.iso")
    ap.add_argument("--mem", default=os.environ.get("MEM", "256M"))
    ap.add_argument("--cpu", default=None, help="QEMU -cpu model (e.g. max to exercise SMEP/SMAP)")
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--keep-disks", action="store_true")
    args = ap.parse_args()

    here = os.path.dirname(os.path.abspath(__file__))
    disks = os.environ.get("DISKS", "build/disks")
    if not args.keep_disks:
        subprocess.run([os.path.join(here, "mkdisks.sh"), disks], check=True, stdout=subprocess.DEVNULL)

    cmd = ["qemu-system-x86_64", "-cdrom", args.iso, "-m", args.mem, "-display", "none",
           "-serial", "stdio", "-monitor", "none", "-no-reboot",
           "-drive", f"file={disks}/ide.img,format=raw,if=ide,index=0",
           "-drive", f"file={disks}/sata.img,format=raw,if=none,id=sata0",
           "-device", "ich9-ahci,id=ahci", "-device", "ide-hd,drive=sata0,bus=ahci.0"]
    if args.cpu:
        cmd += ["-cpu", args.cpu]
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
    state = {"eof": False}

    def wait_for(needle, timeout, start):
        """Read output until `needle` appears after offset `start`. Returns the new offset or -1."""
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
                state["eof"] = True
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

    pos = wait_for("Maleos shell", 120, 0)
    if pos < 0:
        fail("the shell never started")
        print("".join(log)[-2000:])
        qemu.kill()
        sys.exit(1)
    pos = wait_for(PROMPT, 10, pos)

    for cmd_text, want, avoid in STEPS:
        qemu.stdin.write((cmd_text + "\n").encode())
        qemu.stdin.flush()
        if cmd_text == "poweroff":
            wait_for("powering off", 20, pos)
            break
        end = wait_for(PROMPT, 30, pos)
        if end < 0:
            fail(f"'{cmd_text}': no prompt came back")
            break
        out = "".join(log)[pos:end]
        bad = [w for w in want if w not in out] + [a for a in avoid if a in out]
        if bad:
            fail(f"'{cmd_text}': expected/forbidden text problem {bad!r}\n---\n{out}\n---")
        else:
            print(f"ok:   {cmd_text}")
        pos = end

    try:
        qemu.wait(timeout=20)
        powered_off = True
    except subprocess.TimeoutExpired:
        powered_off = False
        qemu.kill()
    if not powered_off:
        fail("QEMU did not power off after 'poweroff'")
    else:
        print("ok:   powered off cleanly")

    text = "".join(log)
    for bad in ("KERNEL PANIC", "EXCEPTION"):
        if bad in text:
            fail(f"kernel reported '{bad}'")
    print("PASS" if not failures else f"{failures} failure(s)")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
