from __future__ import annotations

import importlib.util
import json
import re
import runpy
from pathlib import Path
import stat
import sys
import tempfile
import unittest
from unittest import mock
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
                    '$ConnectArgument = "--connect=127.0.0.1:" + $Account + "::" + $Character',
                    '            $ConnectArgument,',
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
                    "diagnostics unavailable",
                    '"launcher-failure.log"',
                    "Add-Type -TypeDefinition",
                    "AtrinikReviewSecretNative",
                    "CreateFileW",
                    "GetFinalPathNameByHandleW",
                    "SetFileInformationByHandle",
                    "HandlePathMatches",
                    "DeleteFileByHandle",
                    "DeleteOwnerOnlyFile",
                    "0xC0010000U",
                    "0x80200080U",
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
                    "Server saves complete; releasing resources",
                    "ServerDiagnostics",
                    "identity_recent",
                    "ready_marker",
                    "stdout_ready_marker",
                    "ServerExitDiagnostics",
                    "stdout_tail=",
                    "stderr_tail=",
                    "function ConvertTo-ReviewCommandLineArgument",
                    "function Set-ReviewArgumentList",
                    "function Get-ReviewQuicFingerprint",
                    "FromBase64String",
                    "[System.Security.Cryptography.SHA256]::Create()",
                    "function Write-ReviewProgress",
                    '$LauncherProgressLog = Join-Path $Root "launcher-progress.log"',
                    'Write-ReviewProgress "fingerprint-start"',
                    'Write-ReviewProgress "server-output-registered"',
                    'Write-ReviewProgress "server-probe-$ProbeCount-start"',
                    'server-probe-$ProbeCount-result:',
                    'Write-ReviewProgress "client-start"',
                    'Write-ReviewProgress "client-started"',
                    'Write-ReviewProgress "client-exit-code=$($Client.ExitCode)"',
                    'Write-ReviewProgress "server-exit-code=$($Server.ExitCode)"',
                    'GetProperty("ArgumentList")',
                    "ArgumentList.Add",
                    "$StartInfo.Arguments =",
                    "$ClientStartInfo = [System.Diagnostics.ProcessStartInfo]::new()",
                    "$ClientStartInfo.UseShellExecute = $false",
                    "$ClientStartInfo.RedirectStandardOutput = $true",
                    "$ClientStartInfo.RedirectStandardError = $true",
                    '"--logfile=$ClientLog"',
                    "$Client = [System.Diagnostics.Process]::new()",
                    "$Client.StartInfo = $ClientStartInfo",
                    "$Client.BeginOutputReadLine()",
                    "$Client.BeginErrorReadLine()",
                    "client_stdout_tail=",
                    "client_stderr_tail=",
                    "function Register-ReviewProcessOutput",
                    "function Receive-ReviewProcessOutput",
                    "function Unregister-ReviewProcessOutput",
                    "Register-ObjectEvent",
                    "Get-Event -SourceIdentifier",
                    "Remove-Event -EventIdentifier",
                    "Unregister-Event -SourceIdentifier",
                ):
                    self.assertIn(token, powershell)
                self.assertNotIn("ConvertTo-ReviewArguments", powershell)
                self.assertNotIn("review-quic-fingerprint.py", powershell)
                self.assertNotIn("DeleteFileW", powershell)
                self.assertNotIn("add_OutputDataReceived", powershell)
                self.assertNotIn("add_ErrorDataReceived", powershell)
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
                secret_cleanup = powershell[
                    powershell.index("function Remove-ReviewSecretFile") :
                    powershell.index("function Remove-ReviewSecretFiles")
                ]
                self.assertIn(
                    '    $Result = [AtrinikReviewSecretNative]::DeleteOwnerOnlyFile($Path)',
                    secret_cleanup,
                )
                self.assertNotIn("Remove-Item -LiteralPath $Path", secret_cleanup)
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

    def test_shutdown_consumers_require_checked_saves_and_successful_exit(self) -> None:
        root = MODULE_PATH.parents[2]
        server_main = (root / "server/src/server/main.c").read_text(encoding="utf-8")
        shutdown = server_main.split("void server_shutdown(void) {", 1)[1].split("\n}", 1)[0]
        success = re.search(r'if \(ok\) \{\s*LOG\(INFO, "([^"]+)"\);', shutdown)
        failure = re.search(r'LOG\(ERROR, "([^"]+)"\);', shutdown)
        self.assertIsNotNone(success)
        self.assertIsNotNone(failure)
        launcher = bundle.POWERSHELL_LAUNCHER
        smoke = (root / "tools/ci/smoke_windows_review_bundle.ps1").read_text(encoding="utf-8")
        for script in (launcher, smoke):
            markers = re.findall(r'-(?:notmatch|match) "(Server (?:shutdown|saves|resources)[^"]+)"', script)
            self.assertEqual(len(markers), 2)
            for marker in markers:
                self.assertRegex(success.group(1), marker)
                self.assertNotRegex(failure.group(1), marker)
                self.assertNotRegex("Server resources released; deinitializing toolkit.", marker)

        # These guards must reject a timeout/nonzero exit before the success
        # marker can authorize completion; a log message alone is insufficient.
        launcher_finish = launcher.split('$Server.StandardInput.WriteLine("shutdown")', 1)[1]
        launcher_finish = launcher_finish.split('$LauncherSucceeded = $true', 1)[0]
        self.assertRegex(launcher_finish, r'if \(-not \$Server\.WaitForExit\(30000\)\) \{\s*throw ')
        self.assertRegex(launcher_finish, r'if \(\$Server\.ExitCode -ne 0\) \{\s*throw ')
        self.assertLess(launcher_finish.index('$Server.WaitForExit(30000)'),
                        launcher_finish.index('$Server.ExitCode -ne 0'))
        self.assertLess(launcher_finish.index('$Server.ExitCode -ne 0'),
                        launcher_finish.index('-notmatch'))
        flat_finish = smoke.split('    if ($shutdownTimedOut) {')[-1].split('$launcherStartInfo =', 1)[0]
        self.assertTrue(flat_finish.lstrip().startswith('throw ('))
        self.assertRegex(flat_finish, r'if \(\$process\.ExitCode -ne 0\) \{\s*throw ')
        self.assertLess(flat_finish.index('$process.ExitCode -ne 0'), flat_finish.index('-notmatch'))
        launcher_wait = 'if (-not $launcherServer.HasExited -and -not $launcherServer.WaitForExit(45000))'
        launcher_smoke = smoke[smoke.index(launcher_wait):]
        launcher_smoke = launcher_smoke.split('$bodySucceeded = $true', 1)[0]
        self.assertIn('$launcherServer.WaitForExit(45000)', launcher_smoke)
        self.assertIn('$launcherServerExitCode -ne 0', launcher_smoke)
        self.assertLess(launcher_smoke.index('$launcherServerExitCode -ne 0'),
                        launcher_smoke.index('-notmatch'))
        self.assertRegex(launcher_smoke, r'if \(-not \$launcherProcess\.WaitForExit\(60000\)\) \{\s*throw ')
        self.assertRegex(launcher_smoke, r'if \(\$launcherProcess\.ExitCode -ne 0\) \{\s*throw ')

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


    def test_safe_entries_skips_directories_and_rejects_symlinks(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive_path = root / "archive.zip"
            self.make_zip(
                archive_path,
                {"client/empty/": b"", "client/payload": b"payload"},
            )
            with zipfile.ZipFile(archive_path) as archive:
                archive_root, entries = bundle._safe_entries(archive)
            self.assertEqual(archive_root, "client")
            self.assertEqual([entry.filename for entry in entries], ["client/payload"])

            symlink_path = root / "symlink.zip"
            symlink = zipfile.ZipInfo("client/link")
            symlink.external_attr = (stat.S_IFLNK | 0o777) << 16
            with zipfile.ZipFile(symlink_path, "w") as archive:
                archive.writestr(symlink, b"target")
            with zipfile.ZipFile(symlink_path) as archive:
                with self.assertRaisesRegex(
                    bundle.BundleError, "symbolic links are not allowed"
                ):
                    bundle._safe_entries(archive)

    def test_safe_entries_rejects_case_duplicates_and_multiple_roots(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            duplicate = root / "duplicate.zip"
            self.make_zip(
                duplicate,
                {"client/Name": b"one", "client/name": b"two"},
            )
            with zipfile.ZipFile(duplicate) as archive:
                with self.assertRaisesRegex(
                    bundle.BundleError, "duplicate or case-colliding"
                ):
                    bundle._safe_entries(archive)

            multiple_roots = root / "multiple-roots.zip"
            self.make_zip(
                multiple_roots,
                {"client/file": b"one", "other/file": b"two"},
            )
            with zipfile.ZipFile(multiple_roots) as archive:
                with self.assertRaisesRegex(
                    bundle.BundleError, "exactly one top-level directory"
                ):
                    bundle._safe_entries(archive)

    def test_member_target_rejects_non_server_and_empty_members(self) -> None:
        self.assertIsNone(
            bundle._member_target(
                zipfile.ZipInfo("package/not-server.txt"),
                "package",
                server=True,
            )
        )
        self.assertIsNone(
            bundle._member_target(
                zipfile.ZipInfo("client/"),
                "client",
                server=False,
            )
        )

    def test_add_package_rejects_case_colliding_bundle_paths(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            client = root / "client.zip"
            server = root / "server.zip"
            self.make_zip(client, {"client/payload": b"client"})
            self.make_zip(server, {"package/server/PAYLOAD": b"server"})
            payloads: dict[str, bytes] = {}
            origins: dict[str, str] = {}
            bundle._add_package(payloads, origins, client, server=False)
            with self.assertRaisesRegex(bundle.BundleError, "case-colliding bundle paths"):
                bundle._add_package(payloads, origins, server, server=True)

    def test_rejects_invalid_revision_and_existing_output(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            client, server = self.packages(root)
            with self.assertRaisesRegex(bundle.BundleError, "exact 40-character"):
                bundle.compose(client, server, root / "invalid.zip", "invalid")
            existing = root / "existing.zip"
            existing.write_bytes(b"existing")
            with self.assertRaisesRegex(bundle.BundleError, "overwrite existing output"):
                bundle.compose(client, server, existing, self.revision)

    def test_rejects_missing_install_data(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            client, server = self.packages(root)
            self.make_zip(
                server,
                {
                    "package/server/atrinik-server.exe": b"server "
                    + self.revision.encode(),
                    "package/server/python.exe": b"python",
                },
            )
            with self.assertRaisesRegex(bundle.BundleError, "missing install_data"):
                bundle.compose(client, server, root / "out.zip", self.revision)

    def test_rejects_missing_required_executable(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            client, server = self.packages(root)
            self.make_zip(client, {"client/README.md": b"client readme"})
            with self.assertRaisesRegex(bundle.BundleError, "bundle is missing atrinik.exe"):
                bundle.compose(client, server, root / "out.zip", self.revision)

    def test_rejects_missing_embedded_revision(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            client, server = self.packages(root)
            self.make_zip(client, {"client/atrinik.exe": b"client"})
            with self.assertRaisesRegex(bundle.BundleError, "does not embed exact revision"):
                bundle.compose(client, server, root / "out.zip", self.revision)

    def test_rejects_crc_failure(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            client, server = self.packages(root)
            with mock.patch.object(bundle.zipfile.ZipFile, "testzip", return_value="bad"):
                with self.assertRaisesRegex(bundle.BundleError, "CRC validation"):
                    bundle.compose(client, server, root / "out.zip", self.revision)

    def test_main_parses_valid_arguments(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            client, server = self.packages(root)
            output = root / "out.zip"
            argv = [
                "compose_windows_review_bundle.py",
                "--client-package",
                str(client),
                "--server-package",
                str(server),
                "--output",
                str(output),
                "--revision",
                self.revision,
            ]
            with mock.patch.object(sys, "argv", argv):
                self.assertEqual(bundle.main(), 0)

    def test_main_reports_argument_errors(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            client, server = self.packages(root)
            argv = [
                "compose_windows_review_bundle.py",
                "--client-package",
                str(client),
                "--server-package",
                str(server),
                "--output",
                str(root / "out.zip"),
                "--revision",
                "invalid",
            ]
            with mock.patch.object(sys, "argv", argv):
                with self.assertRaises(SystemExit):
                    bundle.main()

    def test_module_entrypoint_exits_successfully(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            client, server = self.packages(root)
            argv = [
                str(MODULE_PATH),
                "--client-package",
                str(client),
                "--server-package",
                str(server),
                "--output",
                str(root / "out.zip"),
                "--revision",
                self.revision,
            ]
            with mock.patch.object(sys, "argv", argv):
                with self.assertRaises(SystemExit) as raised:
                    runpy.run_path(str(MODULE_PATH), run_name="__main__")
            self.assertEqual(raised.exception.code, 0)


if __name__ == "__main__":
    unittest.main()
