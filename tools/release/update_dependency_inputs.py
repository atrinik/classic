"""Stage verified lock updates and their derived bundle before changing sources."""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import shutil
import tempfile


def load_tool(path: Path):
    specification = importlib.util.spec_from_file_location(path.stem, path)
    assert specification is not None and specification.loader is not None
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


def apply_documents(
    root: Path, documents: dict[Path, object], *, cache: Path,
    trusted_bundle: Path | None = None,
) -> None:
    """Generate and verify every derived document before touching tracked files."""
    bundle = load_tool(root / "tools/release/dependency_bundle.py")
    with tempfile.TemporaryDirectory(prefix="atrinik-dependency-update-") as name:
        staging = Path(name) / "source"
        for relative in (*bundle.LOCK_PATHS, *bundle.ACQUISITION_CONTRACT_PATHS):
            destination = staging / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(root / relative, destination)
        for relative, document in documents.items():
            destination = staging / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
        descriptor = bundle.build_layout(
            staging, cache, Path(name) / "oci", trusted_bundle
        )
        bundle.verify_descriptor(staging, descriptor)
        prepared = dict(documents)
        prepared[Path("dependencies.bundle.json")] = descriptor
        # Archive acquisition and validation above may fail without leaving a
        # partial lock/provenance update. Keep publication in the caller.
        for relative, document in prepared.items():
            destination = root / relative
            with tempfile.NamedTemporaryFile(
                mode="w", encoding="utf-8", dir=destination.parent, delete=False,
            ) as stream:
                temporary = Path(stream.name)
                stream.write(json.dumps(document, indent=2) + "\n")
            try:
                temporary.chmod(destination.stat().st_mode)
                temporary.replace(destination)
            finally:
                temporary.unlink(missing_ok=True)
