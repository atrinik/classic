from __future__ import annotations

import importlib.util
import io
from pathlib import Path
import tarfile
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("graphical_evidence", ROOT / "tools/ci/appimage/export_graphical_evidence.py")
assert SPEC is not None and SPEC.loader is not None
exporter = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(exporter)


def archive_bytes(records, root="."):
    result = io.BytesIO()
    with tarfile.open(fileobj=result, mode="w") as archive:
        if root is not None:
            member = tarfile.TarInfo(root)
            member.type = tarfile.DIRTYPE
            archive.addfile(member)
        for name, data, kind in records:
            member = tarfile.TarInfo(name)
            member.type = kind
            member.mode = 0o777
            member.size = len(data) if kind in {tarfile.REGTYPE, tarfile.AREGTYPE} else 0
            member.linkname = "../../host-private"
            archive.addfile(member, io.BytesIO(data) if member.size else None)
    return result.getvalue()


class GraphicalEvidenceExportTests(unittest.TestCase):
    def test_complete_copy_exports_fixed_nonexecutable_regular_files(self):
        for prefix, root in (("./", "."), ("graphical-evidence/", "graphical-evidence/")):
            with self.subTest(prefix=prefix), tempfile.TemporaryDirectory() as temporary:
                output = Path(temporary) / "evidence"
                records = [(prefix + name, name.encode(), tarfile.REGTYPE) for name in exporter.FILES]
                names = exporter.export_evidence(io.BytesIO(archive_bytes(records, root)), output, True)
                self.assertEqual(set(names), exporter.FILES)
                self.assertEqual(output.stat().st_mode & 0o777, 0o700)
                for name in names:
                    file = output / name
                    self.assertFalse(file.is_symlink())
                    self.assertEqual(file.read_bytes(), name.encode())
                    self.assertEqual(file.stat().st_mode & 0o777, 0o600)

    def test_failure_copy_retains_partial_evidence_without_claiming_completeness(self):
        records = [("probe.log", b"failed before a frame", tarfile.REGTYPE)]
        stream = archive_bytes(records)
        self.assertEqual(exporter.read_evidence(io.BytesIO(stream)), {"probe.log": b"failed before a frame"})
        with self.assertRaises(exporter.EvidenceError):
            exporter.read_evidence(io.BytesIO(stream), True)

    def test_rejects_traversal_links_special_files_and_unexpected_names_before_writes(self):
        cases = [(name, tarfile.REGTYPE) for name in
                 ("../probe.log", "/probe.log", "graphical-evidence/../probe.log",
                  "./graphical-evidence/probe.log", "unknown.log", "probe.log/extra")]
        cases += [("probe.log", kind) for kind in
                  (tarfile.SYMTYPE, tarfile.LNKTYPE, tarfile.DIRTYPE, tarfile.FIFOTYPE,
                   tarfile.CHRTYPE, tarfile.BLKTYPE, tarfile.CONTTYPE)]
        for name, kind in cases:
            with self.subTest(name=name, kind=kind), tempfile.TemporaryDirectory() as temporary:
                output = Path(temporary) / "evidence"
                with self.assertRaises(exporter.EvidenceError):
                    exporter.export_evidence(io.BytesIO(archive_bytes([(name, b"data", kind)])), output)
                self.assertFalse(output.exists())

    def test_duplicate_normalized_names_and_roots_reject(self):
        for records in ([('probe.log', b'a', tarfile.REGTYPE), ('./probe.log', b'b', tarfile.REGTYPE)],
                        [('.', b'', tarfile.DIRTYPE)]):
            with self.subTest(records=records), self.assertRaises(exporter.EvidenceError):
                exporter.read_evidence(io.BytesIO(archive_bytes(records)))

    def test_archive_member_and_total_payload_bounds_reject(self):
        records = [("probe.log", b"12345", tarfile.REGTYPE), ("result.json", b"67890", tarfile.REGTYPE)]
        data = archive_bytes(records)
        for limit, value in (("MAX_ARCHIVE_BYTES", len(data) - 1), ("MAX_FILE_BYTES", 4), ("MAX_TOTAL_BYTES", 9)):
            with self.subTest(limit=limit), patch.object(exporter, limit, value), self.assertRaises(exporter.EvidenceError):
                exporter.read_evidence(io.BytesIO(data))

    def test_missing_and_truncated_tar_reject(self):
        data = archive_bytes([("probe.log", b"data", tarfile.REGTYPE)], root=None)
        for value in (b"", b"invalid tar", data[:514]):
            with self.subTest(size=len(value)), self.assertRaises(exporter.EvidenceError):
                exporter.read_evidence(io.BytesIO(value))

    def test_extended_metadata_headers_and_trailing_data_are_bounded(self):
        data = archive_bytes([("probe.log", b"data", tarfile.REGTYPE)])
        for limit, value in (("MAX_HEADERS", 1), ("MAX_METADATA_BYTES", 1)):
            with self.subTest(limit=limit), patch.object(exporter, limit, value):
                stream = data
                if limit == "MAX_METADATA_BYTES":
                    target = io.BytesIO()
                    with tarfile.open(fileobj=target, mode="w", format=tarfile.PAX_FORMAT) as archive:
                        member = tarfile.TarInfo("probe.log")
                        member.pax_headers = {"mtime": "1.234"}
                        archive.addfile(member)
                    stream = target.getvalue()
                with self.assertRaises(exporter.EvidenceError):
                    exporter.read_evidence(io.BytesIO(stream))
        with self.assertRaises(exporter.EvidenceError):
            exporter.read_evidence(io.BytesIO(data + b"hidden trailing data"))

    def test_occupied_destination_and_substituted_parent_are_preserved(self):
        data = archive_bytes([("probe.log", b"data", tarfile.REGTYPE)])
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            private = root / "private"
            private.mkdir()
            (private / "keep").write_text("preserve")
            alias = root / "alias"
            alias.symlink_to(private, target_is_directory=True)
            occupied = root / "occupied"
            occupied.symlink_to(private, target_is_directory=True)
            for output in (alias / "evidence", occupied):
                with self.subTest(output=output), self.assertRaises(OSError):
                    exporter.export_evidence(io.BytesIO(data), output)
            self.assertEqual(list(private.iterdir()), [private / "keep"])
            self.assertEqual((private / "keep").read_text(), "preserve")


if __name__ == "__main__":
    unittest.main()
