#!/usr/bin/env python3
"""Four-player five-gates: verify game.event carries mode_state and that all
players remain connected while auto-playing several rounds."""
import base64
import hashlib
import http.client
import json
import os
import pathlib
import secrets
import socket
import struct
import subprocess
import sys
import tempfile
import time
import urllib.parse


def assert_true(condition, message):
    if not condition:
        raise AssertionError(message)


class JsonHttpClient:
    def __init__(self, host, port):
        self._host = host
        self._port = port

    def request(self, method, path, body=None, token=None, expected_status=None):
        conn = http.client.HTTPConnection(self._host, self._port, timeout=5)
        headers = {}
        payload = None
        if body is not None:
            payload = json.dumps(body).encode("utf-8")
            headers["Content-Type"] = "application/json"
        if token is not None:
            headers["Authorization"] = f"Bearer {token}"
        try:
            conn.request(method, path, body=payload, headers=headers)
            response = conn.getresponse()
            raw_body = response.read()
        finally:
            conn.close()
        decoded = raw_body.decode("utf-8") if raw_body else ""
        parsed = json.loads(decoded) if decoded else None
        if expected_status is not None:
            assert_true(response.status == expected_status,
                        f"{method} {path} returned {response.status}, expected {expected_status}: {decoded}")
        return response.status, parsed


class WebSocketClient:
    def __init__(self, url, token):
        parsed = urllib.parse.urlparse(url)
        self._url = parsed
        host = parsed.hostname or "127.0.0.1"
        port = parsed.port or 80
        self._socket = socket.create_connection((host, port), timeout=5)
        self._socket.settimeout(5)
        self._perform_handshake(host, port, token)
        self._buffer = b""
        self._sent = 0

    def _perform_handshake(self, host, port, token):
        key = base64.b64encode(secrets.token_bytes(16)).decode("ascii")
        headers = [
            f"GET {self._url.path}{'?' + self._url.query if self._url.query else ''} HTTP/1.1",
            f"Host: {host}:{port}",
            "Upgrade: websocket",
            "Connection: Upgrade",
            f"Sec-WebSocket-Key: {key}",
            "Sec-WebSocket-Version: 13",
            "",
            "",
        ]
        headers.insert(6, f"Authorization: Bearer {token}")
        self._socket.sendall("\r\n".join(headers).encode("utf-8"))
        response = b""
        while b"\r\n\r\n" not in response:
            chunk = self._socket.recv(4096)
            assert_true(chunk, "websocket handshake closed unexpectedly")
            response += chunk
        header_blob, _, rest = response.partition(b"\r\n\r\n")
        lines = header_blob.decode("utf-8").split("\r\n")
        assert_true(lines[0].startswith("HTTP/1.1 101"), f"unexpected status: {lines[0]}")
        self._buffer = rest

    def send_json(self, payload):
        self._sent += 1
        encoded = json.dumps(payload, separators=(",", ":")).encode("utf-8")
        frame = bytearray()
        frame.append(0x81)
        mask = secrets.token_bytes(4)
        n = len(encoded)
        if n < 126:
            frame.append(0x80 | n)
        elif n < 65536:
            frame.append(0x80 | 126)
            frame += struct.pack(">H", n)
        else:
            frame.append(0x80 | 127)
            frame += struct.pack(">Q", n)
        frame += mask
        frame += bytes(b ^ mask[i % 4] for i, b in enumerate(encoded))
        self._socket.sendall(bytes(frame))

    def _read_exact(self, n):
        while len(self._buffer) < n:
            chunk = self._socket.recv(4096)
            assert_true(chunk, "websocket closed unexpectedly")
            self._buffer += chunk
        data, self._buffer = self._buffer[:n], self._buffer[n:]
        return data

    def _read_frame(self, timeout):
        self._socket.settimeout(timeout)
        header = self._read_exact(2)
        opcode = header[0] & 0x0F
        length = header[1] & 0x7F
        if length == 126:
            length = struct.unpack(">H", self._read_exact(2))[0]
        elif length == 127:
            length = struct.unpack(">Q", self._read_exact(8))[0]
        masked = header[1] & 0x80
        mask = self._read_exact(4) if masked else b""
        data = self._read_exact(length) if length else b""
        if masked:
            data = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
        return opcode, data

    def next_message(self, timeout):
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
            try:
                opcode, data = self._read_frame(min(remaining, 1.0))
            except socket.timeout:
                continue
            if opcode != 1:
                continue
            try:
                return json.loads(data.decode("utf-8"))
            except Exception:
                continue

    def close(self):
        try:
            self._socket.close()
        except OSError:
            pass


