"""Tests for xbdp against a local mock of the Device Portal API.

Run with:  python -m unittest discover -s tools/xbdp -v
"""

from __future__ import annotations

import base64
import json
import os
import sys
import tempfile
import threading
import unittest
import urllib.parse
from unittest import mock
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import xbdp  # noqa: E402

USER, PASSWORD, TOKEN = "dev", "secret", "csrf-123"

REMOTE_TREE = {
    "LooseApps/Game": [
        {"Name": "Game.exe", "Type": 32, "FileSize": 4},
        {"Name": "Content", "Type": 16},
    ],
    "LooseApps/Game/Content": [{"Name": "level.pak", "Type": 32, "FileSize": 3}],
}
REMOTE_FILES = {("LooseApps/Game", "Game.exe"): b"MZ\x90\x00", ("LooseApps/Game/Content", "level.pak"): b"pak"}


class MockPortal(BaseHTTPRequestHandler):
    log: list[tuple[str, str, dict]] = []
    install_polls = 0
    uploads: list[bytes] = []

    def log_message(self, *args):  # keep test output quiet
        pass

    def _authorized(self) -> bool:
        expected = "Basic " + base64.b64encode(f"{USER}:{PASSWORD}".encode()).decode()
        if self.headers.get("Authorization") != expected:
            self.send_response(401)
            self.end_headers()
            return False
        return True

    def _send(self, status: int, body: bytes = b"", ctype: str = "application/json") -> None:
        self.send_response(status)
        self.send_header("Set-Cookie", f"CSRF-Token={TOKEN}; Path=/")
        if body:
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if body:
            self.wfile.write(body)

    def _json(self, value) -> None:
        self._send(200, json.dumps(value).encode())

    def _route(self, method: str) -> None:
        if not self._authorized():
            return
        url = urllib.parse.urlparse(self.path)
        q = {k: v[0] for k, v in urllib.parse.parse_qs(url.query).items()}
        MockPortal.log.append((method, url.path, q))
        if method != "GET" and self.headers.get("X-CSRF-Token") != TOKEN:
            self._send(403)
            return
        length = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(length) if length else b""

        if url.path == "/api/os/info" and method == "GET":
            self._json({"ComputerName": "XBOX-DEVKIT", "OsVersion": "10.0.25398"})
        elif url.path == "/api/app/packagemanager/packages":
            self._json({"InstalledPackages": [{"Name": "Game", "PackageFullName": "Contoso.Game_1.0.0.0_x64__abc",
                                               "PackageRelativeId": "Contoso.Game_abc!Game"}]})
        elif url.path == "/api/taskmanager/app" and method in ("POST", "DELETE"):
            self._send(200)
        elif url.path == "/api/app/packagemanager/package" and method == "POST":
            MockPortal.uploads.append(body)
            self._send(202)
        elif url.path == "/api/app/packagemanager/state":
            MockPortal.install_polls += 1
            if MockPortal.install_polls < 3:
                self._send(204)
            else:
                self._json({"Success": True, "Code": 0})
        elif url.path == "/api/filesystem/apps/files":
            self._json({"Items": REMOTE_TREE.get(q.get("path", ""), [])})
        elif url.path == "/api/filesystem/apps/file" and method == "GET":
            data = REMOTE_FILES.get((q.get("path", ""), q.get("filename")))
            if data is None:
                self._send(404)
            else:
                self._send(200, data, "application/octet-stream")
        else:
            self._send(404)

    def do_GET(self):
        self._route("GET")

    def do_POST(self):
        self._route("POST")

    def do_DELETE(self):
        self._route("DELETE")


class XbdpTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = ThreadingHTTPServer(("127.0.0.1", 0), MockPortal)
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()

    def setUp(self):
        MockPortal.log.clear()
        MockPortal.install_polls = 0
        MockPortal.uploads.clear()

    def portal(self, password: str = PASSWORD) -> xbdp.DevicePortal:
        conn = xbdp.Connection(host="127.0.0.1", port=self.server.server_address[1], user=USER,
                               password=password, scheme="http", timeout=5)
        return xbdp.DevicePortal(conn)

    def test_info_with_basic_auth(self):
        self.assertEqual(self.portal().os_info()["ComputerName"], "XBOX-DEVKIT")

    def test_wrong_password_reports_401(self):
        with self.assertRaises(xbdp.PortalError) as ctx:
            self.portal(password="nope").os_info()
        self.assertEqual(ctx.exception.status, 401)
        self.assertIn("credentials", str(ctx.exception))

    def test_launch_primes_csrf_and_encodes_params(self):
        p = self.portal()
        p.launch("Contoso.Game_abc!Game", "Contoso.Game_1.0.0.0_x64__abc")
        methods = [(m, path) for m, path, _ in MockPortal.log]
        self.assertEqual(methods[0], ("GET", "/api/os/info"))  # fetched the CSRF cookie first
        self.assertEqual(methods[1], ("POST", "/api/taskmanager/app"))
        params = MockPortal.log[1][2]
        self.assertEqual(base64.b64decode(params["appid"]).decode(), "Contoso.Game_abc!Game")
        self.assertEqual(base64.b64decode(params["package"]).decode(), "Contoso.Game_1.0.0.0_x64__abc")

    def test_packages(self):
        pkgs = self.portal().packages()
        self.assertEqual(pkgs[0]["PackageFullName"], "Contoso.Game_1.0.0.0_x64__abc")

    def test_install_uploads_and_polls(self):
        with tempfile.TemporaryDirectory() as d:
            pkg = Path(d) / "game.msixvc"
            pkg.write_bytes(b"PACKAGE-BYTES")
            result = self.portal().install(pkg, poll_interval=0.01)
        self.assertTrue(result["Success"])
        self.assertEqual(MockPortal.install_polls, 3)
        self.assertIn(b"PACKAGE-BYTES", MockPortal.uploads[0])
        self.assertIn(b'filename="game.msixvc"', MockPortal.uploads[0])

    def test_pull_tree_recurses(self):
        with tempfile.TemporaryDirectory() as d:
            files, total = self.portal().pull_tree("DevelopmentFiles", "LooseApps/Game", Path(d) / "game",
                                                   log=lambda *_: None)
            self.assertEqual((files, total), (2, 7))
            self.assertEqual((Path(d) / "game" / "Game.exe").read_bytes(), b"MZ\x90\x00")
            self.assertEqual((Path(d) / "game" / "Content" / "level.pak").read_bytes(), b"pak")

    def test_cli_requires_host(self):
        with mock.patch.dict(os.environ, {}, clear=True), self.assertRaises(SystemExit):
            xbdp.main(["info"])

    def test_hex64(self):
        self.assertEqual(xbdp.hex64("App"), "QXBw")


if __name__ == "__main__":
    unittest.main()
