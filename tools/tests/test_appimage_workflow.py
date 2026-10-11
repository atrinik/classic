from pathlib import Path
import json
import os
import re
import subprocess
import sys
import tempfile
import textwrap
import unittest

ROOT = Path(__file__).resolve().parents[2]


class AppImageWorkflowTests(unittest.TestCase):
    RUNNER_AUTHORITY = {
        'ACTIONS_RUNTIME_TOKEN': 'fixture-runtime-token-must-stay-on-host',
        'ACTIONS_CACHE_URL': 'https://fixture-cache.invalid',
        'ACTIONS_RESULTS_URL': 'https://fixture-results.invalid',
        'GITHUB_TOKEN': 'fixture-github-token-must-stay-on-host',
        'GH_TOKEN': 'fixture-gh-token-must-stay-on-host',
    }

    def assert_isolated_container(self, args: list[str], environment: set[str]) -> None:
        # Inspect the actual expanded Docker CLI, including when the host has
        # cache/service credentials. Docker receives only explicit NAME=value
        # pairs, never bare NAME arguments that inherit host values.
        passed = [args[index + 1] for index, argument in enumerate(args)
                  if argument == '--env']
        self.assertTrue(all('=' in value for value in passed))
        self.assertEqual({value.split('=', 1)[0] for value in passed}, environment)
        self.assertEqual(args[args.index('--network') + 1], 'none')
        self.assertEqual(args[args.index('--cap-drop') + 1], 'ALL')
        self.assertEqual(args[args.index('--security-opt') + 1], 'no-new-privileges')
        self.assertEqual(args[args.index('--user') + 1], f'{os.getuid()}:{os.getgid()}')
        for forbidden in ('--env-file', '--volume', '-v', '--privileged', '--device',
                          '--pid', '--ipc', '--network=host'):
            self.assertNotIn(forbidden, args)
        for credential, value in self.RUNNER_AUTHORITY.items():
            self.assertNotIn(credential, environment)
            self.assertNotIn(value, ' '.join(args))
        mounts = [args[index + 1] for index, argument in enumerate(args)
                  if argument == '--mount']
        for mount in mounts:
            for forbidden in ('docker.sock', '/_work/_temp', '/_actions'):
                self.assertNotIn(forbidden, mount)
            source = next(part.removeprefix('source=') for part in mount.split(',')
                          if part.startswith('source='))
            self.assertNotIn(Path(source).name, ('.cache', '.config', '.ssh'))
            self.assertNotEqual(Path(source), Path.home())

    def workflow(self) -> str:
        return (ROOT / '.github/workflows/build-release-candidate.yml').read_text()

    def job(self) -> str:
        return self.workflow().split('  client-appimage:\n', 1)[1].split('  client-windows:\n', 1)[0]

    def test_schema_specific_jobs_fail_closed(self) -> None:
        workflow = self.workflow()
        job = self.job()
        self.assertIn("if: needs.metadata.outputs.artifact_schema == '3'", job)
        self.assertIn('needs: [metadata, dependencies, gpu-shaders]', job)
        finalizer = workflow.split('  candidate:\n', 1)[1]
        self.assertIn('      - client-appimage\n', finalizer)
        for schema, deb, app in ((1, 'skipped', 'skipped'), (2, 'success', 'skipped'),
                                 (3, 'skipped', 'success')):
            with self.subTest(schema=schema):
                condition = next(line for line in finalizer.splitlines()
                                 if f"artifact_schema == '{schema}'" in line)
                self.assertIn(f"needs.client-linux.result == '{deb}'", condition)
                self.assertIn(f"needs.client-appimage.result == '{app}'", condition)
        self.assertIn('${artifact_schema} == 3', workflow)

    def test_trusted_preparation_and_smoke_do_not_mount_candidate_or_secrets(self) -> None:
        job = self.job()
        checkouts = re.findall(r'uses: actions/checkout@.*?\n(.*?)(?=      -)', job, re.DOTALL)
        self.assertEqual(len(checkouts), 2)
        self.assertIn('ref: ${{ github.sha }}', checkouts[0])
        self.assertIn('tools/ci/appimage', checkouts[0])
        self.assertIn('tools/release', checkouts[0])
        self.assertIn('path: candidate-source', checkouts[1])
        self.assertIn('ref: ${{ needs.metadata.outputs.commit }}', checkouts[1])
        for checkout in checkouts:
            self.assertIn('persist-credentials: false', checkout)
        for forbidden in ('actions/cache@', 'github.token', '--privileged', '--device',
                          '--env-file', 'secrets.'):
            self.assertNotIn(forbidden, job)
        preparation = job.split('      - name: Prepare pinned Ubuntu', 1)[1].split('      - name:', 1)[0]
        self.assertNotIn('candidate-source', preparation)
        self.assertIn('prepare.py --output build/appimage-context', preparation)
        self.assertIn('--file build/appimage-context/Dockerfile build/appimage-context', preparation)
        self.assertIn('cp build/appimage-context/packaging.lock.json build/appimage-evidence/', preparation)
        self.assertIn('cat /build-packages.tsv', preparation)
        self.assertLess(job.index('cat /build-packages.tsv'), job.index('Build AppImage with offline'))
        smoke = job.split('      - name: Smoke in clean', 1)[1].split('      - name:', 1)[0].split('      - uses:', 1)[0]
        self.assertIn('for runtime in ubuntu24 ubuntu26 debian13', smoke)
        self.assertIn('--network none', smoke)
        self.assertIn('--env ATRINIK_APPIMAGE_SMOKE_CONTAINER=1', smoke)
        self.assertIn('--env ATRINIK_APPIMAGE_RUNTIME_PROBE=/input/appimage-runtime-probe', smoke)
        self.assertIn('--cap-drop ALL --security-opt no-new-privileges', smoke)
        self.assertNotIn('candidate-source', smoke)
        self.assertNotIn('target=/output', smoke)
        self.assertNotIn('apt-get', smoke)
        self.assertIn('tools/ci/appimage/smoke.sh,target=/smoke.sh,readonly', smoke)
        self.assertLess(job.index('bash /smoke.sh'), job.index('name: release-client-appimage-'))

    def test_required_trusted_validator_tools_and_attestation_cover_appimage(self) -> None:
        job = self.job()
        self.assertIn('python3 tools/release/appimage.py', job)
        self.assertLess(job.index('payload-validation.log'), job.index('bash /smoke.sh'))
        finalizer = self.workflow().split('  candidate:\n', 1)[1]
        self.assertLess(finalizer.index('squashfs-tools binutils'), finalizer.index('finalize_artifacts.py'))
        for name, first_consumer in (('package-release.yml', 'release_artifacts.py'),
                                     ('promote-latest.yml', 'check_latest_release.py')):
            workflow = (ROOT / '.github/workflows' / name).read_text()
            self.assertLess(workflow.index('squashfs-tools binutils'), workflow.index(first_consumer))
        check = (ROOT / '.github/workflows/check.yml').read_text()
        core = check.split('  core:\n', 1)[1].split('  windows-test-build:\n', 1)[0]
        self.assertIn('squashfs-tools binutils libzstd1', core)
        self.assertLess(core.index('squashfs-tools binutils libzstd1'), core.index('-m unittest discover -s tools/tests'))
        for prerequisite in ('readelf', 'unsquashfs', 'mksquashfs', 'true'):
            self.assertIn(f'test -x /usr/bin/{prerequisite}', core)
        self.assertIn('test -f /usr/lib/x86_64-linux-gnu/libzstd.so.1', core)
        self.assertIn('squashfs-tools binutils libzstd1 libssl3t64', core)
        self.assertIn('test -f /usr/lib/x86_64-linux-gnu/ossl-modules/legacy.so', core)
        package = (ROOT / '.github/workflows/package-release.yml').read_text()
        self.assertIn('            build/release/*.AppImage\n', package)
        for invocation in re.findall(r'sync_release_assets.py(.*?)(?=\n      -|\Z)',
                                     package, re.DOTALL):
            self.assertIn('--revision "${RELEASE_COMMIT}"', invocation)
            self.assertIn('--source-root "${GITHUB_WORKSPACE}"', invocation)

    def test_host_payload_validation_binds_candidate_git_coordinates_before_smoke(self) -> None:
        job = self.job()
        validation = job.split('      - name: Validate AppImage payload with trusted host tools', 1)[1].split('      - name:', 1)[0]
        self.assertIn('RELEASE_REVISION: ${{ needs.metadata.outputs.commit }}', validation)
        script = textwrap.dedent(validation.split('        run: |\n', 1)[1])
        for fail_validation in ('0', '1'):
            with self.subTest(fail_validation=fail_validation), tempfile.TemporaryDirectory() as temporary:
                workspace = Path(temporary)
                (workspace / 'bin').mkdir()
                (workspace / 'build/appimage-evidence').mkdir(parents=True)
                sudo = workspace / 'bin/sudo'
                sudo.write_text('#!/usr/bin/env sh\nexit 0\n')
                sudo.chmod(0o755)
                python = workspace / 'bin/python3'
                python.write_text(f'#!{sys.executable}\n' + """import json, os, pathlib, sys
pathlib.Path('validator-args.json').write_text(json.dumps(sys.argv[1:]))
sys.exit(7 if os.environ['FAIL_VALIDATION'] == '1' else 0)
""")
                python.chmod(0o755)
                result = subprocess.run(['bash', '-euo', 'pipefail', '-c', script],
                    cwd=workspace, capture_output=True, text=True,
                    env=dict(os.environ, GITHUB_WORKSPACE=str(workspace),
                             RELEASE_VERSION='5.17.0', RELEASE_REVISION='a' * 40,
                             FAIL_VALIDATION=fail_validation,
                             PATH=f"{workspace / 'bin'}:{os.environ['PATH']}"))
                self.assertEqual(result.returncode, 7 if fail_validation == '1' else 0, result.stderr)
                args = json.loads((workspace / 'validator-args.json').read_text())
                self.assertEqual(args, ['tools/release/appimage.py',
                    'build/appimage-packages/atrinik-classic-client-5.17.0-linux-x86_64.AppImage',
                    '5.17.0', '--revision', 'a' * 40,
                    '--source-root', str(workspace / 'candidate-source')])
                self.assertNotIn('candidate-source/tools', ' '.join(args))
        self.assertLess(job.index('--source-root "${GITHUB_WORKSPACE}/candidate-source"'),
                        job.index('"classic-appimage-${runtime}" bash /smoke.sh'))

    def test_preparation_captures_actual_builder_inventory_and_lock(self) -> None:
        preparation = self.job().split('      - name: Prepare pinned Ubuntu', 1)[1].split('      - name:', 1)[0]
        script = textwrap.dedent(preparation.split('        run: |\n', 1)[1])
        with tempfile.TemporaryDirectory() as temporary:
            workspace = Path(temporary)
            (workspace / 'bin').mkdir()
            trusted = workspace / 'tools/ci/appimage'
            trusted.mkdir(parents=True)
            (trusted / 'prepare.py').write_text("""import argparse, pathlib
parser = argparse.ArgumentParser()
parser.add_argument('--output', type=pathlib.Path, required=True)
out = parser.parse_args().output
out.mkdir(parents=True)
(out / 'packaging.lock.json').write_text('{"fixture": "locked inputs"}\\n')
(out / 'Dockerfile').write_text('fixture trusted Dockerfile\\n')
""")
            docker = workspace / 'bin/docker'
            docker.write_text("""#!/usr/bin/env python3
import json, pathlib, sys
with pathlib.Path('docker-calls.jsonl').open('a') as stream:
    stream.write(json.dumps(sys.argv[1:]) + '\\n')
if sys.argv[1] == 'run':
    print('fixture-package\\t1.2.3')
elif sys.argv[1:3] == ['image', 'inspect']:
    print('sha256:' + 'a' * 64)
""")
            docker.chmod(0o755)
            # Preparation must not touch candidate input, even if it contains
            # names that resemble the trusted preparation assets.
            candidate = workspace / 'candidate-source/tools/ci/appimage'
            candidate.mkdir(parents=True)
            (candidate / 'prepare.py').write_text('raise RuntimeError("candidate executed")')
            result = subprocess.run(['bash', '-euo', 'pipefail', '-c', script],
                cwd=workspace, capture_output=True, text=True,
                env=dict(os.environ, PATH=f"{workspace / 'bin'}:{os.environ['PATH']}"))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout, '')
            self.assertEqual((workspace / 'build/appimage-evidence/packaging.lock.json').read_text(),
                             '{"fixture": "locked inputs"}\n')
            self.assertEqual((workspace / 'build/appimage-evidence/build-packages.tsv').read_text(),
                             'fixture-package\t1.2.3\n')
            calls = [json.loads(line) for line in (workspace / 'docker-calls.jsonl').read_text().splitlines()]
            targets = [call[call.index('--target') + 1] for call in calls if call[0] == 'build' and '--target' in call]
            self.assertEqual(targets, ['build', 'smoke-ubuntu24', 'smoke-ubuntu26', 'smoke-debian13'])
            graphical = next(call for call in calls if 'classic-appimage-graphical' in call and call[0] == 'build')
            self.assertIn('tools/ci/appimage/graphical.Dockerfile', graphical)
            self.assertEqual(graphical[-1], 'build/appimage-context')
            for call in calls:
                self.assertNotIn('candidate-source', ' '.join(call))
            self.assertEqual(calls[-1], ['run', '--rm', '--network', 'none',
                                        'classic-appimage-build', 'cat', '/build-packages.tsv'])

    def test_probe_capture_rejects_failures_and_nonregular_host_outputs(self) -> None:
        job = self.job()
        step = job.split('      - name: Capture trusted runtime and graphical qualification probes', 1)[1].split('      - name:', 1)[0]
        script = textwrap.dedent(step.split('        run: |\n', 1)[1])
        cases = [('file', '')] + [(kind, probe) for kind in ('empty', 'failure', 'symlink', 'directory')
                                  for probe in ('appimage-runtime-probe', 'appimage-graphical-probe')]
        for output_type, bad_probe in cases:
            with self.subTest(output_type=output_type, bad_probe=bad_probe), tempfile.TemporaryDirectory() as temporary:
                workspace = Path(temporary)
                (workspace / 'bin').mkdir()
                (workspace / 'build').mkdir()
                (workspace / 'host-private').write_text('private')
                docker = workspace / 'bin/docker'
                docker.write_text(f'#!{sys.executable}\n' + """import json, os, pathlib, sys
with pathlib.Path('docker-calls.jsonl').open('a') as stream:
    stream.write(json.dumps(sys.argv[1:]) + '\\n')
probe = pathlib.Path('build') / pathlib.Path(sys.argv[-1]).name
kind = os.environ['OUTPUT_TYPE'] if probe.name == os.environ['BAD_PROBE'] else 'file'
if kind == 'failure':
    sys.exit(7)
elif kind == 'symlink':
    probe.unlink()
    probe.symlink_to(pathlib.Path('../host-private'))
elif kind == 'directory':
    probe.unlink()
    probe.mkdir()
if kind != 'empty':
    sys.stdout.buffer.write(b'trusted probe fixture')
""")
                docker.chmod(0o755)
                result = subprocess.run(['bash', '-euo', 'pipefail', '-c', script],
                    cwd=workspace, capture_output=True, text=True,
                    env=dict(os.environ, OUTPUT_TYPE=output_type, BAD_PROBE=bad_probe,
                             PATH=f"{workspace / 'bin'}:{os.environ['PATH']}"))
                self.assertEqual(result.returncode == 0, output_type == 'file', result.stderr)
                calls = [json.loads(line) for line in (workspace / 'docker-calls.jsonl').read_text().splitlines()]
                self.assertEqual(len(calls), 1 if bad_probe == 'appimage-runtime-probe' else 2)
                for index, args in enumerate(calls):
                    probe_name = ('appimage-runtime-probe', 'appimage-graphical-probe')[index]
                    self.assertEqual(args[-3:], ['classic-appimage-build', 'cat', '/' + probe_name])
                    self.assertEqual(args[args.index('--network') + 1], 'none')
                    self.assertNotIn('--mount', args)
                    self.assertNotIn('candidate-source', ' '.join(args))
                self.assertEqual((workspace / 'host-private').read_text(), 'private')
                if output_type == 'file':
                    for probe in ('appimage-runtime-probe', 'appimage-graphical-probe'):
                        self.assertEqual((workspace / 'build' / probe).read_bytes(), b'trusted probe fixture')
                        self.assertEqual((workspace / 'build' / probe).stat().st_mode & 0o777, 0o755)
        self.assertLess(job.index('Capture trusted runtime and graphical qualification probes'), job.index('Build AppImage with offline'))

    def test_smoke_loop_requires_each_clean_runtime_success(self) -> None:
        smoke = self.job().split('      - name: Smoke in clean', 1)[1].split('      - name:', 1)[0].split('      - uses:', 1)[0]
        script = textwrap.dedent(smoke.split('        run: |\n', 1)[1])
        for fail_runtime in ('', 'ubuntu24', 'ubuntu26', 'debian13'):
            with self.subTest(fail_runtime=fail_runtime), tempfile.TemporaryDirectory() as temporary:
                workspace = Path(temporary)
                (workspace / 'bin').mkdir()
                (workspace / 'build/appimage-evidence').mkdir(parents=True)
                docker = workspace / 'bin/docker'
                docker.write_text("""#!/usr/bin/env python3
import json, os, pathlib, sys
with pathlib.Path('docker-calls.jsonl').open('a') as stream:
    stream.write(json.dumps(sys.argv[1:]) + '\\n')
failed = os.environ['FAIL_RUNTIME']
sys.exit(7 if failed and 'classic-appimage-' + failed in sys.argv else 0)
""")
                docker.chmod(0o755)
                result = subprocess.run(['bash', '-euo', 'pipefail', '-c', script],
                    cwd=workspace, capture_output=True, text=True,
                    env=dict(os.environ, **self.RUNNER_AUTHORITY, GITHUB_WORKSPACE=str(workspace),
                             RELEASE_VERSION='0.0.0', FAIL_RUNTIME=fail_runtime,
                             PATH=f"{workspace / 'bin'}:{os.environ['PATH']}"))
                self.assertEqual(result.returncode == 0, not fail_runtime, result.stderr)
                calls = [json.loads(line) for line in (workspace / 'docker-calls.jsonl').read_text().splitlines()]
                self.assertEqual(len(calls), {'': 3, 'ubuntu24': 1, 'ubuntu26': 2, 'debian13': 3}[fail_runtime])
                for call in calls:
                    self.assert_isolated_container(call, {
                        'ATRINIK_APPIMAGE_SMOKE_CONTAINER', 'ATRINIK_APPIMAGE_RUNTIME_PROBE'})
                    self.assertEqual(call[call.index('--network') + 1], 'none')
                    mounts = [call[i + 1] for i, arg in enumerate(call) if arg == '--mount']
                    self.assertEqual(len(mounts), 3)
                    self.assertTrue(all(mount.endswith(',readonly') for mount in mounts))
                    self.assertFalse(any('candidate-source' in mount for mount in mounts))
                    self.assertIn(f'type=bind,source={workspace}/build/appimage-runtime-probe,target=/input/appimage-runtime-probe,readonly', mounts)
                    self.assertIn('ATRINIK_APPIMAGE_RUNTIME_PROBE=/input/appimage-runtime-probe', call)
                    self.assertEqual(call[-2:], ['/packages/atrinik-classic-client-0.0.0-linux-x86_64.AppImage', '0.0.0'])

    def test_graphical_qualification_retains_safe_evidence_and_removes_only_owned_container(self) -> None:
        from tools.tests.test_export_graphical_evidence import archive_bytes, exporter
        import shutil
        import tarfile

        job = self.job()
        step = job.split('      - name: Qualify graphical dependencies, virtual window, and virtual audio', 1)[1].split('      - uses:', 1)[0]
        script = textwrap.dedent(step.split('        run: |\n', 1)[1])
        for case in ('success', 'smoke-failure', 'unsafe-archive', 'copy-failure', 'wrong-owner'):
            with self.subTest(case=case), tempfile.TemporaryDirectory() as temporary:
                workspace = Path(temporary)
                (workspace / 'bin').mkdir()
                (workspace / 'build/appimage-evidence').mkdir(parents=True)
                (workspace / 'host-private').write_text('private')
                helper = workspace / 'tools/ci/appimage/export_graphical_evidence.py'
                helper.parent.mkdir(parents=True)
                shutil.copyfile(ROOT / 'tools/ci/appimage/export_graphical_evidence.py', helper)
                records = [(name, name.encode(), tarfile.REGTYPE) for name in exporter.FILES]
                if case == 'smoke-failure':
                    records = [('probe.log', b'failure diagnostic', tarfile.REGTYPE)]
                elif case == 'unsafe-archive':
                    records = [('../../host-private', b'replace private', tarfile.REGTYPE)]
                (workspace / 'copy.tar').write_bytes(archive_bytes(records))
                docker = workspace / 'bin/docker'
                docker.write_text(f'#!{sys.executable}\n' + """import json, os, pathlib, sys
with pathlib.Path('docker-calls.jsonl').open('a') as stream:
    stream.write(json.dumps(sys.argv[1:]) + '\\n')
case = os.environ['GRAPHICAL_CASE']
action = sys.argv[1]
if action == 'create':
    print('a' * 64)
elif action == 'start':
    sys.exit(7 if case == 'smoke-failure' else 0)
elif action == 'cp':
    if case == 'copy-failure':
        sys.exit(9)
    sys.stdout.buffer.write(pathlib.Path('copy.tar').read_bytes())
elif action == 'inspect':
    print('/changed-owner 123-2' if case == 'wrong-owner' else '/classic-appimage-graphical-123-2 123-2')
elif action == 'rm':
    pathlib.Path('container-removed').write_text(sys.argv[-1])
else:
    sys.exit(99)
""")
                docker.chmod(0o755)
                result = subprocess.run(['bash', '-euo', 'pipefail', '-c', script],
                    cwd=workspace, capture_output=True, text=True,
                    env=dict(os.environ, **self.RUNNER_AUTHORITY, GITHUB_WORKSPACE=str(workspace),
                             GITHUB_RUN_ID='123', GITHUB_RUN_ATTEMPT='2',
                             RELEASE_VERSION='5.17.0', GRAPHICAL_CASE=case,
                             PATH=f"{workspace / 'bin'}:{os.environ['PATH']}"))
                self.assertEqual(result.returncode == 0, case == 'success', result.stderr)
                calls = [json.loads(line) for line in (workspace / 'docker-calls.jsonl').read_text().splitlines()]
                args = calls[0]
                self.assert_isolated_container(args, {
                    'ATRINIK_APPIMAGE_SMOKE_CONTAINER', 'ATRINIK_APPIMAGE_GRAPHICAL_PROBE'})
                self.assertEqual(args[0], 'create')
                self.assertNotIn('--rm', args)
                self.assertIn('classic-appimage-graphical-123-2', args)
                self.assertIn('atrinik.ci.graphical-owner=123-2', args)
                self.assertEqual(args[args.index('--network') + 1], 'none')
                self.assertEqual(args[args.index('--cap-drop') + 1], 'ALL')
                self.assertEqual(args[args.index('--security-opt') + 1], 'no-new-privileges')
                self.assertIn('--user', args)
                self.assertIn('ATRINIK_APPIMAGE_SMOKE_CONTAINER=1', args)
                self.assertIn('ATRINIK_APPIMAGE_GRAPHICAL_PROBE=/input/appimage-graphical-probe', args)
                mounts = [args[i + 1] for i, arg in enumerate(args) if arg == '--mount']
                self.assertEqual(mounts, [
                    f'type=bind,source={workspace}/build/appimage-packages,target=/packages,readonly',
                    f'type=bind,source={workspace}/tools/ci/appimage/graphical-smoke.sh,target=/graphical-smoke.sh,readonly',
                    f'type=bind,source={workspace}/build/appimage-graphical-probe,target=/input/appimage-graphical-probe,readonly'])
                self.assertEqual(args[-3:], ['/packages/atrinik-classic-client-5.17.0-linux-x86_64.AppImage',
                                            '5.17.0', '/tmp/graphical-evidence'])
                for forbidden in ('--privileged', '--device', '--env-file', '--volume', '--publish'):
                    self.assertNotIn(forbidden, args)
                self.assertEqual(calls[1], ['start', '--attach', 'a' * 64])
                self.assertEqual(calls[2], ['cp', 'a' * 64 + ':/tmp/graphical-evidence/.', '-'])
                self.assertEqual(calls[3][-1], 'a' * 64)
                if case == 'wrong-owner':
                    self.assertFalse((workspace / 'container-removed').exists())
                    self.assertEqual(len(calls), 4)
                else:
                    self.assertEqual(calls[-1], ['rm', '--force', 'a' * 64])
                    self.assertEqual((workspace / 'container-removed').read_text(), 'a' * 64)
                evidence = workspace / 'build/appimage-evidence/graphical'
                if case in ('success', 'wrong-owner'):
                    self.assertEqual({file.name for file in evidence.iterdir()}, exporter.FILES)
                    self.assertEqual((evidence / 'software-vulkan-frame.xwd').read_bytes(), b'software-vulkan-frame.xwd')
                elif case == 'smoke-failure':
                    self.assertEqual(result.returncode, 7)
                    self.assertEqual((evidence / 'probe.log').read_bytes(), b'failure diagnostic')
                else:
                    self.assertFalse(evidence.exists())
                self.assertEqual((workspace / 'host-private').read_text(), 'private')
        self.assertLess(job.index('payload-validation.log'), job.index('bash /graphical-smoke.sh'))
        self.assertLess(job.index('bash /smoke.sh'), job.index('bash /graphical-smoke.sh'))
        self.assertLess(job.index('export_graphical_evidence.py'), job.index('name: release-client-appimage-'))

    def test_container_boundary_and_host_output_validation(self) -> None:
        workflow = (ROOT / '.github/workflows/build-release-candidate.yml').read_text()
        step = workflow.split('      - name: Build AppImage with offline release inputs', 1)[1]
        step = step.split('      - name:', 1)[0]
        script = textwrap.dedent(step.split('        run: |\n', 1)[1])
        # Exercise the actual workflow shell with an inert Docker fixture. The
        # fixture emits candidate-controlled filesystem types into its one
        # writable mount; host consumers must reject everything except a file.
        for output_type in ('file', 'symlink', 'directory', 'missing'):
            with self.subTest(output_type=output_type), tempfile.TemporaryDirectory() as temporary:
                workspace = Path(temporary)
                (workspace / 'build/appimage-evidence').mkdir(parents=True)
                (workspace / 'build/appimage-packages').mkdir()
                (workspace / 'bin').mkdir()
                docker = workspace / 'bin/docker'
                docker.write_text('''#!/usr/bin/env python3
import json, os, pathlib, sys
root = pathlib.Path(os.environ['GITHUB_WORKSPACE'])
(root / 'docker-args.json').write_text(json.dumps(sys.argv[1:]))
package = root / 'build/appimage-packages/atrinik-classic-client-0.0.0-linux-x86_64.AppImage'
kind = os.environ['OUTPUT_TYPE']
if kind == 'file':
    package.write_bytes(b'fixture')
elif kind == 'symlink':
    package.symlink_to(root / 'host-private')
elif kind == 'directory':
    package.mkdir()
print('::warning::candidate output stays in evidence')
''')
                docker.chmod(0o755)
                (workspace / 'host-private').write_text('do not upload')
                environment = dict(os.environ, **self.RUNNER_AUTHORITY, GITHUB_WORKSPACE=str(workspace),
                                   RELEASE_VERSION='0.0.0', RELEASE_REVISION='a' * 40, SOURCE_DATE_EPOCH='1',
                                   DISCORD_CONFIG_FILE='', OUTPUT_TYPE=output_type,
                                   PATH=f"{workspace / 'bin'}:{os.environ['PATH']}")
                result = subprocess.run(['bash', '-euo', 'pipefail', '-c', script],
                                        cwd=workspace, env=environment, capture_output=True, text=True)
                self.assertEqual(result.returncode == 0, output_type == 'file', result.stderr)
                self.assertEqual(result.stdout, '')
                args = json.loads((workspace / 'docker-args.json').read_text())
                self.assert_isolated_container(args, {
                    'ATRINIK_PACKAGE_VERSION', 'ATRINIK_SOURCE_REVISION', 'SOURCE_DATE_EPOCH',
                    'CMAKE_BUILD_PARALLEL_LEVEL', 'ATRINIK_GPU_SHADER_DIRECTORY',
                    'ATRINIK_DEPENDENCY_DOWNLOADS', 'ATRINIK_DISCORD_APPLICATION_ID_FILE'})
                self.assertEqual(args[args.index('--network') + 1], 'none')
                self.assertEqual(args[args.index('--cap-drop') + 1], 'ALL')
                self.assertEqual(args[args.index('--security-opt') + 1], 'no-new-privileges')
                self.assertIn('--user', args)
                mounts = [args[i + 1] for i, arg in enumerate(args) if arg == '--mount']
                self.assertEqual(len(mounts), 5)
                self.assertEqual([mount for mount in mounts if not mount.endswith(',readonly')],
                                 [f'type=bind,source={workspace}/build/appimage-packages,target=/output'])
                self.assertIn(f'type=bind,source={workspace}/candidate-source,target=/input/source,readonly', mounts)
                for forbidden in ('--privileged', '--pid', '--env-file', '--volume'):
                    self.assertNotIn(forbidden, args)
                passed_env = [args[i + 1].split('=', 1)[0] for i, arg in enumerate(args) if arg == '--env']
                self.assertEqual(set(passed_env), {'ATRINIK_PACKAGE_VERSION', 'ATRINIK_SOURCE_REVISION', 'SOURCE_DATE_EPOCH',
                    'CMAKE_BUILD_PARALLEL_LEVEL', 'ATRINIK_GPU_SHADER_DIRECTORY',
                    'ATRINIK_DEPENDENCY_DOWNLOADS', 'ATRINIK_DISCORD_APPLICATION_ID_FILE'})
                self.assertIn('cp -a --no-preserve=ownership /input/source/. /tmp/source/', args[-1])
                self.assertIn('cd /tmp/source/client', args[-1])
                self.assertIn('test -f tools/tests/test_appimage_package_contract.py', args[-1])
                self.assertIn('python3 -m unittest discover -s tools/tests -p test_appimage_package_contract.py', args[-1])
                self.assertIn('bash tools/build-appimage.sh /output', args[-1])


if __name__ == '__main__':
    unittest.main()
