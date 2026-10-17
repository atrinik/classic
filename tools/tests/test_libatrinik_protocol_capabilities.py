"""Exercise the real configure/installed protocol gate without downloads."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
GATE = ROOT / "libatrinik/cmake/RequireClassicProtocol.cmake"


class ProtocolCapabilityTests(unittest.TestCase):
    def test_source_and_installed_headers_and_incompatible_inputs(self):
        for case in ('source', 'installed', 'missing_talk', 'missing_admission',
                     'pre_talk', 'old_revision', 'wrong_mode', 'wrong_bound',
                     'wrong_framing', 'comment_only', 'block_comment', 'bound_expression',
                     'framing_expression', 'shadowed', 'reconfigure'):
            with self.subTest(case=case), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                headers = root / 'include/atrinik/protocol'
                headers.mkdir(parents=True)
                for name, source in (
                    ('game_commands.h', ROOT / 'protocol/generated/c/include/atrinik/protocol/game_commands.h'),
                    ('rendezvous_admission.h', ROOT / 'protocol/generated/c/include/atrinik/protocol/rendezvous_admission.h'),
                    ('talk.h', ROOT / 'protocol/include/atrinik/protocol/talk.h'),
                ):
                    shutil.copyfile(source, headers / name)
                commands = headers / 'game_commands.h'
                if case in ('missing_talk', 'missing_admission'):
                    (headers / ('talk.h' if case == 'missing_talk' else 'rendezvous_admission.h')).unlink()
                replacements = {
                    'pre_talk': ('CMD_TALK_NPC_TARGET = 7,', ''),
                    'old_revision': ('VERSION 1085U', 'VERSION 1083U'),
                    'wrong_mode': ('CMD_TALK_CLOSE = 6,', 'CMD_TALK_CLOSE = 8,'),
                    'wrong_bound': ('MESSAGE_MAX 4095U', 'MESSAGE_MAX 4096U'),
                    'comment_only': ('CMD_TALK_NPC_TARGET = 7,', '// CMD_TALK_NPC_TARGET = 7,'),
                    'block_comment': ('CMD_TALK_NPC_TARGET = 7,', '/*\n    CMD_TALK_NPC_TARGET = 7,\n*/'),
                    'bound_expression': ('MESSAGE_MAX 4095U', 'MESSAGE_MAX 4095U + 1'),
                }
                if case in replacements:
                    commands.write_text(commands.read_text().replace(*replacements[case]))
                if case in ('wrong_framing', 'framing_expression'):
                    talk = headers / 'talk.h'
                    talk.write_text(talk.read_text().replace('FIXED_SIZE 8U', 'FIXED_SIZE ' + ('9U' if case == 'wrong_framing' else '8U + 1')))
                include = str(root / 'include')
                if case == 'source':
                    include = '$<BUILD_INTERFACE:' + include + '>'
                if case == 'shadowed':
                    earlier = root / 'earlier/atrinik/protocol'
                    earlier.mkdir(parents=True)
                    (earlier / 'game_commands.h').write_text(commands.read_text().replace('VERSION 1085U', 'VERSION 1072U'))
                    include = str(root / 'earlier') + ';' + include
                (root / 'CMakeLists.txt').write_text(
                    'cmake_minimum_required(VERSION 3.21)\nproject(capabilities NONE)\n'
                    'add_library(protocol INTERFACE)\nadd_library(Atrinik::Protocol ALIAS protocol)\n'
                    f'target_include_directories(protocol INTERFACE "{include}")\n'
                    f'include("{GATE}")\natrinik_require_classic_protocol()\n')
                args = ['cmake', '-S', str(root), '-B', str(root / 'build')]
                result = subprocess.run(args, capture_output=True, text=True)
                expected = case in ('source', 'installed', 'reconfigure')
                self.assertEqual(result.returncode == 0, expected, result.stdout + result.stderr)
                if not expected:
                    self.assertIn('Selected Atrinik::Protocol', result.stdout + result.stderr)
                if case == 'reconfigure':
                    commands.write_text(commands.read_text().replace('CMD_TALK_NPC_TARGET = 7,', ''))
                    result = subprocess.run(args, capture_output=True, text=True)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn('CMD_TALK_NPC_TARGET', result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
