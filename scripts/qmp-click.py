#!/usr/bin/env python3
"""Clica num ponto da tela do QEMU via QMP (mouse absoluto / usb-tablet).
Uso: qmp-click.py SOCKET X Y LARGURA ALTURA"""
import json
import socket
import sys
import time

sock_path, x, y, w, h = sys.argv[1], *map(int, sys.argv[2:6])
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(sock_path)
f = s.makefile("rw")
f.readline()  # saudacao


def cmd(name, args=None):
    msg = {"execute": name}
    if args:
        msg["arguments"] = args
    f.write(json.dumps(msg) + "\n")
    f.flush()
    while True:
        r = json.loads(f.readline())
        if "return" in r or "error" in r:
            return r


cmd("qmp_capabilities")
ax, ay = x * 32767 // (w - 1), y * 32767 // (h - 1)
cmd("input-send-event", {"events": [
    {"type": "abs", "data": {"axis": "x", "value": ax}},
    {"type": "abs", "data": {"axis": "y", "value": ay}}]})
time.sleep(0.3)
for down in (True, False):
    cmd("input-send-event", {"events": [
        {"type": "btn", "data": {"down": down, "button": "left"}}]})
    time.sleep(0.15)
