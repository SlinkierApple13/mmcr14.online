#!/usr/bin/env python3
"""Five-gates (过五关) end-to-end integration: room creation, team selection via
WS, ready gating, session start, and mode_state delivery."""
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
    def __init__(self, url, token, use_query_token=False):
        parsed_url = urllib.parse.urlparse(url)
        if use_query_token:
            qs = urllib.parse.parse_qsl(parsed_url.query, keep_blank_values=True)
            qs.append(("access_token", token))
            parsed_url = parsed_url._replace(query=urllib.parse.urlencode(qs))
        self._url = urllib.parse.urlparse(url)
        host = self._url.hostname or "127.0.0.1"
        port = self._url.port or 80
        self._socket = socket.create_connection((host, port), timeout=5)
        self._socket.settimeout(5)
        self._perform_handshake(host, port, token, use_query_token)

    def _perform_handshake(self, host, port, token, use_query_token):
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
        if not use_query_token:
            headers.insert(6, f"Authorization: Bearer {token}")
        self._socket.sendall("\r\n".join(headers).encode("utf-8"))
        response = b""
        while b"\r\n\r\n" not in response:
            chunk = self._socket.recv(4096)
            assert_true(chunk, "websocket handshake closed unexpectedly")
            response += chunk
        header_blob, _, _ = response.partition(b"\r\n\r\n")
        lines = header_blob.decode("utf-8").split("\r\n")
        assert_true(lines[0].startswith("HTTP/1.1 101"), f"unexpected websocket status line: {lines[0]}")

    def send_json(self, payload):
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

    def _read_json(self, timeout):
        self._socket.settimeout(timeout)
        header = self._read_exact(2)
        length = header[1] & 0x7F
        if length == 126:
            length = struct.unpack(">H", self._read_exact(2))[0]
        elif length == 127:
            length = struct.unpack(">Q", self._read_exact(8))[0]
        data = self._read_exact(length) if length else b""
        return json.loads(data.decode("utf-8"))

    def _read_exact(self, n):
        data = b""
        while len(data) < n:
            chunk = self._socket.recv(n - len(data))
            assert_true(chunk, "websocket closed unexpectedly")
            data += chunk
        return data

    def expect_json(self, predicate, timeout, description):
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise AssertionError(f"timed out waiting for {description}")
            try:
                message = self._read_json(remaining)
            except socket.timeout:
                continue
            if predicate(message):
                return message

    def drain(self, idle_timeout):
        self._socket.settimeout(idle_timeout)
        try:
            while True:
                self._read_json(idle_timeout)
        except socket.timeout:
            return

    def close(self):
        try:
            self._socket.close()
        except OSError:
            pass


