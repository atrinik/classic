#!/usr/bin/env python3
"""Build and validate the bounded static Classic performance report."""

from __future__ import annotations

import argparse
import hashlib
import html
import importlib.util
import json
from pathlib import Path, PurePosixPath
import shutil
import sys
from typing import Any
from urllib.error import HTTPError, URLError
from urllib.parse import urljoin, urlparse
from urllib.request import Request, urlopen


ROOT = Path(__file__).resolve().parents[2]
REPORT_SPEC = importlib.util.spec_from_file_location(
    "daily_performance_report_contract", ROOT / "tools" / "ci" / "daily_performance_report.py"
)
if REPORT_SPEC is None or REPORT_SPEC.loader is None:
    raise RuntimeError("cannot load the daily performance report contract")
report = importlib.util.module_from_spec(REPORT_SPEC)
REPORT_SPEC.loader.exec_module(report)

SITE_SCHEMA_VERSION = 1
RETENTION_VERSION = 1
REPOSITORY = "atrinik/classic"
WORKFLOW = "daily-client-performance.yml"
MAX_COHORTS = 8
MAX_FILES = 1_500
MAX_BYTES = 512 * 1024 * 1024
MANIFEST_PATH = "v1/manifest.json"
STATE_PATH = "v1/state.json"
TREND_PATHS = ("trend.json", "v1/trend.json")
ALERTS_PATH = "v1/alerts.json"
ALLOWED_ENVIRONMENT_KEYS = {
    "artifact_url",
    "repository",
    "runner_image",
    "workflow_url",
}


class SiteError(ValueError):
    """Raised when a site or checkpoint does not satisfy the closed contract."""


