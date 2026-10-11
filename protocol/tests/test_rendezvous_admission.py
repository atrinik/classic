"""Independent byte-level admission fixture and generated-binding checks."""
from __future__ import annotations

import hashlib
import hmac
import importlib.util
import json
from pathlib import Path
import re
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
SPEC = importlib.util.spec_from_file_location(
    'admission_bindings', ROOT / 'generated/python/atrinik_protocol/rendezvous_admission.py')
assert SPEC is not None and SPEC.loader is not None
BINDINGS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BINDINGS)


class AdmissionContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.fixture = json.loads((ROOT / 'fixtures/rendezvous-admission-v1.json').read_text())
        cls.constants = json.loads((ROOT / 'schema/rendezvous-admission.json').read_text())['constants']

    def test_generated_domains_and_fixed_bounds(self):
        c = self.constants
        for name, value in c.items():
            self.assertEqual(getattr(BINDINGS, 'ATRINIK_RENDEZVOUS_ADMISSION_' + name), value)
        self.assertEqual(c['SCHEMA'], 'atrinik-rendezvous-admission-v1')
        self.assertEqual(c['HMAC_DOMAIN'], c['SCHEMA'])
        self.assertEqual(c['WORK_DOMAIN'], 'atrinik-rendezvous-work-v1')
        self.assertEqual(c['DIFFICULTY'], 24)
        self.assertEqual(c['TRIALS_MAX'], 1 << 27)
        self.assertEqual(c['CHECK_INTERVAL'], 4096)
        self.assertEqual(c['UTC_SECONDS_MAX'], 253402300799)
        self.assertEqual(c['BUDGET_MS'], 10000)

    def verify(self, value, now, role):
        c = self.constants
        token = value['token']
        header = token + '.' + value['solution']
        if len(header) > c['PROOF_MAX'] or not re.fullmatch('[0-9a-f]{16}', value['solution']):
            return False
        if not re.fullmatch('[0-9a-f]{64}', value['ticket']):
            return False
        parsed = re.fullmatch(r'v1\.([A-Za-z0-9_-]{1,32})\.([1-9][0-9]{0,11})\.([0-9a-f]{64})\.([0-9a-f]{32})\.([0-9a-f]{64})', token)
        if parsed is None:
            return False
        key_id, issued, ticket_hash, _, mac = parsed.groups()
        expiry = int(issued) + c['LIFETIME_SECONDS']
        if expiry > c['UTC_SECONDS_MAX'] or now < int(issued) or now >= expiry:
            return False
        if role not in {'current', 'previous'} or key_id != self.fixture['keyId']:
            return False
        if ticket_hash != hashlib.sha256(bytes.fromhex(value['ticket'])).hexdigest():
            return False
        prefix = token.rsplit('.', 1)[0]
        signing = '\0'.join([c['HMAC_DOMAIN'], value['authority'], 'classic', value['serverId'], prefix]).encode('ascii')
        expected = hmac.new(bytes.fromhex(value['keyHex']), signing, hashlib.sha256).hexdigest()
        if not hmac.compare_digest(expected, mac):
            return False
        work = '\0'.join([c['WORK_DOMAIN'], token, value['solution']]).encode('ascii')
        return hashlib.sha256(work).digest()[:3] == bytes(3)

    def test_schema_rejects_unsafe_literals_and_overflow(self):
        spec = importlib.util.spec_from_file_location('generate_admission', ROOT / 'tools/generate.py')
        assert spec is not None and spec.loader is not None
        generator = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(generator)
        for name, invalid in [('WORK_DOMAIN', 'work"injection'), ('WORK_DOMAIN', 'x' * 65),
                              ('DIFFICULTY', True), ('REQUEST_MAX', 1 << 32),
                              ('UTC_SECONDS_MAX', 1 << 64)]:
            with self.subTest(name=name, value=invalid), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                (root / 'schema').mkdir()
                data = {'schema_version': 1, 'constants': {**self.constants, name: invalid}}
                (root / 'schema/rendezvous-admission.json').write_text(json.dumps(data))
                with self.assertRaises(ValueError):
                    generator.admission_outputs(root)

    def test_shared_vectors_and_exact_input_bytes(self):
        f = self.fixture
        signing = '\0'.join([self.constants['HMAC_DOMAIN'], f['authority'], 'classic', f['serverId'], f['tokenPrefix']]).encode()
        self.assertEqual(signing.hex(), f['hmacInputHex'])
        work = '\0'.join([self.constants['WORK_DOMAIN'], f['token'], f['solution']]).encode()
        self.assertEqual(work.hex(), f['workInputHex'])
        self.assertEqual(hashlib.sha256(work).hexdigest(), f['workDigest'])
        self.assertEqual(str(int(f['issuedAt']) + self.constants['LIFETIME_SECONDS']), f['expiresAt'])
        for case in f['cases']:
            with self.subTest(case=case['name']):
                self.assertEqual(self.verify({**f, **case.get('override', {})}, case['now'], case.get('keyRole', 'current')), case['accepted'])

    def test_shared_request_and_response_grammar(self):
        c = self.constants
        for group, fields, limit in [
                ('requestCases', ['schema', 'ticketHash'], c['REQUEST_MAX']),
                ('responseCases', ['schema', 'challenge', 'difficulty', 'expiresAt'], c['RESPONSE_MAX'])]:
            for case in self.fixture[group]:
                with self.subTest(group=group, case=case['name']):
                    accepted = False
                    try:
                        raw = case['raw']
                        if len(raw.encode('utf-8')) > limit:
                            raise ValueError('bound')
                        def pairs(items):
                            if [name for name, _ in items] != fields:
                                raise ValueError('keys')
                            return dict(items)
                        value = json.loads(raw, object_pairs_hook=pairs)
                        if not isinstance(value, dict) or json.dumps(value, separators=(',', ':')) != raw:
                            raise ValueError('canonical')
                        if value['schema'] != c['SCHEMA']:
                            raise ValueError('schema')
                        if group == 'requestCases':
                            if not isinstance(value['ticketHash'], str) or not re.fullmatch('[0-9a-f]{64}', value['ticketHash']):
                                raise ValueError('hash')
                        else:
                            token = value['challenge']
                            expiry = value['expiresAt']
                            if not isinstance(token, str) or len(token) > c['TOKEN_MAX']:
                                raise ValueError('token')
                            matched = re.fullmatch(r'v1\.([A-Za-z0-9_-]{1,32})\.([1-9][0-9]{0,11})\.([0-9a-f]{64})\.([0-9a-f]{32})\.([0-9a-f]{64})', token)
                            if matched is None:
                                raise ValueError('token grammar')
                            if type(value['difficulty']) is not int or value['difficulty'] != c['DIFFICULTY']:
                                raise ValueError('difficulty')
                            if not isinstance(expiry, str) or not re.fullmatch('[1-9][0-9]{0,11}', expiry):
                                raise ValueError('expiry')
                            expected = int(matched[2]) + c['LIFETIME_SECONDS']
                            if expected > c['UTC_SECONDS_MAX'] or str(expected) != expiry:
                                raise ValueError('expiry bound or mismatch')
                        accepted = True
                    except ValueError:
                        pass
                    self.assertEqual(accepted, case['accepted'])

    def test_shared_fixture_license_and_immutable_digest(self):
        path = ROOT / 'fixtures/rendezvous-admission-v1.json'
        self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(), '6c98dd74b612c67af7b2692367a7c4cab3c55769859110256b4a67dbb874ca74')
        self.assertIn('MIT License', (ROOT / 'fixtures/rendezvous-admission-LICENSE.txt').read_text())


if __name__ == '__main__':
    unittest.main()