def main():
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument("--server-binary", required=True)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=18198)
    args = parser.parse_args()

    host, port = args.host, args.port
    server_binary = pathlib.Path(args.server_binary)
    assert_true(server_binary.exists(), f"server binary not found: {server_binary}")

    temp_dir = tempfile.mkdtemp(prefix="mmcr14-fivegates-")
    db = pathlib.Path(temp_dir) / "fg.sqlite3"
    records = pathlib.Path(temp_dir) / "records"
    records.mkdir(parents=True, exist_ok=True)
    log_path = pathlib.Path(temp_dir) / "server.log"
    env = os.environ.copy()
    env["MMCR_BACKEND_BIND_ADDRESS"] = host
    env["MMCR_BACKEND_PORT"] = str(port)
    env["MMCR_BACKEND_THREADS"] = "1"
    env["MMCR_BACKEND_DB_PATH"] = str(db)
    env["MMCR_RECORDS_DIR"] = str(records)

    with log_path.open("w", encoding="utf-8") as server_log:
        process = subprocess.Popen([str(server_binary)],
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
                u = f"fg_e2e_{os.getpid()}_{i}"
                http_client.request("POST", "/api/v1/auth/register",
                                    body={"username": u, "password": "pass1234"}, expected_status=201)
                _, payload = http_client.request("POST", "/api/v1/auth/login",
                                                body={"identity": u, "password": "pass1234"},
                                                expected_status=200)
                tokens.append(payload["session"]["token"])

            # Create a pass-five-gates room.
            _, created = http_client.request("POST", "/api/v1/lobby/sessions", body={
                "game_config": {
                    "round_count": 0,
                    "unranked": True,
                    "mode": "pass_five_gates",
                    "mode_config": {"knockout_score": 0},
                }
            }, token=tokens[0], expected_status=201)
            session_id = created["session"]["summary"]["session_id"]
            assert_true(created["session"]["summary"].get("mode") == "pass_five_gates",
                        f"mode not in summary: {created}")
            print("room created:", session_id)

            game_sockets = [WebSocketClient(f"ws://{host}:{port}/ws/game", tok) for tok in tokens]
            try:
                # Join all players.
                for tok in tokens:
                    http_client.request("POST", f"/api/v1/lobby/sessions/{session_id}/join",
                                        token=tok, expected_status=200)

                # queue.ready without team selection must fail with an error envelope.
                game_sockets[0].send_json({"type": "queue.ready", "requestId": "r0",
                                           "payload": {"session_id": session_id, "ready": True}})
                err = game_sockets[0].expect_json(lambda m: m.get("requestId") == "r0", 2.0,
                                                  "ready-without-team error")
                assert_true(err.get("type") == "error", f"expected error envelope, got {err}")
                assert_true("team" in err.get("payload", {}).get("message", "").lower() or
                            "team" in str(err.get("payload", {})).lower(),
                            f"expected team error message, got {err}")
                print("ready without team rejected OK")

                # Select teams via queue.team (players 0,1 -> 虎; 2,3 -> 龙).
                for idx, team in [(0, 0), (1, 0), (2, 1), (3, 1)]:
                    game_sockets[idx].send_json({"type": "queue.team", "requestId": f"t{idx}",
                                                 "payload": {"session_id": session_id, "team": team}})
                    ack = game_sockets[idx].expect_json(lambda m: m.get("requestId") == f"t{idx}", 2.0,
                                                        f"team ack {idx}")
                    assert_true(ack.get("type") == "ack", f"expected ack for team, got {ack}")
                print("teams selected OK")

                # Ready all four; last one starts the session.
                for idx in range(3):
                    game_sockets[idx].send_json({"type": "queue.ready", "requestId": f"rd{idx}",
                                                 "payload": {"session_id": session_id, "ready": True}})
                game_sockets[3].send_json({"type": "queue.ready", "requestId": "rd3",
                                           "payload": {"session_id": session_id, "ready": True}})

                # Game sockets receive resume.required once the session starts.
                for idx in range(4):
                    game_sockets[idx].expect_json(lambda m: m.get("type") == "resume.required", 3.0,
                                                  f"resume.required {idx}")
                print("session started, all resume.required received")

                # The snapshot must carry mode_state with targets and teams.
                game_sockets[0].send_json({"type": "resume.ack", "requestId": "ra",
                                           "payload": {"session_id": session_id}})
                snap = game_sockets[0].expect_json(lambda m: m.get("type") == "session.snapshot", 3.0,
                                                   "session snapshot")
                mode_state = snap.get("payload", {}).get("mode_state", {})
                assert_true(mode_state.get("mode") == "pass_five_gates", f"bad mode_state: {mode_state}")
                targets = mode_state.get("targets", [])
                assert_true(len(targets) == 5, f"expected 5 targets, got {len(targets)}")
                teams = mode_state.get("teams", [])
                assert_true(len(teams) == 4, f"expected 4 team entries, got {len(teams)}")
                team_ids = sorted(int(entry["player_id"]) for entry in teams)
                assert_true(len(team_ids) == 4, f"team entries must cover 4 players: {teams}")
                print("mode_state OK: targets=", targets)
                print("ALL FIVE-GATES E2E STEPS PASSED")
            finally:
                for ws in game_sockets:
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
        print(f"five-gates e2e failed: {exc}", file=sys.stderr)
        raise