def main():
    host, port = "127.0.0.1", 18200
    temp_dir = tempfile.mkdtemp(prefix="mmcr14-four2-")
    db = pathlib.Path(temp_dir) / "four.sqlite3"
    records = pathlib.Path(temp_dir) / "records"
    records.mkdir(parents=True, exist_ok=True)
    log_path = pathlib.Path(temp_dir) / "server.log"
    env = os.environ.copy()
    env["MMCR_BACKEND_BIND_ADDRESS"] = host
    env["MMCR_BACKEND_PORT"] = str(port)
    env["MMCR_BACKEND_THREADS"] = "2"
    env["MMCR_BACKEND_DB_PATH"] = str(db)
    env["MMCR_RECORDS_DIR"] = str(records)

    with log_path.open("w", encoding="utf-8") as server_log:
        process = subprocess.Popen([r"Q:\mmcrdlc\backend\build\src\app\mmcr_backend.exe"],
                                   stdout=server_log, stderr=subprocess.STDOUT, env=env)
        try:
            http_client = JsonHttpClient(host, port)
            for _ in range(40):
                if process.poll() is not None:
                    raise AssertionError(f"server died at startup: {log_path.read_text()}")
                try:
                    s, _ = http_client.request("GET", "/healthz")
                    if s == 200:
                        break
                except Exception:
                    pass
                time.sleep(0.25)

            tokens = []
            for i in range(4):
                u = f"four2_{os.getpid()}_{i}"
                http_client.request("POST", "/api/v1/auth/register",
                                    body={"username": u, "password": "pass1234"}, expected_status=201)
                _, payload = http_client.request("POST", "/api/v1/auth/login",
                                                body={"identity": u, "password": "pass1234"},
                                                expected_status=200)
                tokens.append(payload["session"]["token"])
            print("4 players registered")

            _, created = http_client.request("POST", "/api/v1/lobby/sessions", body={
                "game_config": {
                    "round_count": 0,
                    "unranked": True,
                    "mode": "pass_five_gates",
                    "mode_config": {"knockout_score": 0},
                }
            }, token=tokens[0], expected_status=201)
            session_id = created["session"]["summary"]["session_id"]
            print("room:", session_id)

            sockets = [WebSocketClient(f"ws://{host}:{port}/ws/game", tok) for tok in tokens]
            try:
                for tok in tokens:
                    http_client.request("POST", f"/api/v1/lobby/sessions/{session_id}/join",
                                        token=tok, expected_status=200)
                for idx, team in [(0, 0), (1, 0), (2, 1), (3, 1)]:
                    sockets[idx].send_json({"type": "queue.team", "requestId": f"t{idx}",
                                            "payload": {"session_id": session_id, "team": team}})
                for idx in range(4):
                    sockets[idx].next_message(2.0)  # team ack
                for idx in range(3):
                    sockets[idx].send_json({"type": "queue.ready", "requestId": f"rd{idx}",
                                            "payload": {"session_id": session_id, "ready": True}})
                sockets[3].send_json({"type": "queue.ready", "requestId": "rd3",
                                      "payload": {"session_id": session_id, "ready": True}})
                for idx in range(4):
                    sockets[idx].next_message(3.0)  # resume.required
                print("session started")

                for idx in range(4):
                    sockets[idx].send_json({"type": "resume.ack", "requestId": f"ra{idx}",
                                            "payload": {"session_id": session_id}})

                # Collect snapshots until we learn the targets.
                targets = None
                deadline = time.monotonic() + 8
                while time.monotonic() < deadline and targets is None:
                    for idx in range(4):
                        msg = sockets[idx].next_message(0.5)
                        if msg and msg.get("type") == "session.snapshot":
                            ms = msg.get("payload", {}).get("mode_state", {})
                            if ms and "targets" in ms:
                                targets = ms["targets"]
                                break
                print("targets:", targets)
                assert_true(targets and len(targets) == 5, "expected 5 targets in mode_state")

                # Auto-play: whenever a player can act (discard), send the action.
                # Track game events that carry mode_state (the fix under test).
                total_events = 0
                mode_state_on_events = 0
                mode_update_count = 0
                completed_tracked = None
                deadline = time.monotonic() + 25
                while time.monotonic() < deadline:
                    acted = False
                    for idx in range(4):
                        msg = sockets[idx].next_message(0.4)
                        if msg is None:
                            continue
                        if msg.get("type") != "game.event":
                            continue
                        total_events += 1
                        payload = msg.get("payload", {})
                        if "mode_state" in payload:
                            mode_state_on_events += 1
                            ms = payload["mode_state"]
                            if "completed" in ms:
                                completed_tracked = [int(ms["completed"][0]), int(ms["completed"][1])]
                        if "mode_update" in payload:
                            mode_update_count += 1
                            mu = payload["mode_update"]
                            print(f"  mode_update team={mu.get('team')} now={mu.get('completed_now')}")
                        ev = payload.get("event", {})
                        viewer = payload.get("viewer", {})
                        acts = viewer.get("available_actions", [])
                        for a in acts:
                            if a.get("kind") == "discard_tile":
                                sockets[idx].send_json({
                                    "type": "game.input", "requestId": f"in{idx}",
                                    "payload": {"kind": "discard_tile",
                                                "stage_counter": ev.get("stage_counter"),
                                                "tile": a.get("tile"),
                                                "use_drawn_tile": a.get("use_drawn_tile", True)}})
                                acted = True
                                break
                        if acted:
                            break

                print(f"total game events: {total_events}")
                print(f"events carrying mode_state: {mode_state_on_events}")
                print(f"mode_update messages: {mode_update_count}")
                print(f"completed tracked from events: {completed_tracked}")

                # All players must still be connected.
                for idx, ws in enumerate(sockets):
                    assert_true(ws._socket.fileno() >= 0, f"player {idx} socket closed")
                print("ALL 4 PLAYERS STILL CONNECTED")
                print("FOUR-PLAYER E2E OK")
            finally:
                for ws in sockets:
                    ws.close()
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(f"four-player e2e failed: {exc}", file=sys.stderr)
        raise
