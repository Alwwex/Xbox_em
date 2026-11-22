#!/usr/bin/env python3
"""xbdp - command-line client for the Xbox Device Portal (Windows Device Portal on Xbox).

Talks to a development console (Dev Mode retail console or a devkit) over the
Device Portal REST API on your local network. Uses only the Python standard
library, so it runs on Windows, Linux and macOS without installing anything.

Typical workflow with xbem:
    xbdp --host 192.168.1.50 info
    xbdp packages
    xbdp pull-tree DevelopmentFiles LooseApps/MyGame ./mygame   # copy a loose build to the PC
    xbem run ./mygame

Connection settings can come from the environment:
    XBDP_HOST, XBDP_PORT (default 11443), XBDP_USER, XBDP_PASSWORD, XBDP_CAFILE

TLS: the console uses a self-signed certificate. Prefer pinning it: download it
once from https://<console>:11443/config/rootcertificate (or the Device Portal
web UI) and pass --cafile. --insecure disables verification and should only be
used on a network you trust.
"""

from __future__ import annotations

import argparse
import base64
import http.cookiejar
import json
import mimetypes
import os
import ssl
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable, Optional

__version__ = "0.1.0"

DEFAULT_PORT = 11443
CSRF_COOKIE = "CSRF-Token"
CSRF_HEADER = "X-CSRF-Token"
FILE_ATTRIBUTE_DIRECTORY = 0x10


class PortalError(Exception):
    """A Device Portal request failed."""

    def __init__(self, message: str, status: Optional[int] = None, body: str = ""):
        super().__init__(message)
        self.status = status
        self.body = body


def hex64(value: str) -> str:
    """Device Portal calls base64 'hex64'. App IDs and package names must be encoded with it."""
    return base64.b64encode(value.encode("utf-8")).decode("ascii")


def encode_multipart(field: str, filename: str, data: bytes) -> tuple[bytes, str]:
    boundary = "----xbdp" + uuid.uuid4().hex
    ctype = mimetypes.guess_type(filename)[0] or "application/octet-stream"
    head = (
        f"--{boundary}\r\n"
        f'Content-Disposition: form-data; name="{field}"; filename="{filename}"\r\n'
        f"Content-Type: {ctype}\r\n\r\n"
    ).encode("utf-8")
    tail = f"\r\n--{boundary}--\r\n".encode("utf-8")
    return head + data + tail, f"multipart/form-data; boundary={boundary}"


@dataclass
class Connection:
    host: str
    port: int = DEFAULT_PORT
    user: Optional[str] = None
    password: Optional[str] = None
    cafile: Optional[str] = None
    insecure: bool = False
    scheme: str = "https"
    timeout: float = 30.0


