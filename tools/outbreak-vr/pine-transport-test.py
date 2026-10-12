#!/usr/bin/env python3
# Outbreak VR (Steam Frame): checks pine.py's transport against a fake PINE server, with no emulator.
#
# pine.py reaches PINE where pcsx2/PINE.cpp listens: TCP 127.0.0.1:<slot> on Windows, and on Linux a Unix socket
# $XDG_RUNTIME_DIR/pcsx2.sock (/tmp/pcsx2.sock without XDG_RUNTIME_DIR), ".<slot>" added when the slot is not 28011.
# This starts a small server that answers MsgStatus, MsgID and MsgRead32 the way PINE does, and checks that pine.py
# finds it at the default slot, at another slot, through PINE_SOCKET, and over TCP (PINE_TCP=1). The Unix socket
# cases need AF_UNIX (Linux, macOS); elsewhere they are skipped and said so.
#
# Usage: python3 tools/outbreak-vr/pine-transport-test.py     Exit: 0 passed, 1 failed.

import importlib.util
import os
import socket
import struct
import sys
import tempfile
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("pine", os.path.join(HERE, "pine.py"))
pine = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pine)

GAME_ID = "SLPM-65428"
failures = []


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        failures.append(what)


def recv_exact(conn, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = conn.recv(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return bytes(buf)


def serve(listener):
    """One client, PINE's framing: u32 total length, then commands; the reply is u32 total length, u8 result, replies."""
    conn, _ = listener.accept()
    with conn:
        while True:
            head = recv_exact(conn, 4)
            if head is None:
                return
            body = recv_exact(conn, struct.unpack("<I", head)[0] - 4)
            if body is None:
                return
            out, i, ok = b"", 0, True
            while i < len(body):
                op = body[i]
                i += 1
                if op == pine.MSG_STATUS:
                    out += struct.pack("<I", 0)
                elif op == pine.MSG_ID:
                    s = GAME_ID.encode() + b"\0"
                    out += struct.pack("<I", len(s)) + s
                elif op == pine.MSG_READ32:
                    addr = struct.unpack_from("<I", body, i)[0]
                    i += 4
                    out += struct.pack("<I", addr ^ 0x5A5A5A5A)
                else:
                    ok = False
                    break
            reply = (b"\x00" + out) if ok else b"\xff"
            conn.sendall(struct.pack("<I", len(reply) + 4) + reply)


def start_unix(path):
    if os.path.exists(path):
        os.unlink(path)
    listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    listener.bind(path)
    listener.listen(1)
    threading.Thread(target=serve, args=(listener,), daemon=True).start()
    return listener


def start_tcp():
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.bind(("127.0.0.1", 0))
    listener.listen(1)
    threading.Thread(target=serve, args=(listener,), daemon=True).start()
    return listener, listener.getsockname()[1]


def talk(what, **kw):
    try:
        with pine.Pine(**kw) as p:
            check(p.status() == "running" and p.game_id() == GAME_ID and p.read32(0x1000) == 0x1000 ^ 0x5A5A5A5A,
                  "%s: %s answers MsgStatus, MsgID and MsgRead32" % (what, pine.describe_endpoint(p.endpoint)))
    except (OSError, pine.PineError) as e:
        check(False, "%s: %s" % (what, e))


def clear_env():
    for k in ("PINE_SOCKET", "PINE_TCP", "PINE_HOST"):
        os.environ.pop(k, None)


clear_env()
saved_xdg = os.environ.get("XDG_RUNTIME_DIR")

# The slot rule, whatever the platform: no suffix for the default slot, ".<slot>" for any other.
os.environ["XDG_RUNTIME_DIR"] = "/run/user/1000"
if sys.platform != "darwin":
    check(pine.unix_socket_path() == "/run/user/1000/pcsx2.sock", "default slot: $XDG_RUNTIME_DIR/pcsx2.sock")
    check(pine.unix_socket_path(28012) == "/run/user/1000/pcsx2.sock.28012", "slot 28012: pcsx2.sock.28012")
    del os.environ["XDG_RUNTIME_DIR"]
    check(pine.unix_socket_path() == "/tmp/pcsx2.sock", "no XDG_RUNTIME_DIR: /tmp/pcsx2.sock")
os.environ["PINE_SOCKET"] = "/x/y.sock"
check(pine.endpoint() == ("unix", "/x/y.sock"), "PINE_SOCKET names the socket")
clear_env()
os.environ["PINE_TCP"] = "1"
check(pine.endpoint(28013) == ("tcp", ("127.0.0.1", 28013)), "PINE_TCP=1: TCP 127.0.0.1:<slot>")
clear_env()
os.environ["PINE_HOST"] = "10.0.0.5"
check(pine.endpoint() == ("tcp", ("10.0.0.5", 28011)), "PINE_HOST: TCP <host>:<slot>")
clear_env()
expected_kind = "tcp" if (os.name == "nt" or not hasattr(socket, "AF_UNIX")) else "unix"
check(pine.endpoint()[0] == expected_kind, "no override: the platform's own transport (%s)" % expected_kind)

# Talking to a server: TCP everywhere, the Unix socket where there is AF_UNIX.
listener, port = start_tcp()
os.environ["PINE_TCP"] = "1"
talk("TCP (PINE_TCP=1)", slot=port)
listener.close()
clear_env()

if hasattr(socket, "AF_UNIX") and os.name != "nt":
    with tempfile.TemporaryDirectory() as run_dir:
        os.environ["XDG_RUNTIME_DIR"] = run_dir
        listener = start_unix(os.path.join(run_dir, "pcsx2.sock"))
        talk("unix, default slot")
        listener.close()
        listener = start_unix(os.path.join(run_dir, "pcsx2.sock.28012"))
        talk("unix, slot 28012", slot=28012)
        listener.close()
        other = os.path.join(run_dir, "elsewhere.sock")
        listener = start_unix(other)
        os.environ["PINE_SOCKET"] = other
        talk("unix, PINE_SOCKET")
        listener.close()
        clear_env()
        try:
            pine.wait_for_pine(slot=28099, seconds=1)
            check(False, "wait_for_pine with no server raises")
        except pine.PineError as e:
            check("pcsx2.sock.28099" in str(e), "wait_for_pine with no server names the socket: %s" % e)
else:
    print("skip the Unix socket cases: no AF_UNIX on this platform")

if saved_xdg is None:
    os.environ.pop("XDG_RUNTIME_DIR", None)
else:
    os.environ["XDG_RUNTIME_DIR"] = saved_xdg

if failures:
    print("pine-transport-test: FAILED (%d)" % len(failures))
    sys.exit(1)
print("pine-transport-test: passed")
