from pathlib import Path
import re
import json
import os
import subprocess
import tempfile
import textwrap
import unittest


ROOT = Path(__file__).resolve().parents[2]


class LinuxDebWorkflowTests(unittest.TestCase):
    def test_candidate_requires_offline_build_and_clean_install(self) -> None:
        workflow = (ROOT / '.github/workflows/build-release-candidate.yml').read_text()
        job = workflow.split('  client-linux:\n', 1)[1].split('  client-windows:\n', 1)[0]
        self.assertIn('needs: [metadata, dependencies, gpu-shaders]', job)
        self.assertIn("if: needs.metadata.outputs.artifact_schema == '2'", job)
        self.assertIn('ref: ${{ needs.metadata.outputs.commit }}', job)
        self.assertIn('persist-credentials: false', job)
        self.assertIn('--network none', job)
        self.assertIn('--env CMAKE_BUILD_PARALLEL_LEVEL=2', job)
        self.assertIn('bash tools/build-linux-package.sh /output', job)
        self.assertIn('classic-deb-runtime bash /smoke.sh', job)
        self.assertIn('build/linux-packages/atrinik-classic-client-${{ needs.metadata.outputs.version }}-linux-amd64.deb', job)
        candidate = workflow.split('  candidate:\n', 1)[1]
        self.assertIn('      - client-linux\n', candidate)
        self.assertIn("needs.metadata.outputs.artifact_schema == '2' && needs.client-linux.result == 'success'", candidate)
        self.assertIn("needs.metadata.outputs.artifact_schema == '1' && needs.client-linux.result == 'skipped'", candidate)
        metadata = workflow.split('  metadata:\n', 1)[1].split('  dependencies:\n', 1)[0]
        self.assertIn('--print-source-schema --revision "${commit}"', metadata)
        self.assertIn('--source-root "${GITHUB_WORKSPACE}"', metadata)
        self.assertIn('ref: ${{ github.sha }}', metadata)

        self.assertLess(job.index('classic-deb-runtime bash /smoke.sh'),
                        job.index('name: release-client-linux-'))

    def test_candidate_automation_and_source_have_separate_authority(self) -> None:
        workflow = (ROOT / '.github/workflows/build-release-candidate.yml').read_text()
        job = workflow.split('  client-linux:\n', 1)[1].split('  client-windows:\n', 1)[0]
        checkouts = re.findall(r'uses: actions/checkout@.*?\n(.*?)(?=      -)', job, re.DOTALL)
        self.assertEqual(len(checkouts), 2)
        self.assertIn('ref: ${{ github.sha }}', checkouts[0])
        self.assertIn('sparse-checkout: tools/ci', checkouts[0])
        self.assertNotIn('path:', checkouts[0])
        self.assertIn('ref: ${{ needs.metadata.outputs.commit }}', checkouts[1])
        self.assertIn('path: candidate-source', checkouts[1])
        for checkout in checkouts:
            self.assertIn('persist-credentials: false', checkout)
        self.assertIn('permissions:\n      contents: read', job)
        self.assertNotIn('actions/cache@', job)
        self.assertNotIn('github.token', job)
        preparation = job.split('name: Prepare pinned Debian', 1)[1].split('      - name:', 1)[0]
        self.assertNotIn('candidate-source', preparation)
        self.assertIn('--file tools/ci/debian-client/Dockerfile tools/ci/debian-client', preparation)
        smoke = job.split('name: Install through APT', 1)[1].split('      - uses:', 1)[0]
        self.assertNotIn('candidate-source', smoke)
        self.assertIn('$GITHUB_WORKSPACE/tools/ci/smoke_linux_client_package.sh,target=/smoke.sh,readonly', smoke)
        self.assertNotIn('target=/output', smoke)

    def test_container_boundary_and_host_output_validation(self) -> None:
        workflow = (ROOT / '.github/workflows/build-release-candidate.yml').read_text()
        step = workflow.split('      - name: Build client with distribution libraries', 1)[1]
        step = step.split('      - name:', 1)[0]
        script = textwrap.dedent(step.split('        run: |\n', 1)[1])
        # Exercise the actual workflow shell with an inert Docker fixture. The
        # fixture emits candidate-controlled filesystem types into its one
        # writable mount; host consumers must reject everything except a file.
        for output_type in ('file', 'symlink', 'directory', 'missing'):
            with self.subTest(output_type=output_type), tempfile.TemporaryDirectory() as temporary:
                workspace = Path(temporary)
                (workspace / 'build/linux-package-evidence').mkdir(parents=True)
                (workspace / 'build/linux-packages').mkdir()
                (workspace / 'bin').mkdir()
                docker = workspace / 'bin/docker'
                docker.write_text('''#!/usr/bin/env python3
import json, os, pathlib, sys
root = pathlib.Path(os.environ['GITHUB_WORKSPACE'])
(root / 'docker-args.json').write_text(json.dumps(sys.argv[1:]))
package = root / 'build/linux-packages/atrinik-classic-client-0.0.0-linux-amd64.deb'
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
                environment = dict(os.environ, GITHUB_WORKSPACE=str(workspace),
                                   RELEASE_VERSION='0.0.0', SOURCE_DATE_EPOCH='1',
                                   DISCORD_CONFIG_FILE='', OUTPUT_TYPE=output_type,
                                   PATH=f"{workspace / 'bin'}:{os.environ['PATH']}")
                result = subprocess.run(['bash', '-euo', 'pipefail', '-c', script],
                                        cwd=workspace, env=environment, capture_output=True, text=True)
                self.assertEqual(result.returncode == 0, output_type == 'file', result.stderr)
                self.assertEqual(result.stdout, '')
                args = json.loads((workspace / 'docker-args.json').read_text())
                self.assertEqual(args[args.index('--network') + 1], 'none')
                self.assertEqual(args[args.index('--cap-drop') + 1], 'ALL')
                self.assertEqual(args[args.index('--security-opt') + 1], 'no-new-privileges')
                self.assertIn('--user', args)
                mounts = [args[i + 1] for i, arg in enumerate(args) if arg == '--mount']
                self.assertEqual(len(mounts), 5)
                self.assertEqual([mount for mount in mounts if not mount.endswith(',readonly')],
                                 [f'type=bind,source={workspace}/build/linux-packages,target=/output'])
                self.assertIn(f'type=bind,source={workspace}/candidate-source,target=/input/source,readonly', mounts)
                for forbidden in ('--privileged', '--pid', '--env-file', '--volume'):
                    self.assertNotIn(forbidden, args)
                passed_env = [args[i + 1].split('=', 1)[0] for i, arg in enumerate(args) if arg == '--env']
                self.assertEqual(set(passed_env), {'ATRINIK_PACKAGE_VERSION', 'SOURCE_DATE_EPOCH',
                    'CMAKE_BUILD_PARALLEL_LEVEL', 'ATRINIK_GPU_SHADER_DIRECTORY',
                    'ATRINIK_DEPENDENCY_DOWNLOADS', 'ATRINIK_DISCORD_APPLICATION_ID_FILE'})
                self.assertIn('cp -a --no-preserve=ownership /input/source/. /tmp/source/', args[-1])
                self.assertIn('cd /tmp/source/client', args[-1])
                self.assertIn('test -f tools/tests/test_linux_package_contract.py', args[-1])
                self.assertIn('python3 -m unittest discover -s tools/tests -p test_linux_package_contract.py', args[-1])
                self.assertIn('bash tools/build-linux-package.sh /output', args[-1])

    def test_build_and_install_share_a_pinned_distribution_snapshot(self) -> None:
        dockerfile = (ROOT / 'tools/ci/debian-client/Dockerfile').read_text()
        self.assertRegex(dockerfile, r'FROM debian:forky-slim@sha256:[0-9a-f]{64} AS runtime')
        self.assertIn('FROM runtime AS build', dockerfile)
        self.assertIn('snapshot.debian.org/archive/debian/20261009T000000Z/', dockerfile)
        for dependency in ('libsdl3-dev', 'libsdl3-image-dev', 'libsdl3-mixer-dev',
                           'libsdl3-ttf-dev', 'libssl-dev', 'dpkg-dev'):
            self.assertIn(dependency, dockerfile)
        self.assertNotIn('--allow-unauthenticated', dockerfile)
        self.assertNotIn('Verify-Peer=false', dockerfile)
        self.assertIn('/build-packages.tsv', dockerfile)

    def test_publication_binds_validation_to_source_and_attests_deb(self) -> None:
        workflow = (ROOT / '.github/workflows/package-release.yml').read_text()
        self.assertNotIn('-eq 12', workflow)
        self.assertIn('tools/release/release_artifacts.py', workflow)
        self.assertIn('--revision "${RELEASE_COMMIT}"', workflow)
        self.assertIn('--source-root "${GITHUB_WORKSPACE}"', workflow)
        self.assertIn('            build/release/*.deb\n', workflow)
        for invocation in re.findall(r'sync_release_assets.py(.*?)(?=\n      -|\Z)',
                                     workflow, re.DOTALL):
            self.assertIn('--revision "${RELEASE_COMMIT}"', invocation)
            self.assertIn('--source-root "${GITHUB_WORKSPACE}"', invocation)


if __name__ == '__main__':
    unittest.main()
