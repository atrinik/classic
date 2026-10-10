from pathlib import Path
import re
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
        self.assertIn('bash tools/build-linux-package.sh build/packages', job)
        self.assertIn('classic-deb-runtime bash /smoke.sh', job)
        self.assertIn('client/build/packages/*.deb', job)
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