class DevicePortal:
    """Minimal Device Portal client: auth, CSRF handling and the endpoints xbdp uses."""

    def __init__(self, conn: Connection):
        self.conn = conn
        self.cookies = http.cookiejar.CookieJar()
        handlers: list[Any] = [urllib.request.HTTPCookieProcessor(self.cookies)]
        if conn.scheme == "https":
            if conn.insecure:
                ctx = ssl.create_default_context()
                ctx.check_hostname = False
                ctx.verify_mode = ssl.CERT_NONE
            else:
                ctx = ssl.create_default_context(cafile=conn.cafile)
                if conn.cafile:
                    # Console certificates are issued for the device name, not its IP.
                    ctx.check_hostname = False
            handlers.append(urllib.request.HTTPSHandler(context=ctx))
        self.opener = urllib.request.build_opener(*handlers)

    # ------------------------------------------------------------ plumbing

    def url(self, path: str, params: Optional[dict[str, Any]] = None) -> str:
        query = ""
        if params:
            query = "?" + urllib.parse.urlencode({k: v for k, v in params.items() if v is not None})
        return f"{self.conn.scheme}://{self.conn.host}:{self.conn.port}{path}{query}"

    def csrf_token(self) -> Optional[str]:
        for c in self.cookies:
            if c.name == CSRF_COOKIE:
                return c.value
        return None

    def request(
        self,
        method: str,
        path: str,
        params: Optional[dict[str, Any]] = None,
        body: Optional[bytes] = None,
        content_type: Optional[str] = None,
        raw: bool = False,
    ) -> Any:
        # State-changing requests need the CSRF token the portal hands out as a cookie.
        if method != "GET" and self.csrf_token() is None:
            try:
                self.request("GET", "/api/os/info")
            except PortalError:
                pass
        req = urllib.request.Request(self.url(path, params), data=body, method=method)
        if self.conn.user is not None:
            cred = f"{self.conn.user}:{self.conn.password or ''}".encode("utf-8")
            req.add_header("Authorization", "Basic " + base64.b64encode(cred).decode("ascii"))
        token = self.csrf_token()
        if token and method != "GET":
            req.add_header(CSRF_HEADER, token)
        if content_type:
            req.add_header("Content-Type", content_type)
        try:
            with self.opener.open(req, timeout=self.conn.timeout) as resp:
                data = resp.read()
                status = resp.status
        except urllib.error.HTTPError as e:
            text = e.read().decode("utf-8", "replace")
            hint = ""
            if e.code == 401:
                hint = " (check --user/--password: the Device Portal credentials set on the console)"
            elif e.code == 403:
                hint = " (CSRF token rejected or the action is not allowed)"
            raise PortalError(f"{method} {path}: HTTP {e.code}{hint}", e.code, text) from None
        except urllib.error.URLError as e:
            raise PortalError(f"{method} {path}: cannot reach {self.conn.host}:{self.conn.port}: {e.reason}") from None
        if raw:
            return data
        if status == 204 or not data:
            return None
        try:
            return json.loads(data.decode("utf-8"))
        except ValueError:
            return data.decode("utf-8", "replace")

    # ------------------------------------------------------------- system

    def os_info(self) -> Any:
        return self.request("GET", "/api/os/info")

    def machine_name(self) -> Any:
        return self.request("GET", "/api/os/machinename")

    def system_perf(self) -> Any:
        return self.request("GET", "/api/resourcemanager/systemperf")

    def processes(self) -> list[dict[str, Any]]:
        return (self.request("GET", "/api/resourcemanager/processes") or {}).get("Processes", [])

    def restart(self) -> None:
        self.request("POST", "/api/control/restart")

    def shutdown(self) -> None:
        self.request("POST", "/api/control/shutdown")

    # --------------------------------------------------------------- apps

    def packages(self) -> list[dict[str, Any]]:
        return (self.request("GET", "/api/app/packagemanager/packages") or {}).get("InstalledPackages", [])

    def launch(self, app_id: str, package_full_name: str) -> None:
        self.request("POST", "/api/taskmanager/app", {"appid": hex64(app_id), "package": hex64(package_full_name)})

    def terminate(self, package_full_name: str) -> None:
        self.request("DELETE", "/api/taskmanager/app", {"package": hex64(package_full_name)})

    def install(self, package_path: Path, poll_interval: float = 1.0, timeout: float = 1800.0) -> Any:
        data = package_path.read_bytes()
        body, ctype = encode_multipart(package_path.name, package_path.name, data)
        self.request("POST", "/api/app/packagemanager/package", {"package": package_path.name}, body, ctype)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            state = self.request("GET", "/api/app/packagemanager/state")
            if state is not None:  # 204 while the installation is still running
                return state
            time.sleep(poll_interval)
        raise PortalError("installation did not finish in time")

    def uninstall(self, package_full_name: str) -> None:
        self.request("DELETE", "/api/app/packagemanager/package", {"package": package_full_name})

    # -------------------------------------------------------------- files

    def known_folders(self) -> list[str]:
        return (self.request("GET", "/api/filesystem/apps/knownfolders") or {}).get("KnownFolders", [])

    def list_files(self, folder: str, path: str = "", package: Optional[str] = None) -> list[dict[str, Any]]:
        params = {"knownfolderid": folder, "packagefullname": package, "path": path or None}
        return (self.request("GET", "/api/filesystem/apps/files", params) or {}).get("Items", [])

    def download(self, folder: str, path: str, filename: str, package: Optional[str] = None) -> bytes:
        params = {"knownfolderid": folder, "filename": filename, "packagefullname": package, "path": path or None}
        return self.request("GET", "/api/filesystem/apps/file", params, raw=True)

    def upload(self, folder: str, path: str, local: Path, package: Optional[str] = None) -> None:
        body, ctype = encode_multipart(local.name, local.name, local.read_bytes())
        params = {"knownfolderid": folder, "packagefullname": package, "path": path or None}
        self.request("POST", "/api/filesystem/apps/file", params, body, ctype)

    def pull_tree(
        self, folder: str, remote: str, local: Path, package: Optional[str] = None, log=print
    ) -> tuple[int, int]:
        """Recursively copies a remote directory. Returns (files, bytes)."""
        local.mkdir(parents=True, exist_ok=True)
        files = total = 0
        for item in self.list_files(folder, remote, package):
            name = item.get("Name", "")
            if not name or name in (".", ".."):
                continue
            sub = f"{remote}/{name}" if remote else name
            if int(item.get("Type", 0)) & FILE_ATTRIBUTE_DIRECTORY:
                f, b = self.pull_tree(folder, sub, local / name, package, log)
                files += f
                total += b
            else:
                data = self.download(folder, remote, name, package)
                (local / name).write_bytes(data)
                files += 1
                total += len(data)
                log(f"  {sub} ({len(data)} bytes)")
        return files, total

    # ---------------------------------------------------- Xbox extensions
    # Xbox-specific endpoints live under /ext and may differ between OS versions.

    def screenshot(self) -> bytes:
        return self.request("GET", "/ext/screenshot", {"download": "true"}, raw=True)


