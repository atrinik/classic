"use strict";

const { execFileSync } = require("node:child_process");

const FULL_HASH = /^[0-9a-f]{40}$/;

function git(cwd, arguments_) {
  try {
    return execFileSync("git", ["--no-replace-objects", ...arguments_], {
      cwd,
      encoding: "utf8",
      maxBuffer: 64 * 1024 * 1024,
      stdio: ["ignore", "pipe", "pipe"],
    }).trim();
  } catch (error) {
    const detail =
      error && error.stderr ? String(error.stderr).trim() : String(error);
    throw new Error(`git ${arguments_.join(" ")}: ${detail}`);
  }
}

function requireFullHash(value, label) {
  if (!FULL_HASH.test(value)) {
    throw new Error(`${label} is not a full lowercase Git commit ID`);
  }
  return value;
}

function resolveCommit(revision, cwd) {
  if (typeof revision !== "string" || revision.length === 0) {
    throw new Error("release revision is empty");
  }
  return requireFullHash(
    git(cwd, ["rev-parse", "--verify", "--end-of-options", `${revision}^{commit}`]),
    "release revision",
  );
}

function requireCompleteHistory(cwd) {
  if (git(cwd, ["rev-parse", "--is-shallow-repository"]) !== "false") {
    throw new Error("first-parent release analysis requires complete Git history");
  }
}

function firstParentHashes(revision, cwd) {
  requireCompleteHistory(cwd);
  const commit = resolveCommit(revision, cwd);
  const output = git(cwd, ["rev-list", "--first-parent", commit]);
  if (!output) {
    throw new Error("first-parent release history is empty");
  }
  const hashes = output.split("\n");
  for (const hash of hashes) {
    requireFullHash(hash, "first-parent history entry");
  }
  return new Set(hashes);
}

function firstParentReleaseRules(hashes, releaseRules) {
  if (!(hashes instanceof Set) || hashes.size === 0) {
    throw new Error("first-parent release rules require at least one commit");
  }
  if (!Array.isArray(releaseRules) || releaseRules.length === 0) {
    throw new Error("first-parent release rules require a base policy");
  }
  return [
    { release: false },
    ...[...hashes].flatMap((hash) =>
      releaseRules.map((rule) => ({ hash, ...rule })),
    ),
  ];
}

function firstParentWriterOptions(hashes) {
  if (!(hashes instanceof Set) || hashes.size === 0) {
    throw new Error("first-parent release notes require at least one commit");
  }
  return {
    skip(commit) {
      const hash = commit && commit.raw ? commit.raw.hash : commit && commit.hash;
      return typeof hash !== "string" || !hashes.has(hash);
    },
  };
}

module.exports = {
  firstParentHashes,
  firstParentReleaseRules,
  firstParentWriterOptions,
  requireFullHash,
  resolveCommit,
};
