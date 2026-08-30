#!/usr/bin/env python3

"""Compose a deterministic flat Windows client/server review bundle."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import stat
import tempfile
import zipfile


REVISION = re.compile(r"^[0-9a-fA-F]{40}$")
FIXED_TIME = (1980, 1, 1, 0, 0, 0)
CLIENT_METADATA = {
    "README.md": "CLIENT-README.md",
    "ATTRIBUTIONS.md": "CLIENT-ATTRIBUTIONS.md",
    "LICENSE.md": "CLIENT-LICENSE.md",
    "LICENSE.txt": "CLIENT-LICENSE.txt",
    "INSTALL": "CLIENT-INSTALL",
    "INSTALL.md": "CLIENT-INSTALL.md",
}
SERVER_METADATA = {
    "README.md": "SERVER-README.md",
    "ATTRIBUTIONS.md": "SERVER-ATTRIBUTIONS.md",
    "LICENSE.md": "SERVER-LICENSE.md",
    "LICENSE.txt": "SERVER-LICENSE.txt",
}
CLIENT_PREFERRED_CONFLICTS = {"ca-bundle.crt", "libffi-8.dll"}
TEST_MARKERS = (b"--gpu-player-view", b"injected GPU conformance fault")


class BundleError(RuntimeError):
    """The input packages cannot form an unambiguous review bundle."""


def _safe_entries(archive: zipfile.ZipFile) -> tuple[str, list[zipfile.ZipInfo]]:
    files: list[zipfile.ZipInfo] = []
    roots: set[str] = set()
    seen: set[str] = set()
    for info in archive.infolist():
        name = info.filename
        path = PurePosixPath(name)
        if (
            not name
            or "\\" in name
            or path.is_absolute()
            or any(part in ("", ".", "..") for part in path.parts)
        ):
            raise BundleError(f"unsafe ZIP member: {name!r}")
        roots.add(path.parts[0])
        if info.is_dir():
            continue
        mode = info.external_attr >> 16
        if stat.S_ISLNK(mode):
            raise BundleError(f"symbolic links are not allowed: {name}")
        folded = name.casefold()
        if folded in seen:
            raise BundleError(f"duplicate or case-colliding ZIP member: {name}")
        seen.add(folded)
        files.append(info)
    if len(roots) != 1:
        raise BundleError("package must contain exactly one top-level directory")
    return next(iter(roots)), files


def _member_target(
    info: zipfile.ZipInfo,
    root: str,
    *,
    server: bool,
) -> str | None:
    parts = PurePosixPath(info.filename).parts
    relative = parts[1:]
    if server:
        if not relative or relative[0] != "server":
            return None
        relative = relative[1:]
        if relative == ("server.bat",):
            return None
        metadata = SERVER_METADATA
    else:
        metadata = CLIENT_METADATA
    if not relative:
        return None
    target = "/".join(relative)
    if len(relative) == 1:
        target = metadata.get(target, target)
    return target


def _add_package(
    payloads: dict[str, bytes],
    origins: dict[str, str],
    package: Path,
    *,
    server: bool,
) -> None:
    label = "server" if server else "client"
    with zipfile.ZipFile(package) as archive:
        root, entries = _safe_entries(archive)
        for info in entries:
            target = _member_target(info, root, server=server)
            if target is None:
                continue
            data = archive.read(info)
            folded = target.casefold()
            prior = next((name for name in payloads if name.casefold() == folded), None)
            if prior is None:
                payloads[target] = data
                origins[target] = label
                continue
            if prior != target:
                raise BundleError(f"case-colliding bundle paths: {prior} and {target}")
            if payloads[prior] == data:
                origins[prior] = "shared"
                continue
            if server and target.casefold() in CLIENT_PREFERRED_CONFLICTS:
                continue
            raise BundleError(f"different client/server payloads collide at {target}")


FINGERPRINT_HELPER = '''"""Print the SHA-256 fingerprint of a QUIC identity certificate."""
from __future__ import annotations
import hashlib
from pathlib import Path
import ssl
import sys

BEGIN = "-----BEGIN CERTIFICATE-----"
END = "-----END CERTIFICATE-----"

if len(sys.argv) != 2:
    raise SystemExit("usage: review-quic-fingerprint.py IDENTITY_FILE")
identity = Path(sys.argv[1]).read_text(encoding="ascii")
begin = identity.find(BEGIN)
end = identity.find(END, begin)
if begin < 0 or end < 0:
    raise SystemExit("QUIC certificate is missing from the identity file")
pem = identity[begin : end + len(END)]
print(hashlib.sha256(ssl.PEM_cert_to_DER_cert(pem)).hexdigest())
'''


BAT_LAUNCHER = r'''@echo off
setlocal EnableExtensions
cd /d "%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0run-review.ps1"
set "RESULT=%ERRORLEVEL%"
if not "%RESULT%"=="0" (
  echo.
  echo Review launch failed. See the error above.
  pause
)
exit /b %RESULT%
'''


POWERSHELL_LAUNCHER = r'''$ErrorActionPreference = "Stop"
$Root = (Resolve-Path -LiteralPath $PSScriptRoot).Path
$State = Join-Path $Root "server-data"
$Sentinel = Join-Path $State ".atrinik-review-initialized"
$InstallData = Join-Path $Root "install_data"
$Identity = Join-Path $State "quic-identity.pem"
$LaunchMutex = [System.Threading.Mutex]::new(
    $false,
    "Local\AtrinikClassicReviewUdp1731"
)
$LaunchLockHeld = $false

try {
    try {
        $LaunchLockHeld = $LaunchMutex.WaitOne(0)
    } catch [System.Threading.AbandonedMutexException] {
        $LaunchLockHeld = $true
    }
    if (-not $LaunchLockHeld) {
        throw "Another Atrinik review launcher is already starting UDP port 1731"
    }

    $ExistingEndpoint = Get-NetUDPEndpoint -LocalPort 1731 -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($null -ne $ExistingEndpoint) {
        throw "UDP port 1731 is already owned by PID $($ExistingEndpoint.OwningProcess); stop it before launching this review"
    }

    foreach ($Required in @("atrinik.exe", "atrinik-server.exe", "python.exe", "install_data")) {
        if (-not (Test-Path -LiteralPath (Join-Path $Root $Required))) {
            throw "The complete package was not extracted: missing $Required"
        }
    }

    if (-not (Test-Path -LiteralPath $Sentinel)) {
        if (Test-Path -LiteralPath $State) {
            $Preserved = Join-Path $Root ("server-data-incomplete-" + [Guid]::NewGuid().ToString("N"))
            Move-Item -LiteralPath $State -Destination $Preserved
            Write-Warning "Preserved incomplete server state as $Preserved"
        }
        $Stage = Join-Path $Root ("server-data-stage-" + [Guid]::NewGuid().ToString("N"))
        try {
            Copy-Item -LiteralPath $InstallData -Destination $Stage -Recurse
            New-Item -ItemType File -Path (Join-Path $Stage ".atrinik-review-initialized") | Out-Null
            Move-Item -LiteralPath $Stage -Destination $State
        } catch {
            if (Test-Path -LiteralPath $Stage) {
                $Failed = Join-Path $Root ("server-data-incomplete-" + [Guid]::NewGuid().ToString("N"))
                Move-Item -LiteralPath $Stage -Destination $Failed
            }
            throw
        }
    }

    $ClientMaps = Join-Path $InstallData "http\client-maps"
    if (Test-Path -LiteralPath $ClientMaps) {
        $StateHttp = Join-Path $State "http"
        New-Item -ItemType Directory -Force -Path $StateHttp | Out-Null
        $StateMaps = Join-Path $StateHttp "client-maps"
        if (Test-Path -LiteralPath $StateMaps) { Remove-Item -LiteralPath $StateMaps -Recurse }
        Copy-Item -LiteralPath $ClientMaps -Destination $StateMaps -Recurse
    }
    New-Item -ItemType Directory -Force -Path (Join-Path $State "tmp") | Out-Null

    if (Test-Path -LiteralPath $Identity) {
        Move-Item -LiteralPath $Identity -Destination ($Identity + ".previous-" + [Guid]::NewGuid().ToString("N"))
    }
    $Started = Get-Date
    $ServerArgs = @(
        "--datapath=`"$State`"", "--port_quic=1731", "--server_public=false",
        "--stun_server=off", "--port_mapping=off"
    )
    $Server = Start-Process -FilePath (Join-Path $Root "atrinik-server.exe") `
        -ArgumentList $ServerArgs -WorkingDirectory $Root -PassThru

    try {
        $Deadline = (Get-Date).AddSeconds(60)
        $Ready = $false
        while ((Get-Date) -lt $Deadline) {
            if ($Server.HasExited) { throw "Server exited with code $($Server.ExitCode)" }
            $Endpoint = Get-NetUDPEndpoint -LocalPort 1731 -ErrorAction SilentlyContinue |
                Where-Object { $_.OwningProcess -eq $Server.Id } | Select-Object -First 1
            $IdentityInfo = Get-Item -LiteralPath $Identity -ErrorAction SilentlyContinue
            if ($Endpoint -and $IdentityInfo -and $IdentityInfo.Length -gt 0 -and
                $IdentityInfo.LastWriteTime -ge $Started) {
                $Ready = $true
                break
            }
            Start-Sleep -Milliseconds 250
            $Server.Refresh()
        }
        if (-not $Ready) { throw "Server did not own UDP port 1731 with a fresh QUIC identity within 60 seconds" }

        $Fingerprint = & (Join-Path $Root "python.exe") `
            (Join-Path $Root "review-quic-fingerprint.py") $Identity
        if ($LASTEXITCODE -ne 0 -or $Fingerprint -notmatch "^[0-9a-f]{64}$") {
            throw "Could not derive the server QUIC certificate fingerprint"
        }
        $ClientArgs = @(
            "--nometa", "--stun_server=off",
            "--server=`"127.0.0.1 1731 $Fingerprint`"", "--connect=127.0.0.1", "--reconnect"
        )
        Start-Process -FilePath (Join-Path $Root "atrinik.exe") `
            -ArgumentList $ClientArgs -WorkingDirectory $Root
        Write-Host "Server and client started. The server owns UDP port 1731 (PID $($Server.Id))."
    } catch {
        if (-not $Server.HasExited) { Stop-Process -Id $Server.Id -Force }
        throw
    }
} finally {
    if ($LaunchLockHeld) { $LaunchMutex.ReleaseMutex() }
    $LaunchMutex.Dispose()
}
'''


def compose(client: Path, server: Path, output: Path, revision: str) -> None:
    revision = revision.lower()
    if not REVISION.fullmatch(revision):
        raise BundleError("revision must be an exact 40-character hexadecimal commit")
    if output.exists():
        raise BundleError(f"refusing to overwrite existing output: {output}")

    payloads: dict[str, bytes] = {}
    origins: dict[str, str] = {}
    _add_package(payloads, origins, client, server=False)
    _add_package(payloads, origins, server, server=True)
    for required in ("atrinik.exe", "atrinik-server.exe", "python.exe", "install_data"):
        if required == "install_data":
            if not any(name.startswith("install_data/") for name in payloads):
                raise BundleError("server package is missing install_data")
        elif required not in payloads:
            raise BundleError(f"bundle is missing {required}")

    encoded_revision = revision.encode("ascii")
    for executable in ("atrinik.exe", "atrinik-server.exe"):
        if encoded_revision not in payloads[executable]:
            raise BundleError(f"{executable} does not embed exact revision {revision}")
    for marker in TEST_MARKERS:
        if marker in payloads["atrinik.exe"]:
            raise BundleError(f"production atrinik.exe contains test marker {marker.decode()}")

    payloads.update(
        {
            "run-review.bat": BAT_LAUNCHER.replace("\n", "\r\n").encode(),
            "run-review.ps1": POWERSHELL_LAUNCHER.replace("\n", "\r\n").encode(),
            "review-quic-fingerprint.py": FINGERPRINT_HELPER.encode(),
            "REVIEW-README.txt": (
                "Atrinik Classic issue #477 Windows review package\r\n"
                f"Exact revision: {revision}\r\n\r\n"
                "Extract the complete ZIP to a writable directory, then double-click "
                "run-review.bat. It creates isolated state, starts the server on UDP 1731, "
                "waits for the exact server process to own the port, and starts a reconnecting "
                "client pinned to the fresh QUIC identity.\r\n"
            ).encode(),
        }
    )
    origins.update({name: "generated" for name in payloads if name.startswith("run-review")})
    origins["review-quic-fingerprint.py"] = "generated"
    origins["REVIEW-README.txt"] = "generated"
    manifest = {
        "format": 1,
        "revision": revision,
        "udp_port": 1731,
        "files": [
            {
                "path": name,
                "sha256": hashlib.sha256(data).hexdigest(),
                "size": len(data),
                "origin": origins.get(name, "generated"),
            }
            for name, data in sorted(payloads.items(), key=lambda item: item[0].casefold())
        ],
    }
    payloads["BUNDLE-MANIFEST.json"] = (
        json.dumps(manifest, indent=2, sort_keys=True) + "\n"
    ).encode()

    root = f"atrinik-classic-issue-477-windows-one-click-{revision[:7]}"
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=output.parent, suffix=".zip", delete=False) as handle:
        temporary = Path(handle.name)
    try:
        with zipfile.ZipFile(temporary, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
            for name, data in sorted(payloads.items(), key=lambda item: item[0].casefold()):
                info = zipfile.ZipInfo(f"{root}/{name}", FIXED_TIME)
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = (0o100644 & 0xFFFF) << 16
                archive.writestr(info, data)
        with zipfile.ZipFile(temporary) as archive:
            if archive.testzip() is not None:
                raise BundleError("composed ZIP failed CRC validation")
            _safe_entries(archive)
        temporary.replace(output)
    finally:
        temporary.unlink(missing_ok=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--client-package", type=Path, required=True)
    parser.add_argument("--server-package", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--revision", required=True)
    args = parser.parse_args()
    try:
        compose(args.client_package, args.server_package, args.output, args.revision)
    except (BundleError, OSError, zipfile.BadZipFile) as error:
        parser.error(str(error))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
