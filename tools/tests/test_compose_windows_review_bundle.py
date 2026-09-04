from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import zipfile


MODULE_PATH = Path(__file__).resolve().parents[1] / "release" / "compose_windows_review_bundle.py"
SPEC = importlib.util.spec_from_file_location("compose_windows_review_bundle", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
bundle = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(bundle)


class ComposeWindowsReviewBundleTests(unittest.TestCase):
    revision = "a" * 40

    def make_zip(self, path: Path, members: dict[str, bytes]) -> None:
        with zipfile.ZipFile(path, "w") as archive:
            for name, data in members.items():
                archive.writestr(name, data)

    def packages(self, root: Path) -> tuple[Path, Path]:
        client = root / "client.zip"
        server = root / "server.zip"
        revision = self.revision.encode()
        self.make_zip(
            client,
            {
                "client/atrinik.exe": b"client " + revision,
                "client/SDL3.dll": b"shared",
                "client/libffi-8.dll": b"client-superset",
                "client/ca-bundle.crt": b"client-certs",
                "client/README.md": b"client readme",
            },
        )
        self.make_zip(
            server,
            {
                "package/server/atrinik-server.exe": b"server " + revision,
                "package/server/python.exe": b"python",
                "package/server/SDL3.dll": b"shared",
                "package/server/libffi-8.dll": b"server",
                "package/server/ca-bundle.crt": b"server-certs",
                "package/server/install_data/maps/emergency": b"map",
                "package/server/README.md": b"server readme",
                "package/server/server.bat": b"legacy launcher",
            },
        )
        return client, server

    def test_composes_flat_deterministic_review_bundle(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            client, server = self.packages(root)
            first = root / "first.zip"
            second = root / "second.zip"
            bundle.compose(client, server, first, self.revision)
            bundle.compose(client, server, second, self.revision)
            self.assertEqual(first.read_bytes(), second.read_bytes())

            with zipfile.ZipFile(first) as archive:
                names = archive.namelist()
                prefix = f"atrinik-classic-issue-521-windows-one-click-{self.revision[:7]}/"
                self.assertTrue(all(name.startswith(prefix) for name in names))
                relative = [name.removeprefix(prefix) for name in names]
                self.assertNotIn("client", {Path(name).parts[0] for name in relative})
                self.assertNotIn("server", {Path(name).parts[0] for name in relative})
                self.assertEqual(
                    [name for name in relative if name.endswith(".bat")],
                    ["run-review.bat"],
                )
                self.assertEqual(archive.read(prefix + "libffi-8.dll"), b"client-superset")
                bat = archive.read(prefix + "run-review.bat").decode()
                powershell = archive.read(prefix + "run-review.ps1").decode()
                self.assertIn("ATRINIK_REVIEW_NO_PAUSE", bat)
                for token in (
                    "server-data-stage-",
                    "server-data-incomplete-",
                    '"Local\\AtrinikClassicReviewUdp1731"',
                    "$LaunchMutex.WaitOne(0)",
                    "UDP port 1731 is already owned by PID",
                    "Get-NetUDPEndpoint -LocalPort 1731",
                    "OwningProcess -eq $Server.Id",
                    '"--no_console"',
                    '"--provision_scenario"',
                    '"--provision_account=$Account"',
                    '"--provision_character=$Character"',
                    '"--provision_archetype=human_male"',
                    '"--provision_preset=basic-player"',
                    '"--provision_password_file=$StagePassword"',
                    '"--http_url=off"',
                    '"--network_stack=ipv4=127.0.0.1"',
                    '"--nometa"',
                    '"--game_news_url=off"',
                    "--connect=127.0.0.1:",
                    '"--connect=127.0.0.1:" + $Account + "::" + $Character',
                    '"--connect_password_file=$PasswordFile"',
                    'Substring(0, 20)',
                    "Stop-ReviewProcessTree",
                    "taskkill.exe",
                    "function Invoke-ReviewIcacls",
                    "$PreviousErrorActionPreference",
                    '$ErrorActionPreference = "Continue"',
                    "$ExitCode = $LASTEXITCODE",
                    "function Get-ReviewDiagnosticTail",
                    "function Assert-ReviewPathAncestors",
                    "function Write-ReviewFailure",
                    '"launcher-failure.log"',
                    "Add-Type -TypeDefinition",
                    "AtrinikReviewSecretNative",
                    "CreateFileW",
                    "WriteFile",
                    "FlushFileBuffers",
                    "function New-ReviewOwnerSecretFile",
                    "RawSecurityDescriptor",
                    "CreateOwnerOnlyFile",
                    "function Protect-ReviewSecretFile",
                    "function Protect-ReviewTemporaryDirectory",
                    '"$($SidArgument):(OI)(CI)(F)"',
                    "icacls.exe",
                    '"/inheritance:r"',
                    '"/grant:r"',
                    '"/remove"',
                    '"/setowner"',
                    "RawSecurityDescriptor",
                    "DiscretionaryAclProtected",
                    "AccessMask -eq $ExpectedMask",
                    '-Encoding Unicode',
                    r'Where-Object { $_ -match "^D:" }',
                    "function Remove-ReviewSecretFile",
                    "function Remove-ReviewSecretFiles",
                    "Remove-ReviewSecretFile $Candidate",
                    '"$($SidArgument):(F)"',
                    "$StageTmp",
                    "$StateTmp",
                    '"server-data-stage-*"',
                    '"server-data-incomplete-*"',
                    "Test-Path -LiteralPath $PasswordFile",
                    "Protect-ReviewSecretFile $StagePassword",
                    "Protect-ReviewSecretFile $PasswordFile",
                    "Remove-ReviewSecretFiles",
                    "$Provision.WaitForExit(60000)",
                    "$ClientData",
                    "StandardInput.WriteLine(\"shutdown\")",
                    "Client shutdown complete",
                    "Server shutdown complete",
                    "ServerDiagnostics",
                    "identity_recent",
                    "ready_marker",
                    "BeginOutputReadLine",
                    "stdout_ready_marker",
                    "ServerExitDiagnostics",
                    "stdout_tail=",
                    "stderr_tail=",
                    "$ClientStartInfo = [System.Diagnostics.ProcessStartInfo]::new()",
                    "$ClientStartInfo.UseShellExecute = $false",
                    "$Client = [System.Diagnostics.Process]::new()",
                    "$Client.StartInfo = $ClientStartInfo",
                ):
                    self.assertIn(token, powershell)
                self.assertIn(
                    '# Create the secret with an explicit owner and protected DACL.',
                    powershell,
                )
                self.assertIn(
                    'New-ReviewOwnerSecretFile $StagePassword $StagePasswordValue',
                    powershell,
                )
                self.assertNotIn("$StagePasswordSeed", powershell)
                self.assertNotIn("Move-Item -LiteralPath $StagePasswordSeed", powershell)
                secret_protection = powershell[
                    powershell.index("function Protect-ReviewSecretFile") :
                    powershell.index("function Protect-ReviewTemporaryDirectory")
                ]
                self.assertNotIn('"/reset"', secret_protection)
                self.assertNotIn('"/restore"', secret_protection)
                self.assertIn('"/remove"', secret_protection)
                self.assertNotIn('"/setowner"', secret_protection)
                self.assertIn(
                    '        "/grant",\r\n'
                    '        "$($SidArgument):(F)",\r\n'
                    '        "/q"\r\n'
                    '    ))\r\n'
                    '    Remove-Item -LiteralPath $Path',
                    powershell,
                )
                self.assertNotIn("Set-Content -LiteralPath $LauncherFailureLog", powershell)
                self.assertIn("if ($LaunchLockHeld)", powershell)
                self.assertNotIn('$Account + ":" + $Password', powershell)
                self.assertNotIn("$Process.Kill($true)", powershell)
                self.assertNotIn("Get-Acl", powershell)
                self.assertNotIn("Set-Acl", powershell)
                self.assertNotIn('"--reconnect"', powershell)
                manifest = json.loads(archive.read(prefix + "BUNDLE-MANIFEST.json"))
                self.assertEqual(manifest["revision"], self.revision)
                self.assertEqual(manifest["udp_port"], 1731)

    def test_rejects_path_traversal(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            client, server = self.packages(root)
            self.make_zip(client, {"client/../atrinik.exe": self.revision.encode()})
            with self.assertRaisesRegex(bundle.BundleError, "unsafe ZIP member"):
                bundle.compose(client, server, root / "out.zip", self.revision)

    def test_rejects_different_unapproved_collision(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            client, server = self.packages(root)
            self.make_zip(
                server,
                {
                    "package/server/atrinik-server.exe": b"server "
                    + self.revision.encode(),
                    "package/server/python.exe": b"python",
                    "package/server/SDL3.dll": b"different",
                    "package/server/install_data/maps/emergency": b"map",
                },
            )
            with self.assertRaises(bundle.BundleError):
                bundle.compose(client, server, root / "out.zip", self.revision)

    def test_rejects_production_test_marker(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            client, server = self.packages(root)
            with zipfile.ZipFile(client, "w") as archive:
                archive.writestr(
                    "client/atrinik.exe",
                    self.revision.encode() + b" --gpu-player-view",
                )
            with self.assertRaisesRegex(bundle.BundleError, "test marker"):
                bundle.compose(client, server, root / "out.zip", self.revision)


if __name__ == "__main__":
    unittest.main()