# ------------------------------------------------------------------ CLI


def print_json(value: Any) -> None:
    print(json.dumps(value, indent=2, ensure_ascii=False))


def connection_from_args(args: argparse.Namespace) -> Connection:
    host = args.host or os.environ.get("XBDP_HOST")
    if not host:
        raise SystemExit("error: no console address - use --host or set XBDP_HOST")
    return Connection(
        host=host,
        port=args.port or int(os.environ.get("XBDP_PORT", DEFAULT_PORT)),
        user=args.user if args.user is not None else os.environ.get("XBDP_USER"),
        password=args.password if args.password is not None else os.environ.get("XBDP_PASSWORD"),
        cafile=args.cafile or os.environ.get("XBDP_CAFILE"),
        insecure=args.insecure,
        scheme=args.scheme,
        timeout=args.timeout,
    )


def cmd_info(p: DevicePortal, a: argparse.Namespace) -> None:
    info = p.os_info()
    if a.json:
        print_json(info)
        return
    for k, v in (info or {}).items():
        print(f"{k:<22} {v}")


def cmd_packages(p: DevicePortal, a: argparse.Namespace) -> None:
    pkgs = p.packages()
    if a.json:
        print_json(pkgs)
        return
    for pkg in pkgs:
        print(f"{pkg.get('PackageFullName', '?')}")
        print(f"    name: {pkg.get('Name', '')}   app id: {pkg.get('PackageRelativeId', '')}")


def cmd_processes(p: DevicePortal, a: argparse.Namespace) -> None:
    procs = p.processes()
    if a.json:
        print_json(procs)
        return
    print(f"{'PID':>7}  {'Working set':>12}  Image")
    for pr in sorted(procs, key=lambda x: x.get("ImageName", "")):
        ws = pr.get("PrivateWorkingSet", pr.get("WorkingSetSize", 0)) or 0
        print(f"{pr.get('ProcessId', 0):>7}  {int(ws) // 1024:>9} KiB  {pr.get('ImageName', '')}")


def cmd_launch(p: DevicePortal, a: argparse.Namespace) -> None:
    p.launch(a.app_id, a.package)
    print(f"launched {a.app_id}")


def cmd_terminate(p: DevicePortal, a: argparse.Namespace) -> None:
    p.terminate(a.package)
    print(f"terminated {a.package}")


def cmd_install(p: DevicePortal, a: argparse.Namespace) -> None:
    result = p.install(Path(a.file))
    if a.json:
        print_json(result)
    else:
        print(f"install finished: {result}")


def cmd_uninstall(p: DevicePortal, a: argparse.Namespace) -> None:
    p.uninstall(a.package)
    print(f"uninstalled {a.package}")


def cmd_folders(p: DevicePortal, a: argparse.Namespace) -> None:
    for f in p.known_folders():
        print(f)


def cmd_ls(p: DevicePortal, a: argparse.Namespace) -> None:
    items = p.list_files(a.folder, a.path, a.package)
    if a.json:
        print_json(items)
        return
    for it in items:
        is_dir = int(it.get("Type", 0)) & FILE_ATTRIBUTE_DIRECTORY
        size = "<DIR>" if is_dir else str(it.get("FileSize", ""))
        print(f"{size:>14}  {it.get('Name', '')}")


def cmd_pull(p: DevicePortal, a: argparse.Namespace) -> None:
    remote_dir, _, name = a.remote.replace("\\", "/").rpartition("/")
    data = p.download(a.folder, remote_dir, name, a.package)
    target = Path(a.local or name)
    target.write_bytes(data)
    print(f"{a.remote} -> {target} ({len(data)} bytes)")


