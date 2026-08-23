import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { createRequire } from "node:module";
import { dirname, resolve } from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";

import {
  importPinnedReleasePackage,
  pinnedReleaseEnvironment,
} from "../release/semantic_release_environment.mjs";

const require = createRequire(import.meta.url);
const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
process.env.ATRINIK_RELEASE_BRANCH = "main";
const config = require("../../.releaserc.cjs");
const {
  firstParentHashes,
  firstParentReleaseRules,
  firstParentWriterOptions,
} = require("../release/first_parent_release.cjs");
const environment = pinnedReleaseEnvironment();
const { analyzeCommits } = await importPinnedReleasePackage(
  "@semantic-release/commit-analyzer",
  environment,
);
const { generateNotes } = await importPinnedReleasePackage(
  "@semantic-release/release-notes-generator",
  environment,
);
const logger = { log() {} };
const RELEASE_BASELINE_TAG = "v5.34.4";
const RELEASE_BASELINE_VERSION = "5.34.4";
const NEXT_RELEASE_TAG = "v5.47.0";
const NEXT_RELEASE_VERSION = "5.47.0";

function git(...arguments_) {
  return execFileSync("git", ["--no-replace-objects", ...arguments_], {
    cwd: ROOT,
    encoding: "utf8",
    maxBuffer: 64 * 1024 * 1024,
  }).trim();
}

function commits(from, to) {
  const output = execFileSync(
    "git",
    [
      "--no-replace-objects",
      "log",
      "-z",
      "--format=%H%x00%B%x00%D%x00%cI",
      `${from}..${to}`,
    ],
    { cwd: ROOT, encoding: "utf8", maxBuffer: 64 * 1024 * 1024 },
  );
  const fields = output.split("\0");
  if (fields.at(-1) === "") {
    fields.pop();
  }
  assert.equal(fields.length % 4, 0);
  const result = [];
  for (let index = 0; index < fields.length; index += 4) {
    result.push({
      hash: fields[index],
      message: fields[index + 1].trim(),
      gitTags: fields[index + 2].trim(),
      committerDate: new Date(fields[index + 3]),
    });
  }
  return result;
}

function notesContext(selectedCommits, gitHead = "a".repeat(40)) {
  return {
    commits: selectedCommits,
    cwd: ROOT,
    lastRelease: {
      gitHead: git(`rev-parse`, `${RELEASE_BASELINE_TAG}^{commit}`),
      gitTag: RELEASE_BASELINE_TAG,
      version: RELEASE_BASELINE_VERSION,
    },
    logger,
    nextRelease: {
      gitHead,
      gitTag: NEXT_RELEASE_TAG,
      version: NEXT_RELEASE_VERSION,
    },
    options: { repositoryUrl: "https://github.com/atrinik/classic.git" },
  };
}

test("side-parent commits cannot affect release type or notes", async () => {
  const allowedHash = "a".repeat(40);
  const sideHash = "b".repeat(40);
  const allowed = new Set([allowedHash]);
  const releaseRules = firstParentReleaseRules(allowed, [
    { breaking: true, release: "minor" },
    { type: "feat", release: "minor" },
    { type: "*", release: "patch" },
  ]);
  const selectedCommits = [
    { hash: allowedHash, message: "fix(core): keep first parent (#1)" },
    {
      hash: sideHash,
      message:
        "feat(import)!: exclude side history (#999)\n\nBREAKING CHANGE: imported",
    },
  ];

  assert.equal(
    await analyzeCommits(
      { preset: "conventionalcommits", releaseRules },
      { commits: selectedCommits, cwd: ROOT, logger },
    ),
    "patch",
  );
  const automatic = await generateNotes(
    {
      preset: "conventionalcommits",
      writerOpts: firstParentWriterOptions(allowed),
    },
    notesContext(selectedCommits),
  );
  const recovery = await generateNotes(
    { preset: "conventionalcommits" },
    notesContext([selectedCommits[0]]),
  );
  assert.equal(automatic, recovery);
  assert.match(automatic, /keep first parent/);
  assert.doesNotMatch(automatic, /side history|#999|BREAKING CHANGES/);
});

test("current Classic analysis is exactly first-parent analysis", async () => {
  const analyzer = config.plugins.find(
    ([name]) => name === "@semantic-release/commit-analyzer",
  )[1];
  const generator = config.plugins.find(
    ([name]) => name === "@semantic-release/release-notes-generator",
  )[1];
  const allCommits = commits(
    git("rev-parse", `${RELEASE_BASELINE_TAG}^{commit}`),
    "HEAD",
  );
  const allowed = firstParentHashes("HEAD", ROOT);
  const selectedCommits = allCommits.filter(({ hash }) => allowed.has(hash));
  assert.equal(
    await analyzeCommits(analyzer, { commits: allCommits, cwd: ROOT, logger }),
    "minor",
  );
  const automatic = await generateNotes(
    generator,
    notesContext(allCommits, git("rev-parse", "HEAD")),
  );
  const recovery = await generateNotes(
    { preset: "conventionalcommits" },
    notesContext(selectedCommits, git("rev-parse", "HEAD")),
  );
  assert.equal(automatic, recovery);
  assert.doesNotMatch(automatic, /commit\/013fdfd/);
  for (const [, shortHash] of automatic.matchAll(/\/commit\/([0-9a-f]{7})/g)) {
    assert.ok([...allowed].some((hash) => hash.startsWith(shortHash)));
  }
});

test("missing revisions fail closed", () => {
  assert.throws(
    () => firstParentHashes("--not-a-release-ref", ROOT),
    /git rev-parse|release revision/,
  );
});