def _json_bytes(value: Any) -> bytes:
    return (json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n").encode()


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _read_json(path: Path, context: str) -> Any:
    try:
        return json.loads(path.read_text())
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise SiteError(f"cannot read {context}: {error}") from error


def _safe_relative_path(value: str) -> PurePosixPath:
    path = PurePosixPath(value)
    if (
        not value
        or path.is_absolute()
        or value != path.as_posix()
        or any(part in ("", ".", "..") for part in path.parts)
    ):
        raise SiteError(f"unsafe static path: {value!r}")
    return path


def _write(root: Path, relative: str, data: bytes) -> None:
    destination = root.joinpath(*_safe_relative_path(relative).parts)
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(data)


def _manifest_digest(root: Path) -> str:
    path = root / MANIFEST_PATH
    if not path.is_file():
        raise SiteError("checkpoint manifest is missing")
    return _sha256(path.read_bytes())


def _validate_url(value: Any, context: str, *, expected_host: str | None = None) -> str:
    if not isinstance(value, str):
        raise SiteError(f"{context} must be a URL")
    parsed = urlparse(value)
    if parsed.scheme != "https" or not parsed.netloc or parsed.username or parsed.password:
        raise SiteError(f"{context} must be an HTTPS URL without credentials")
    if expected_host is not None and parsed.netloc != expected_host:
        raise SiteError(f"{context} has an unexpected host")
    return value


def _validate_environment(environment: Any) -> dict[str, Any]:
    if not isinstance(environment, dict) or set(environment) != ALLOWED_ENVIRONMENT_KEYS:
        raise SiteError("environment must contain the complete allowed identity fields")
    if environment.get("repository") != REPOSITORY:
        raise SiteError("environment repository is not atrinik/classic")
    _validate_url(environment.get("workflow_url"), "workflow URL", expected_host="github.com")
    _validate_url(environment.get("artifact_url"), "artifact URL", expected_host="github.com")
    runner = environment.get("runner_image")
    if not isinstance(runner, str) or not runner or len(runner) > 128:
        raise SiteError("runner image identity is invalid")
    return environment


def _retained_points(trend: dict[str, Any]) -> list[dict[str, Any]]:
    cohorts = trend.get("cohorts")
    if not isinstance(cohorts, dict):
        raise SiteError("trend cohorts are malformed")
    points: list[dict[str, Any]] = []
    for cohort, values in cohorts.items():
        if not isinstance(cohort, str) or not isinstance(values, list):
            raise SiteError("trend cohort is malformed")
        for point in values:
            if (
                not isinstance(point, dict)
                or point.get("cohort") != cohort
                or not str(point.get("run_id", "")).isdigit()
                or not str(point.get("run_attempt", "")).isdigit()
                or point.get("id") != f"run-{point.get('run_id')}"
                or point.get("point_path") != f"points/run-{point.get('run_id')}.json"
                or point.get("report_path")
                != f"reports/run-{point.get('run_id')}/index.html"
            ):
                raise SiteError("trend point cohort is malformed")
            points.append(point)
    return sorted(points, key=lambda item: int(item["run_id"]))


def _prune_global_history(
    trend: dict[str, Any], current_cohort: str, current_run_id: str
) -> None:
    cohorts = trend["cohorts"]
    ranked = sorted(
        cohorts,
        key=lambda cohort: max(
            (int(point["run_id"]) for point in cohorts[cohort]), default=0
        ),
        reverse=True,
    )
    retained = set(ranked[:MAX_COHORTS]) | {current_cohort}
    removed = [cohort for cohort in cohorts if cohort not in retained]
    dropped_run_ids = [
        int(point["run_id"])
        for cohort in removed
        for point in cohorts[cohort]
    ]
    if dropped_run_ids:
        trend["global_retention_watermark"] = max(
            trend.get("global_retention_watermark", 0), *dropped_run_ids
        )
    for cohort in removed:
        del cohorts[cohort]
        trend.get("retention_watermarks", {}).pop(cohort, None)
    for key in list(trend.get("alerts", {})):
        state = trend["alerts"][key]
        if state.get("retired_at_run") not in (None, current_run_id):
            del trend["alerts"][key]
        elif key.split(":", 1)[0] in removed:
            if state.get("active") is True:
                state.update(
                    active=False,
                    last_transition="recovered",
                    retired_at_run=current_run_id,
                )
            else:
                del trend["alerts"][key]


def _metric(value: Any) -> str:
    return f"{float(value):.2f}"


def _page(title: str, body: str) -> bytes:
    document = f"""<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>{html.escape(title)}</title>
  <style>
    :root {{ color-scheme: light dark; font-family: system-ui, sans-serif; }}
    body {{ margin: auto; max-width: 76rem; padding: 1rem; line-height: 1.5; }}
    table {{ border-collapse: collapse; width: 100%; margin-block: 1rem; }}
    th, td {{ border: 1px solid #8888; padding: .4rem; text-align: left; }}
    th {{ background: #8882; }}
    code {{ overflow-wrap: anywhere; }}
    .status {{ font-weight: 700; }}
    nav a {{ margin-right: 1rem; }}
  </style>
</head>
<body>
<header><h1>{html.escape(title)}</h1><nav aria-label="Report data"><a href="trend.json">Trend JSON</a><a href="v1/state.json">State v1</a><a href="v1/manifest.json">Manifest v1</a></nav></header>
<main>{body}</main>
</body>
</html>
"""
    return document.encode()


def _phase_table(point: dict[str, Any], key: str = "phases") -> str:
    rows = []
    for name, phase in point[key].items():
        work = phase["work_ms"]
        window = phase["window_p95_ms"]
        rows.append(
            "<tr>"
            f"<th scope=\"row\">{html.escape(name)}</th>"
            f"<td>{_metric(work['p50'])}</td><td>{_metric(work['p95'])}</td>"
            f"<td>{_metric(work['p99'])}</td>"
            f"<td>{_metric(window['first'])} → {_metric(window['last'])}</td>"
            "</tr>"
        )
    return (
        "<table><caption>Frame work in milliseconds</caption>"
        "<thead><tr><th scope=\"col\">Phase</th><th scope=\"col\">p50</th>"
        "<th scope=\"col\">p95</th><th scope=\"col\">p99</th>"
        "<th scope=\"col\">First → last p95</th></tr></thead>"
        f"<tbody>{''.join(rows)}</tbody></table>"
    )


def render_report(point: dict[str, Any]) -> bytes:
    environment = _validate_environment(point.get("environment"))
    contexts = point.get("contexts", {})
    context_rows = []
    for name in sorted(contexts):
        context = contexts[name]
        work = context["work_ms"]
        lighting = context["lighting_work_ms"]
        context_rows.append(
            "<tr>"
            f"<th scope=\"row\">{html.escape(name)}</th>"
            f"<td>{_metric(work['p50'])}</td><td>{_metric(work['p95'])}</td>"
            f"<td>{_metric(work['p99'])}</td><td>{_metric(lighting['p95'])}</td>"
            "</tr>"
        )
    checks = point.get("checks", {})
    check_rows = []
    for name in sorted(checks):
        value = checks[name]
        state = "pass" if isinstance(value, dict) and value.get("passed") is True else "fail"
        check_rows.append(
            f"<tr><th scope=\"row\">{html.escape(name)}</th><td>{state}</td></tr>"
        )
    resource_rows = []
    for name, value in sorted(point.get("resources", {}).items()):
        resource_rows.append(
            f"<tr><th scope=\"row\">{html.escape(name)}</th>"
            f"<td>{html.escape(str(value))}</td></tr>"
        )
    body = (
        f"<p class=\"status\">Status: {html.escape(str(point['status']))}</p>"
        f"<p>Source <code>{html.escape(point['commit'])}</code>; run "
        f"<a href=\"{html.escape(environment['workflow_url'], quote=True)}\">"
        f"{html.escape(point['run_id'])}, attempt {html.escape(point['run_attempt'])}</a>; "
        f"cohort <code>{html.escape(point['cohort'])}</code>.</p>"
        "<h2>Standard viewport</h2>"
        + _phase_table(point)
        + ("<h2>Large viewport</h2>" + _phase_table(point, "large_phases")
           if point.get("large_phases") else "")
        + "<h2>Movement contexts</h2><table><caption>Candidate contexts in milliseconds</caption>"
        "<thead><tr><th scope=\"col\">Context</th><th scope=\"col\">p50</th>"
        "<th scope=\"col\">p95</th><th scope=\"col\">p99</th>"
        "<th scope=\"col\">Lighting p95</th></tr></thead>"
        f"<tbody>{''.join(context_rows)}</tbody></table>"
        "<h2>Correctness and resource checks</h2><table><caption>Enforced and informational checks</caption>"
        f"<tbody>{''.join(check_rows)}</tbody></table>"
        "<h2>Resource state</h2><table><caption>Process and cache resources</caption>"
        f"<tbody>{''.join(resource_rows)}</tbody></table>"
        f"<p><a href=\"../../points/{html.escape(point['id'])}.json\">Detailed point JSON</a> · "
        f"<a href=\"{html.escape(environment['artifact_url'], quote=True)}\">Raw run artifact</a></p>"
    )
    return _page(f"Classic client performance — run {point['run_id']}", body)


def render_index(trend: dict[str, Any], point: dict[str, Any]) -> bytes:
    rows = []
    for item in reversed(_retained_points(trend)):
        sustained = item["phases"]["sustained"]
        rows.append(
            "<tr>"
            f"<th scope=\"row\"><a href=\"{html.escape(item['report_path'], quote=True)}\">"
            f"{html.escape(item['run_id'])}.{html.escape(item['run_attempt'])}</a></th>"
            f"<td><code>{html.escape(item['commit'][:12])}</code></td>"
            f"<td><code>{html.escape(item['cohort'])}</code></td>"
            f"<td>{html.escape(item['status'])}</td>"
            f"<td>{_metric(sustained['work_ms']['p50'])}</td>"
            f"<td>{_metric(sustained['work_ms']['p95'])}</td>"
            f"<td>{_metric(sustained['work_ms']['p99'])}</td>"
            f"<td>{_metric(sustained['window_p95_ms']['first'])} → "
            f"{_metric(sustained['window_p95_ms']['last'])}</td>"
            "</tr>"
        )
    body = (
        f"<p class=\"status\">Latest status: {html.escape(point['status'])}</p>"
        f"<p>Exact source <code>{html.escape(point['commit'])}</code>; run "
        f"{html.escape(point['run_id'])}, attempt {html.escape(point['run_attempt'])}; "
        f"environment/schema cohort <code>{html.escape(point['cohort'])}</code>.</p>"
        "<p>This no-JavaScript report retains bounded compatible history. Detailed points "
        "and reports are separate from the compact trend and digest-bound checkpoint.</p>"
        "<table><caption>Retained observations, newest first</caption><thead><tr>"
        "<th scope=\"col\">Run.attempt</th><th scope=\"col\">Source</th>"
        "<th scope=\"col\">Cohort</th><th scope=\"col\">Status</th>"
        "<th scope=\"col\">p50</th><th scope=\"col\">p95</th>"
        "<th scope=\"col\">p99</th><th scope=\"col\">First → last p95</th>"
        f"</tr></thead><tbody>{''.join(rows)}</tbody></table>"
    )
    return _page("Classic client performance", body)


def _manifest_files(root: Path) -> dict[str, dict[str, Any]]:
    files: dict[str, dict[str, Any]] = {}
    for path in sorted(root.rglob("*")):
        if path.is_symlink() or not path.is_file():
            if path.is_symlink():
                raise SiteError("static tree must not contain symbolic links")
            continue
        relative = path.relative_to(root).as_posix()
        if relative == MANIFEST_PATH:
            continue
        data = path.read_bytes()
        files[relative] = {"sha256": _sha256(data), "size": len(data)}
    return files


def validate_site(root: Path, *, expected_repository: str = REPOSITORY) -> dict[str, Any]:
    manifest = _read_json(root / MANIFEST_PATH, "site manifest")
    if not isinstance(manifest, dict) or manifest.get("schema_version") != SITE_SCHEMA_VERSION:
        raise SiteError("unsupported site manifest schema")
    if manifest.get("repository") != expected_repository or manifest.get("workflow") != WORKFLOW:
        raise SiteError("site manifest producer identity is invalid")
    files = manifest.get("files")
    if not isinstance(files, dict) or files != _manifest_files(root):
        raise SiteError("site manifest file inventory or digests do not match")
    actual_files = len(files) + 1
    actual_bytes = sum(item["size"] for item in files.values()) + (root / MANIFEST_PATH).stat().st_size
    bounds = manifest.get("bounds")
    expected_bounds = {
        "max_bytes": MAX_BYTES,
        "max_cohorts": MAX_COHORTS,
        "max_files": MAX_FILES,
        "max_points_per_cohort": report.TREND_RETENTION,
    }
    if bounds != expected_bounds or actual_files > MAX_FILES or actual_bytes > MAX_BYTES:
        raise SiteError("site bounds are invalid or exceeded")
    state = _read_json(root / STATE_PATH, "site state")
    if not isinstance(state, dict) or state.get("schema_version") != SITE_SCHEMA_VERSION:
        raise SiteError("unsupported site state schema")
    if state.get("repository") != REPOSITORY or state.get("workflow") != WORKFLOW:
        raise SiteError("site state producer identity is invalid")
    published = {
        "cohort": manifest.get("cohort"),
        "recorded_at": manifest.get("recorded_at"),
        "run_attempt": manifest.get("run_attempt"),
        "run_id": manifest.get("run_id"),
        "source_sha": manifest.get("source_sha"),
    }
    generation = manifest.get("generation")
    predecessor = manifest.get("predecessor")
    valid_predecessor = (
        type(generation) is int
        and generation > 0
        and (
            (
                generation == 1
                and isinstance(predecessor, dict)
                and set(predecessor) == {"legacy_final_ref"}
                and isinstance(predecessor["legacy_final_ref"], str)
                and len(predecessor["legacy_final_ref"]) == 40
            )
            or (
                generation > 1
                and isinstance(predecessor, dict)
                and set(predecessor)
                == {"digest", "generation", "run_attempt", "run_id"}
                and predecessor["generation"] == generation - 1
                and isinstance(predecessor["digest"], str)
                and len(predecessor["digest"]) == 64
                and str(predecessor["run_id"]).isdigit()
                and str(predecessor["run_attempt"]).isdigit()
            )
        )
    )
    if (
        state.get("retention_version") != RETENTION_VERSION
        or manifest.get("retention_version") != RETENTION_VERSION
        or state.get("generation") != manifest.get("generation")
        or state.get("predecessor") != manifest.get("predecessor")
        or state.get("published") != published
        or not isinstance(manifest.get("source_sha"), str)
        or len(manifest["source_sha"]) != 40
        or any(character not in "0123456789abcdef" for character in manifest["source_sha"])
        or not str(manifest.get("run_id", "")).isdigit()
        or not str(manifest.get("run_attempt", "")).isdigit()
        or not isinstance(manifest.get("recorded_at"), str)
        or not valid_predecessor
    ):
        raise SiteError("site state and manifest identity do not match")
    trend = state.get("trend")
    points = _retained_points(trend)
    if len(trend["cohorts"]) > MAX_COHORTS:
        raise SiteError("site retains too many cohorts")
    expected_paths = {
        path
        for point in points
        for path in (point["point_path"], point["report_path"])
    }
    if not expected_paths.issubset(files):
        raise SiteError("site is missing a retained point or report")
    for compact in points:
        detailed = _read_json(root / compact["point_path"], "retained detailed point")
        if report.compact_point(detailed) != compact:
            raise SiteError("retained detailed point does not match compact state")
    if _read_json(root / TREND_PATHS[0], "public trend") != trend:
        raise SiteError("public trend does not match durable state")
    if (root / TREND_PATHS[0]).read_bytes() != (root / TREND_PATHS[1]).read_bytes():
        raise SiteError("versioned and stable trend endpoints differ")
    alerts = _read_json(root / ALERTS_PATH, "desired alerts")
    if alerts != {
        "schema_version": SITE_SCHEMA_VERSION,
        "observation": {
            "run_id": manifest["run_id"],
            "run_attempt": manifest["run_attempt"],
        },
        "alerts": trend.get("alerts", {}),
    }:
        raise SiteError("desired alerts do not match durable state")
    for required in ("<!doctype html>", "<html lang=\"en\">", "<main>", "<table"):
        if required not in (root / "index.html").read_text():
            raise SiteError(f"site index is missing accessibility structure: {required}")
    for path in root.rglob("*"):
        if path.is_file():
            data = path.read_bytes()
            if b"<script" in data.lower() or b"github_pat_" in data or b"ghp_" in data:
                raise SiteError(f"forbidden active content or credential pattern in {path}")
    return manifest


def load_checkpoint(root: Path) -> tuple[dict[str, Any], dict[str, Any], str]:
    manifest = validate_site(root)
    state = _read_json(root / STATE_PATH, "checkpoint state")
    return manifest, state, _manifest_digest(root)


def _legacy_checkpoint(root: Path, final_ref: str) -> tuple[dict[str, Any], dict[str, Any]]:
    if len(final_ref) != 40 or any(character not in "0123456789abcdef" for character in final_ref):
        raise SiteError("legacy final ref must be a lowercase full commit SHA")
    legacy = _read_json(root / "trend.json", "legacy trend")
    if not isinstance(legacy, dict) or not isinstance(legacy.get("cohorts"), dict):
        raise SiteError("legacy trend is malformed")
    trend = None
    details: dict[str, Any] = {}
    for old_point in sorted(
        (point for values in legacy["cohorts"].values() for point in values),
        key=lambda item: int(item["run_id"]),
    ):
        point_path = root / "points" / f"{old_point['run_id']}.json"
        point = _read_json(point_path, "legacy detailed point") if point_path.is_file() else old_point
        point.setdefault("run_attempt", "1")
        trend = report.merge_trend(trend, point)
        details[point["id"]] = point
    if trend is None:
        raise SiteError("legacy checkpoint contains no observations")
    state = {
        "schema_version": SITE_SCHEMA_VERSION,
        "retention_version": RETENTION_VERSION,
        "repository": REPOSITORY,
        "workflow": WORKFLOW,
        "generation": 0,
        "published": {"legacy_final_ref": final_ref},
        "predecessor": None,
        "trend": trend,
    }
    return state, details


def build_site(
    *, evidence_path: Path, output: Path, commit: str, run_id: str,
    run_attempt: str, recorded_at: str, environment: dict[str, Any],
    checkpoint: Path | None = None, legacy_checkpoint: Path | None = None,
    legacy_final_ref: str | None = None, summary: Path | None = None,
) -> dict[str, Any]:
    _validate_environment(environment)
    if (checkpoint is None) == (legacy_checkpoint is None):
        raise SiteError("select exactly one validated Pages or legacy checkpoint")
    details: dict[str, Any] = {}
    predecessor_digest = None
    predecessor: dict[str, Any] | None = None
    prior_root = checkpoint
    if checkpoint is not None:
        previous_manifest, previous_state, predecessor_digest = load_checkpoint(checkpoint)
        trend = previous_state["trend"]
        generation = int(previous_state["generation"]) + 1
        predecessor = {
            "digest": predecessor_digest,
            "generation": previous_state["generation"],
            "run_attempt": previous_manifest["run_attempt"],
            "run_id": previous_manifest["run_id"],
        }
    else:
        assert legacy_checkpoint is not None and legacy_final_ref is not None
        previous_state, details = _legacy_checkpoint(legacy_checkpoint, legacy_final_ref)
        trend = previous_state["trend"]
        generation = 1
        predecessor = {"legacy_final_ref": legacy_final_ref}
        prior_root = None
    point = report.build_point(
        _read_json(evidence_path, "movement evidence"), commit=commit, run_id=run_id,
        run_attempt=run_attempt, recorded_at=recorded_at, environment=environment,
    )
    trend = report.merge_trend(trend, point)
    _prune_global_history(trend, point["cohort"], run_id)
    retained = _retained_points(trend)
    retained_ids = {item["id"] for item in retained}
    if output.exists() and any(output.iterdir()):
        raise SiteError("output directory must be absent or empty")
    output.mkdir(parents=True, exist_ok=True)
    for item in retained:
        if item["id"] == point["id"]:
            detail = point
            point_data = _json_bytes(point)
            report_data = render_report(point)
        elif item["id"] in details:
            detail = details[item["id"]]
            point_data = _json_bytes(detail)
            report_data = render_report(detail)
        else:
            assert prior_root is not None
            point_source = prior_root.joinpath(*_safe_relative_path(item["point_path"]).parts)
            report_source = prior_root.joinpath(*_safe_relative_path(item["report_path"]).parts)
            if not point_source.is_file() or not report_source.is_file():
                raise SiteError("validated checkpoint lacks retained detailed history")
            point_data = point_source.read_bytes()
            report_data = report_source.read_bytes()
        _write(output, item["point_path"], point_data)
        _write(output, item["report_path"], report_data)
    if point["id"] not in retained_ids:
        raise SiteError("current point was pruned from its own deployment")
    published = {
        "cohort": point["cohort"],
        "recorded_at": recorded_at,
        "run_attempt": run_attempt,
        "run_id": run_id,
        "source_sha": commit,
    }
    state = {
        "schema_version": SITE_SCHEMA_VERSION,
        "retention_version": RETENTION_VERSION,
        "repository": REPOSITORY,
        "workflow": WORKFLOW,
        "generation": generation,
        "published": published,
        "predecessor": predecessor,
        "trend": trend,
    }
    public_trend = trend
    alerts = {
        "schema_version": SITE_SCHEMA_VERSION,
        "observation": {"run_id": run_id, "run_attempt": run_attempt},
        "alerts": trend.get("alerts", {}),
    }
    _write(output, "index.html", render_index(public_trend, point))
    _write(output, STATE_PATH, _json_bytes(state))
    _write(output, ALERTS_PATH, _json_bytes(alerts))
    for path in TREND_PATHS:
        _write(output, path, _json_bytes(public_trend))
    files = _manifest_files(output)
    manifest = {
        "schema_version": SITE_SCHEMA_VERSION,
        "retention_version": RETENTION_VERSION,
        "repository": REPOSITORY,
        "workflow": WORKFLOW,
        "generation": generation,
        "run_id": run_id,
        "run_attempt": run_attempt,
        "recorded_at": recorded_at,
        "source_sha": commit,
        "cohort": point["cohort"],
        "predecessor": predecessor,
        "bounds": {
            "max_bytes": MAX_BYTES,
            "max_cohorts": MAX_COHORTS,
            "max_files": MAX_FILES,
            "max_points_per_cohort": report.TREND_RETENTION,
        },
        "files": files,
    }
    _write(output, MANIFEST_PATH, _json_bytes(manifest))
    validate_site(output)
    if summary is not None:
        summary.parent.mkdir(parents=True, exist_ok=True)
        summary.write_text(report.render_summary(point, trend))
    return manifest


def fetch_checkpoint(base_url: str, output: Path) -> dict[str, Any]:
    base = _validate_url(base_url, "Pages base URL", expected_host="atrinik.github.io")
    if not base.endswith("/"):
        base += "/"
    if output.exists() and any(output.iterdir()):
        raise SiteError("checkpoint output directory must be absent or empty")
    output.mkdir(parents=True, exist_ok=True)

    def fetch(relative: str, maximum: int) -> bytes:
        url = urljoin(base, relative)
        if not url.startswith(base):
            raise SiteError("checkpoint URL escaped the Pages base")
        try:
            with urlopen(Request(url, headers={"User-Agent": "atrinik-classic-checkpoint/1"}), timeout=30) as response:
                if response.geturl() != url or response.status != 200:
                    raise SiteError(f"checkpoint request did not return the exact URL: {relative}")
                data = response.read(maximum + 1)
        except (HTTPError, URLError, TimeoutError) as error:
            raise SiteError(f"cannot fetch checkpoint {relative}: {error}") from error
        if len(data) > maximum:
            raise SiteError(f"checkpoint response exceeds its declared bound: {relative}")
        return data

    manifest_data = fetch(MANIFEST_PATH, 4 * 1024 * 1024)
    _write(output, MANIFEST_PATH, manifest_data)
    try:
        manifest = json.loads(manifest_data)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise SiteError("fetched checkpoint manifest is not JSON") from error
    files = manifest.get("files") if isinstance(manifest, dict) else None
    if not isinstance(files, dict) or len(files) + 1 > MAX_FILES:
        raise SiteError("fetched checkpoint manifest inventory is invalid")
    declared_bytes = 0
    for relative, metadata in files.items():
        _safe_relative_path(relative)
        if not isinstance(metadata, dict) or set(metadata) != {"sha256", "size"}:
            raise SiteError("fetched checkpoint file metadata is invalid")
        size = metadata["size"]
        if type(size) is not int or size < 0:
            raise SiteError("fetched checkpoint file size is invalid")
        declared_bytes += size
    if declared_bytes > MAX_BYTES:
        raise SiteError("fetched checkpoint declares too many bytes")
    for relative in sorted(files):
        data = fetch(relative, files[relative]["size"])
        if len(data) != files[relative]["size"] or _sha256(data) != files[relative]["sha256"]:
            raise SiteError(f"fetched checkpoint digest or size mismatch: {relative}")
        _write(output, relative, data)
    return validate_site(output)


def main() -> int:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)
    build = subparsers.add_parser("build")
    build.add_argument("--evidence", type=Path, required=True)
    build.add_argument("--output", type=Path, required=True)
    build.add_argument("--commit", required=True)
    build.add_argument("--run-id", required=True)
    build.add_argument("--run-attempt", required=True)
    build.add_argument("--recorded-at", required=True)
    build.add_argument("--environment", type=json.loads, required=True)
    build.add_argument("--summary", type=Path)
    source = build.add_mutually_exclusive_group(required=True)
    source.add_argument("--checkpoint", type=Path)
    source.add_argument("--legacy-checkpoint", type=Path)
    build.add_argument("--legacy-final-ref")
    validate = subparsers.add_parser("validate")
    validate.add_argument("--site", type=Path, required=True)
    fetch = subparsers.add_parser("fetch")
    fetch.add_argument("--base-url", required=True)
    fetch.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        if args.command == "build":
            if (args.legacy_checkpoint is None) != (args.legacy_final_ref is None):
                raise SiteError("legacy checkpoint and final ref must be supplied together")
            build_site(
                evidence_path=args.evidence, output=args.output, commit=args.commit,
                run_id=args.run_id, run_attempt=args.run_attempt,
                recorded_at=args.recorded_at, environment=args.environment,
                checkpoint=args.checkpoint, legacy_checkpoint=args.legacy_checkpoint,
                legacy_final_ref=args.legacy_final_ref, summary=args.summary,
            )
        elif args.command == "validate":
            validate_site(args.site)
        else:
            fetch_checkpoint(args.base_url, args.output)
    except (OSError, report.ReportError, SiteError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