def cmd_pull_tree(p: DevicePortal, a: argparse.Namespace) -> None:
    files, total = p.pull_tree(a.folder, a.remote.replace("\\", "/").strip("/"), Path(a.local), a.package)
    print(f"copied {files} files, {total} bytes into {a.local}")


def cmd_push(p: DevicePortal, a: argparse.Namespace) -> None:
    p.upload(a.folder, a.remote, Path(a.local), a.package)
    print(f"{a.local} -> {a.folder}:{a.remote}")


def cmd_screenshot(p: DevicePortal, a: argparse.Namespace) -> None:
    data = p.screenshot()
    Path(a.out).write_bytes(data)
    print(f"screenshot saved to {a.out} ({len(data)} bytes)")


def cmd_perf(p: DevicePortal, a: argparse.Namespace) -> None:
    print_json(p.system_perf())


def cmd_reboot(p: DevicePortal, a: argparse.Namespace) -> None:
    p.restart()
    print("restart requested")


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(prog="xbdp", description="Xbox Device Portal client")
    ap.add_argument("--host", help="console IP or host name (env XBDP_HOST)")
    ap.add_argument("--port", type=int, help=f"Device Portal port (default {DEFAULT_PORT})")
    ap.add_argument("--user", help="Device Portal user name (env XBDP_USER)")
    ap.add_argument("--password", help="Device Portal password (env XBDP_PASSWORD)")
    ap.add_argument("--cafile", help="pin the console certificate (PEM/CRT file)")
    ap.add_argument("--insecure", action="store_true", help="do not verify TLS (trusted LAN only)")
    ap.add_argument("--scheme", default="https", choices=["https", "http"], help=argparse.SUPPRESS)
    ap.add_argument("--timeout", type=float, default=30.0)
    ap.add_argument("--json", action="store_true", help="print raw JSON")
    ap.add_argument("--version", action="version", version=f"xbdp {__version__}")
    sub = ap.add_subparsers(dest="command", required=True)

    def add(name: str, fn, help_text: str) -> argparse.ArgumentParser:
        sp = sub.add_parser(name, help=help_text)
        sp.set_defaults(fn=fn)
        return sp

    add("info", cmd_info, "OS / console information")
    add("packages", cmd_packages, "installed packages (full names and app ids)")
    add("processes", cmd_processes, "running processes")
    add("perf", cmd_perf, "system performance counters")
    sp = add("launch", cmd_launch, "start an app")
    sp.add_argument("app_id", help="PackageRelativeId, e.g. App or Game")
    sp.add_argument("package", help="PackageFullName")
    sp = add("terminate", cmd_terminate, "stop a running app")
    sp.add_argument("package", help="PackageFullName")
    sp = add("install", cmd_install, "upload and install a package (.appx/.msix/.msixvc)")
    sp.add_argument("file")
    sp = add("uninstall", cmd_uninstall, "uninstall a package")
    sp.add_argument("package", help="PackageFullName")
    add("folders", cmd_folders, "known folders available for file transfer")
    for name, fn, help_text in (("ls", cmd_ls, "list a directory"),):
        sp = add(name, fn, help_text)
        sp.add_argument("folder", help="known folder id, e.g. DevelopmentFiles or LocalAppData")
        sp.add_argument("path", nargs="?", default="")
        sp.add_argument("--package", help="PackageFullName (for per-app folders)")
    sp = add("pull", cmd_pull, "download one file")
    sp.add_argument("folder")
    sp.add_argument("remote", help="path/inside/folder/file.bin")
    sp.add_argument("local", nargs="?")
    sp.add_argument("--package")
    sp = add("pull-tree", cmd_pull_tree, "download a directory recursively (e.g. a loose game build)")
    sp.add_argument("folder")
    sp.add_argument("remote")
    sp.add_argument("local")
    sp.add_argument("--package")
    sp = add("push", cmd_push, "upload one file into a directory")
    sp.add_argument("folder")
    sp.add_argument("remote", help="target directory inside the folder")
    sp.add_argument("local")
    sp.add_argument("--package")
    sp = add("screenshot", cmd_screenshot, "capture the screen (Xbox extension endpoint)")
    sp.add_argument("out", nargs="?", default="screenshot.png")
    add("reboot", cmd_reboot, "restart the console")
    return ap


def main(argv: Optional[Iterable[str]] = None) -> int:
    args = build_parser().parse_args(list(argv) if argv is not None else None)
    portal = DevicePortal(connection_from_args(args))
    try:
        args.fn(portal, args)
    except PortalError as e:
        print(f"error: {e}", file=sys.stderr)
        if e.body:
            print(e.body[:2000], file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
