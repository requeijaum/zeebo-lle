#!/usr/bin/env python3
"""
Test client for Zeebo LLE Control Server
"""
import socket
import json
import time
import subprocess

def test_control():
    proc = subprocess.Popen(
        ["./zeebo_lle_main", "--headless", "--control-port=48998"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True
    )
    time.sleep(0.5)

    try:
        s = socket.create_connection(("127.0.0.1", 48998), timeout=5)
        def rpc(cmd_dict):
            s.sendall((json.dumps(cmd_dict) + "\n").encode())
            data = b""
            while b"\n" not in data:
                chunk = s.recv(1024)
                if not chunk: break
                data += chunk
            return json.loads(data.decode().strip())

        print("Testing ping:", rpc({"cmd": "ping"}))
        print("Testing state:", rpc({"cmd": "state"}))
        print("Testing pause:", rpc({"cmd": "pause"}))
        print("Testing state (paused):", rpc({"cmd": "state"}))
        print("Testing reg 15 (PC core 0):", rpc({"cmd": "reg", "core": 0, "n": 15}))
        print("Testing reg 15 (PC core 1):", rpc({"cmd": "reg", "core": 1, "n": 15}))
        print("Testing read core0 @ 0xb0000000:", rpc({"cmd": "read", "core": 0, "addr": 0xb0000000, "len": 16}))
        print("Testing cont:", rpc({"cmd": "cont"}))
        print("Testing bp (set at 0xb0003424):", rpc({"cmd": "bp", "core": 0, "addr": 0xb0003424}))
        print("Testing bpclear (clear 0xb0003424):", rpc({"cmd": "bpclear", "core": 0, "addr": 0xb0003424}))
        print("Testing quit:", rpc({"cmd": "quit"}))
        s.close()
    finally:
        proc.terminate()
        proc.wait()

if __name__ == "__main__":
    test_control()
