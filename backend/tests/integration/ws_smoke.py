#!/usr/bin/env python3

import argparse
import base64
import hashlib
import http.client
import json
import os
import pathlib
import secrets
import signal
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
        connection = http.client.HTTPConnection(self._host, self._port, timeout=5)
        headers = {}
        payload = None
        if body is not None:
            payload = json.dumps(body).encode("utf-8")
            headers["Content-Type"] = "application/json"
        if token is not None:
            headers["Authorization"] = f"Bearer {token}"

        try:
            connection.request(method, path, body=payload, headers=headers)
            response = connection.getresponse()
            raw_body = response.read()
        finally:
            connection.close()

        decoded_body = raw_body.decode("utf-8") if raw_body else ""
        parsed_body = json.loads(decoded_body) if decoded_body else None
        if expected_status is not None:
            assert_true(
                response.status == expected_status,
                f"{method} {path} returned {response.status}, expected {expected_status}: {decoded_body}",
            )
        return response.status, parsed_body


class WebSocketClient:
    def __init__(self, url, token=None, use_query_token=False):
        parsed_url = urllib.parse.urlparse(url)
        if use_query_token and token:
            query_pairs = urllib.parse.parse_qsl(parsed_url.query, keep_blank_values=True)
            query_pairs.append(("access_token", token))
            url = parsed_url._replace(query=urllib.parse.urlencode(query_pairs)).geturl()
        self._url = urllib.parse.urlparse(url)
        assert_true(self._url.scheme == "ws", f"unsupported websocket scheme: {self._url.scheme}")
        host = self._url.hostname or "127.0.0.1"
        port = self._url.port or 80
        self._socket = socket.create_connection((host, port), timeout=5)
        self._socket.settimeout(5)
        self._perform_handshake(host, port, token, use_query_token)

    def _perform_handshake(self, host, port, token, use_query_token):
        websocket_key = base64.b64encode(secrets.token_bytes(16)).decode("ascii")
        headers = [
            f"GET {self._url.path}{'?' + self._url.query if self._url.query else ''} HTTP/1.1",
            f"Host: {host}:{port}",
            "Upgrade: websocket",
            "Connection: Upgrade",
            f"Sec-WebSocket-Key: {websocket_key}",
            "Sec-WebSocket-Version: 13",
            "",
            "",
        ]
        if not use_query_token and token:
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
        headers = {}
        for line in lines[1:]:
            if not line:
                continue
            name, value = line.split(":", 1)
            headers[name.strip().lower()] = value.strip()

        expected_accept = base64.b64encode(
            hashlib.sha1((websocket_key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode("utf-8")).digest()
        ).decode("ascii")
        assert_true(headers.get("sec-websocket-accept") == expected_accept, "invalid websocket accept header")

    def close(self):
        try:
            self._send_frame(0x8, b"")
        except OSError:
            pass
        try:
            self._socket.close()
        except OSError:
            pass

    def send_json(self, payload):
        encoded = json.dumps(payload, separators=(",", ":")).encode("utf-8")
        self._send_frame(0x1, encoded)

    def expect_json(self, predicate, timeout, description):
        deadline = time.monotonic() + timeout
        seen = []
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise AssertionError(f"timed out waiting for {description}; seen={seen}")
            message, received_at = self._read_json(remaining)
            seen.append(message.get("type"))
            if predicate(message):
                return message, received_at

    def drain(self, idle_timeout):
        messages = []
        while True:
            try:
                message, _ = self._read_json(idle_timeout)
                messages.append(message)
            except socket.timeout:
                return messages

    def expect_no_message(self, timeout, description):
        try:
            message, _ = self._read_json(timeout)
        except socket.timeout:
            return
        raise AssertionError(f"expected no message for {description}, got {message}")

    def _read_json(self, timeout):
        self._socket.settimeout(timeout)
        while True:
            opcode, payload = self._read_frame()
            if opcode == 0x9:
                self._send_frame(0xA, payload)
                continue
            if opcode == 0xA:
                continue
            if opcode == 0x8:
                raise AssertionError("websocket closed unexpectedly")
            assert_true(opcode == 0x1, f"unexpected websocket opcode: {opcode}")
            decoded = payload.decode("utf-8")
            return json.loads(decoded), time.monotonic()

    def _read_frame(self):
        header = self._read_exact(2)
        first_byte, second_byte = header[0], header[1]
        opcode = first_byte & 0x0F
        masked = (second_byte & 0x80) != 0
        payload_length = second_byte & 0x7F
        if payload_length == 126:
            payload_length = struct.unpack("!H", self._read_exact(2))[0]
        elif payload_length == 127:
            payload_length = struct.unpack("!Q", self._read_exact(8))[0]

        mask_key = self._read_exact(4) if masked else b""
        payload = self._read_exact(payload_length)
        if masked:
            payload = bytes(byte ^ mask_key[index % 4] for index, byte in enumerate(payload))
        return opcode, payload

    def _send_frame(self, opcode, payload):
        first_byte = 0x80 | (opcode & 0x0F)
        mask_key = secrets.token_bytes(4)
        payload_length = len(payload)
        header = bytearray([first_byte])
        if payload_length < 126:
            header.append(0x80 | payload_length)
        elif payload_length < (1 << 16):
            header.append(0x80 | 126)
            header.extend(struct.pack("!H", payload_length))
        else:
            header.append(0x80 | 127)
            header.extend(struct.pack("!Q", payload_length))

        masked_payload = bytes(byte ^ mask_key[index % 4] for index, byte in enumerate(payload))
        self._socket.sendall(bytes(header) + mask_key + masked_payload)

    def _read_exact(self, length):
        chunks = bytearray()
        while len(chunks) < length:
            chunk = self._socket.recv(length - len(chunks))
            if not chunk:
                raise AssertionError("websocket connection closed while reading frame")
            chunks.extend(chunk)
        return bytes(chunks)


def wait_for_server(http_client, timeout):
    deadline = time.monotonic() + timeout
    last_error = None
    while time.monotonic() < deadline:
        try:
            http_client.request("GET", "/healthz", expected_status=200)
            return
        except Exception as exc:  # noqa: BLE001
            last_error = exc
            time.sleep(0.1)
    raise AssertionError(f"server did not become healthy: {last_error}")


def register_and_login(http_client, username, password):
    http_client.request(
        "POST",
        "/api/v1/auth/register",
        body={"username": username, "password": password},
        expected_status=201,
    )
    _, payload = http_client.request(
        "POST",
        "/api/v1/auth/login",
        body={"identity": username, "password": password},
        expected_status=200,
    )
    return payload["session"]["token"]


def wait_for_snapshot_with_session(ws_client, session_id, timeout):
    def predicate(message):
        if message.get("type") != "lobby.list.snapshot":
            return False
        sessions = message.get("payload", {}).get("sessions", [])
        return any(session.get("session_id") == session_id for session in sessions)

    return ws_client.expect_json(predicate, timeout, f"lobby snapshot containing session {session_id}")


def wait_for_snapshot_without_session(ws_client, session_id, timeout):
    def predicate(message):
        if message.get("type") != "lobby.list.snapshot":
            return False
        sessions = message.get("payload", {}).get("sessions", [])
        return all(session.get("session_id") != session_id for session in sessions)

    return ws_client.expect_json(predicate, timeout, f"lobby snapshot without session {session_id}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--server-binary", required=True)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=18181)
    parser.add_argument("--network-delay-ms", type=int, default=100)
    args = parser.parse_args()

    server_binary = pathlib.Path(args.server_binary)
    assert_true(server_binary.exists(), f"server binary not found: {server_binary}")

    with tempfile.TemporaryDirectory(prefix="mmcr14-ws-smoke-") as temp_dir:
        database_path = pathlib.Path(temp_dir) / "smoke.sqlite3"
        records_path = pathlib.Path(temp_dir) / "records"
        records_path.mkdir(parents=True, exist_ok=True)
        server_log_path = pathlib.Path(temp_dir) / "server.log"

        env = os.environ.copy()
        env["MMCR_BACKEND_BIND_ADDRESS"] = args.host
        env["MMCR_BACKEND_PORT"] = str(args.port)
        env["MMCR_BACKEND_THREADS"] = "1"
        env["MMCR_BACKEND_DB_PATH"] = str(database_path)
        env["MMCR_RECORDS_DIR"] = str(records_path)

        with server_log_path.open("w", encoding="utf-8") as server_log:
            process = subprocess.Popen(
                [str(server_binary)],
                stdout=server_log,
                stderr=subprocess.STDOUT,
                env=env,
            )
            try:
                http_client = JsonHttpClient(args.host, args.port)
                wait_for_server(http_client, timeout=10)

                tokens = []
                for index in range(5):
                    username = f"ws_smoke_{os.getpid()}_{index}"
                    tokens.append(register_and_login(http_client, username, "pass1234"))

                lobby_ws = WebSocketClient(f"ws://{args.host}:{args.port}/ws/lobby", tokens[4])
                browser_lobby_ws = WebSocketClient(
                    f"ws://{args.host}:{args.port}/ws/lobby", tokens[4], use_query_token=True
                )
                game_sockets = [
                    WebSocketClient(f"ws://{args.host}:{args.port}/ws/game", token)
                    for token in tokens[:4]
                ]
                spectator_ws = None
                denied_spectator_ws = None

                try:
                    initial_lobby_message, _ = lobby_ws.expect_json(
                        lambda message: message.get("type") == "lobby.list.snapshot",
                        2.0,
                        "initial lobby snapshot",
                    )
                    assert_true(
                        initial_lobby_message.get("payload", {}).get("sessions") == [],
                        f"unexpected initial lobby sessions: {initial_lobby_message}",
                    )

                    browser_lobby_message, _ = browser_lobby_ws.expect_json(
                        lambda message: message.get("type") == "lobby.list.snapshot",
                        2.0,
                        "initial browser-compatible lobby snapshot",
                    )
                    assert_true(
                        browser_lobby_message.get("payload", {}).get("sessions") == [],
                        f"unexpected browser-compatible lobby sessions: {browser_lobby_message}",
                    )

                    for index, game_ws in enumerate(game_sockets):
                        game_ws.expect_no_message(0.2, f"initial game socket bootstrap for player {index}")

                    lobby_ws.send_json({"type": "lobby.list", "requestId": "watch-lobby", "payload": {}})
                    ack_message, _ = lobby_ws.expect_json(
                        lambda message: message.get("requestId") == "watch-lobby",
                        2.0,
                        "lobby watch ack",
                    )
                    assert_true(
                        ack_message.get("type") == "ack",
                        f"expected ack after lobby.list, got {ack_message}",
                    )
                    lobby_ws.drain(0.05)

                    lobby_ws.send_json({"type": "game.input", "requestId": "bad-lobby", "payload": {}})
                    lobby_error, _ = lobby_ws.expect_json(
                        lambda message: message.get("requestId") == "bad-lobby",
                        2.0,
                        "wrong-socket error on lobby websocket",
                    )
                    assert_true(lobby_error.get("type") == "error", f"unexpected lobby error envelope: {lobby_error}")
                    assert_true(
                        lobby_error.get("payload", {}).get("code") == "wrong_socket",
                        f"unexpected lobby wrong-socket payload: {lobby_error}",
                    )

                    game_sockets[0].send_json({"type": "lobby.list", "requestId": "bad-game", "payload": {}})
                    game_error, _ = game_sockets[0].expect_json(
                        lambda message: message.get("requestId") == "bad-game",
                        2.0,
                        "wrong-socket error on game websocket",
                    )
                    assert_true(game_error.get("type") == "error", f"unexpected game error envelope: {game_error}")
                    assert_true(
                        game_error.get("payload", {}).get("code") == "wrong_socket",
                        f"unexpected game wrong-socket payload: {game_error}",
                    )

                    _, created = http_client.request(
                        "POST",
                        "/api/v1/lobby/sessions",
                        token=tokens[0],
                        expected_status=201,
                    )
                    session_id = created["session"]["summary"]["session_id"]

                    queued_snapshot, _ = wait_for_snapshot_with_session(lobby_ws, session_id, 2.0)
                    queued_sessions = queued_snapshot.get("payload", {}).get("sessions", [])
                    assert_true(
                        any(session.get("session_id") == session_id for session in queued_sessions),
                        f"session {session_id} was not visible on the lobby websocket",
                    )

                    browser_queued_snapshot, _ = wait_for_snapshot_with_session(
                        browser_lobby_ws, session_id, 2.0
                    )
                    browser_queued_sessions = browser_queued_snapshot.get("payload", {}).get("sessions", [])
                    assert_true(
                        any(session.get("session_id") == session_id for session in browser_queued_sessions),
                        f"session {session_id} was not visible on the browser-compatible lobby websocket",
                    )

                    for token in tokens[:4]:
                        http_client.request(
                            "POST",
                            f"/api/v1/lobby/sessions/{session_id}/join",
                            token=token,
                            expected_status=200,
                        )

                    game_sockets[0].drain(0.5)
                    lobby_ws.drain(0.05)

                    for token in tokens[:3]:
                        http_client.request(
                            "POST",
                            f"/api/v1/lobby/sessions/{session_id}/ready",
                            body={"ready": True},
                            token=token,
                            expected_status=200,
                        )

                    lobby_ws.drain(0.05)
                    start_time = time.monotonic()
                    http_client.request(
                        "POST",
                        f"/api/v1/lobby/sessions/{session_id}/ready",
                        body={"ready": True},
                        token=tokens[3],
                        expected_status=200,
                    )

                    lobby_after_start, lobby_received_at = wait_for_snapshot_without_session(
                        lobby_ws, session_id, 2.0
                    )
                    assert_true(
                        lobby_after_start.get("type") == "lobby.list.snapshot",
                        f"unexpected lobby start update: {lobby_after_start}",
                    )

                    resume_required, resume_received_at = game_sockets[0].expect_json(
                        lambda message: message.get("type") == "resume.required",
                        3.0,
                        "delayed resume.required on game websocket",
                    )
                    assert_true(
                        resume_required.get("payload", {}).get("session_id") == session_id,
                        f"unexpected resume.required payload: {resume_required}",
                    )

                    lobby_elapsed_ms = (lobby_received_at - start_time) * 1000.0
                    resume_elapsed_ms = (resume_received_at - start_time) * 1000.0
                    assert_true(
                        resume_elapsed_ms >= args.network_delay_ms - 75,
                        f"resume.required arrived too early: {resume_elapsed_ms:.1f}ms < {args.network_delay_ms}ms",
                    )
                    assert_true(
                        lobby_elapsed_ms < args.network_delay_ms,
                        f"lobby update should stay immediate relative to game delay: lobby={lobby_elapsed_ms:.1f}ms delay={args.network_delay_ms}ms",
                    )

                    unauthorized_spectator_ws = WebSocketClient(
                        f"ws://{args.host}:{args.port}/ws/spectate?session_id={session_id}"
                    )
                    unauthorized_error, _ = unauthorized_spectator_ws.expect_json(
                        lambda message: message.get("type") == "error"
                        and message.get("payload", {}).get("code") == "unauthorized",
                        2.0,
                        "spectator auth rejection",
                    )
                    assert_true(
                        unauthorized_error.get("payload", {}).get("code") == "unauthorized",
                        f"unexpected unauthorized spectate response: {unauthorized_error}",
                    )
                    unauthorized_spectator_ws.close()

                    spectator_ws = WebSocketClient(
                        f"ws://{args.host}:{args.port}/ws/spectate?session_id={session_id}",
                        tokens[4],
                    )
                    spectator_snapshot, _ = spectator_ws.expect_json(
                        lambda message: message.get("type") == "session.snapshot",
                        2.0,
                        "spectator snapshot",
                    )
                    spectator_payload = spectator_snapshot.get("payload", {})
                    assert_true(
                        spectator_payload.get("spectator") is True,
                        f"spectator marker missing: {spectator_snapshot}",
                    )
                    for seat in spectator_payload.get("seats", []):
                        assert_true("hand_tiles" not in seat, f"spectator received concealed hand tiles: {seat}")
                        assert_true("drawn_tile" not in seat, f"spectator received a concealed draw: {seat}")
                    viewer = spectator_payload.get("viewer", {})
                    assert_true(viewer.get("available_actions") == [], f"spectator received actions: {viewer}")
                    assert_true(viewer.get("wait_data") is None, f"spectator received wait data: {viewer}")

                    spectator_ws.send_json(
                        {
                            "type": "spectator.perspective",
                            "requestId": "perspective-seat-two",
                            "payload": {"seat_index": 2},
                        }
                    )
                    shifted_snapshot, _ = spectator_ws.expect_json(
                        lambda message: message.get("type") == "session.snapshot"
                        and message.get("requestId") == "perspective-seat-two",
                        2.0,
                        "shifted spectator perspective",
                    )
                    assert_true(
                        shifted_snapshot.get("payload", {}).get("viewer", {}).get("seat_index") == 2,
                        f"spectator perspective did not shift: {shifted_snapshot}",
                    )
                    for seat in shifted_snapshot.get("payload", {}).get("seats", []):
                        assert_true("hand_tiles" not in seat, f"shifted snapshot leaked hand tiles: {seat}")
                        assert_true("drawn_tile" not in seat, f"shifted snapshot leaked a draw: {seat}")

                    spectator_ws.send_json(
                        {"type": "game.input", "requestId": "spectator-write", "payload": {}}
                    )
                    spectator_error, _ = spectator_ws.expect_json(
                        lambda message: message.get("requestId") == "spectator-write",
                        2.0,
                        "spectator read-only rejection",
                    )
                    assert_true(
                        spectator_error.get("payload", {}).get("code") == "spectator_read_only",
                        f"unexpected spectator write response: {spectator_error}",
                    )

                    spectator_ws.send_json(
                        {
                            "type": "spectator.hand.request",
                            "requestId": "request-seat-zero",
                            "payload": {"seat_index": 0},
                        }
                    )
                    pending_hand, _ = spectator_ws.expect_json(
                        lambda message: message.get("type") == "spectator.hand.pending",
                        2.0,
                        "spectator hand request pending",
                    )
                    assert_true(
                        pending_hand.get("payload", {}).get("seat_index") == 0,
                        f"unexpected pending hand response: {pending_hand}",
                    )
                    game_sockets[0].close()
                    game_sockets[0] = WebSocketClient(
                        f"ws://{args.host}:{args.port}/ws/game?session_id={session_id}",
                        tokens[0],
                    )
                    player_hand_request, _ = game_sockets[0].expect_json(
                        lambda message: message.get("type") == "spectator.hand.request",
                        2.0,
                        "replayed player hand consent request",
                    )
                    hand_request_id = player_hand_request.get("payload", {}).get("request_id")
                    assert_true(isinstance(hand_request_id, str), f"missing hand request id: {player_hand_request}")
                    game_sockets[0].send_json(
                        {
                            "type": "spectator.hand.respond",
                            "requestId": "approve-spectator",
                            "payload": {"request_id": hand_request_id, "approved": True},
                        }
                    )
                    approval, _ = spectator_ws.expect_json(
                        lambda message: message.get("type") == "spectator.hand.result",
                        2.0,
                        "spectator hand approval",
                    )
                    assert_true(
                        approval.get("payload", {}).get("approved") is True,
                        f"unexpected approval response: {approval}",
                    )
                    revealed_hand, _ = spectator_ws.expect_json(
                        lambda message: message.get("type") == "spectator.hand.update",
                        2.0,
                        "approved spectator hand update",
                    )
                    revealed_payload = revealed_hand.get("payload", {})
                    assert_true(revealed_payload.get("seat_index") == 0, f"wrong revealed seat: {revealed_hand}")
                    if revealed_payload.get("stage_counter", 0) == 0:
                        # Pre-deal: the consent flow runs before any tiles exist.
                        assert_true(
                            revealed_payload.get("hand_tiles") == []
                            and revealed_payload.get("drawn_tile") is None,
                            f"unexpected pre-deal hand payload: {revealed_hand}",
                        )
                    else:
                        assert_true(
                            isinstance(revealed_payload.get("hand_tiles"), list)
                            and len(revealed_payload["hand_tiles"]) > 0,
                            f"approved hand tiles missing: {revealed_hand}",
                        )

                    spectator_ws.send_json(
                        {
                            "type": "spectator.hand.refresh",
                            "requestId": "refresh-approved-hand",
                            "payload": {},
                        }
                    )
                    refreshed_hand, _ = spectator_ws.expect_json(
                        lambda message: message.get("type") == "spectator.hand.update"
                        and message.get("requestId") == "refresh-approved-hand",
                        2.0,
                        "approved spectator hand refresh",
                    )
                    assert_true(
                        refreshed_hand.get("payload", {}).get("seat_index") == 0,
                        f"unexpected refreshed hand: {refreshed_hand}",
                    )

                    spectator_ws.send_json(
                        {
                            "type": "spectator.hand.request",
                            "requestId": "rate-limited-request",
                            "payload": {"seat_index": 1},
                        }
                    )
                    rate_error, _ = spectator_ws.expect_json(
                        lambda message: message.get("requestId") == "rate-limited-request",
                        2.0,
                        "spectator rate limit rejection",
                    )
                    assert_true(
                        rate_error.get("type") == "error"
                        and rate_error.get("payload", {}).get("code") == "rate_limited",
                        f"unexpected rate limit response: {rate_error}",
                    )

                    game_sockets[0].send_json(
                        {
                            "type": "spectator.hand.revoke",
                            "requestId": "revoke-seat-zero",
                            "payload": {"session_id": session_id, "seat_index": 0},
                        }
                    )
                    game_sockets[0].expect_json(
                        lambda message: message.get("requestId") == "revoke-seat-zero"
                        and message.get("type") == "ack",
                        2.0,
                        "hand revoke ack",
                    )
                    revoked_notice, _ = spectator_ws.expect_json(
                        lambda message: message.get("type") == "spectator.hand.revoked"
                        and message.get("payload", {}).get("seat_index") == 0,
                        2.0,
                        "spectator hand revoked notice",
                    )
                    assert_true(
                        revoked_notice.get("payload", {}).get("seat_index") == 0,
                        f"unexpected revoke notice: {revoked_notice}",
                    )
                    spectator_ws.send_json(
                        {
                            "type": "spectator.hand.refresh",
                            "requestId": "refresh-after-revoke",
                            "payload": {},
                        }
                    )
                    revoked_refresh, _ = spectator_ws.expect_json(
                        lambda message: message.get("requestId") == "refresh-after-revoke",
                        2.0,
                        "post-revoke hand refresh",
                    )
                    assert_true(
                        revoked_refresh.get("type") == "error"
                        and revoked_refresh.get("payload", {}).get("code") == "hand_access_not_granted",
                        f"revoked spectator received hand access: {revoked_refresh}",
                    )

                    denied_spectator_ws = WebSocketClient(
                        f"ws://{args.host}:{args.port}/ws/spectate?session_id={session_id}",
                        tokens[4],
                    )
                    denied_spectator_ws.expect_json(
                        lambda message: message.get("type") == "session.snapshot",
                        2.0,
                        "second spectator snapshot",
                    )
                    denied_spectator_ws.send_json(
                        {
                            "type": "spectator.hand.request",
                            "requestId": "request-seat-one",
                            "payload": {"seat_index": 1},
                        }
                    )
                    denied_spectator_ws.expect_json(
                        lambda message: message.get("type") == "spectator.hand.pending",
                        2.0,
                        "second spectator hand request pending",
                    )
                    denied_player_request, _ = game_sockets[1].expect_json(
                        lambda message: message.get("type") == "spectator.hand.request",
                        2.0,
                        "second player hand consent request",
                    )
                    denied_request_id = denied_player_request.get("payload", {}).get("request_id")
                    assert_true(
                        isinstance(denied_request_id, str),
                        f"missing denied hand request id: {denied_player_request}",
                    )
                    game_sockets[1].send_json(
                        {
                            "type": "spectator.hand.respond",
                            "requestId": "deny-spectator",
                            "payload": {"request_id": denied_request_id, "approved": False},
                        }
                    )
                    denial, _ = denied_spectator_ws.expect_json(
                        lambda message: message.get("type") == "spectator.hand.result",
                        2.0,
                        "spectator hand denial",
                    )
                    assert_true(
                        denial.get("payload", {}).get("approved") is False,
                        f"unexpected denial response: {denial}",
                    )
                    denied_spectator_ws.send_json(
                        {
                            "type": "spectator.hand.refresh",
                            "requestId": "refresh-denied-hand",
                            "payload": {},
                        }
                    )
                    denied_refresh, _ = denied_spectator_ws.expect_json(
                        lambda message: message.get("requestId") == "refresh-denied-hand",
                        2.0,
                        "denied spectator hand refresh",
                    )
                    assert_true(
                        denied_refresh.get("type") == "error"
                        and denied_refresh.get("payload", {}).get("code") == "hand_access_not_granted",
                        f"denied spectator received hand access: {denied_refresh}",
                    )

                finally:
                    if denied_spectator_ws is not None:
                        denied_spectator_ws.close()
                    if spectator_ws is not None:
                        spectator_ws.close()
                    lobby_ws.close()
                    browser_lobby_ws.close()
                    for game_ws in game_sockets:
                        game_ws.close()

            finally:
                process.send_signal(signal.SIGTERM)
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)

                if process.returncode not in (0, -signal.SIGTERM):
                    log_text = server_log_path.read_text(encoding="utf-8")
                    raise AssertionError(
                        f"server exited unexpectedly with code {process.returncode}\n{log_text}"
                    )


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:  # noqa: BLE001
        print(f"ws smoke test failed: {exc}", file=sys.stderr)
        raise
