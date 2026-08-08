#!/usr/bin/env node

import { execFileSync } from "node:child_process";
import { mkdirSync, writeFileSync } from "node:fs";
import { createRequire } from "node:module";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { parseArgs } from "node:util";

import {
  importPinnedReleasePackage,
  pinnedReleaseEnvironment,
} from "./semantic_release_environment.mjs";

const require = createRequire(import.meta.url);
const { firstParentHashes, firstParentWriterOptions, resolveCommit } = require(
  "./first_parent_release.cjs",
);
const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
const TAG = /^v(\d+)\.(\d+)\.(\d+)$/;
const FULL_HASH = /^[0-9a-f]{40}$/;

function fail(message) {
  throw new Error(`release-note generation failed: ${message}`);
}

function version(tag) {
  const match = TAG.exec(tag);
  if (!match) {
    fail(`${tag} is not an unprefixed vMAJOR.MINOR.PATCH tag`);
  }
  return match.slice(1).map(Number);
}

function compareVersions(left, right) {
  for (let index = 0; index < 3; index += 1) {
    if (left[index] !== right[index]) {
      return left[index] - right[index];
    }
  }
  return 0;
}

function gitLog(from, to) {
  const output = execFileSync(
    "git",
    [
      "--no-replace-objects",
      "log",
      "-z",
      "--format=%H%x00%B%x00%D%x00%cI",
      `${from}..${to}`,
    ],
    {
      cwd: ROOT,
      encoding: "utf8",
      maxBuffer: 64 * 1024 * 1024,
      stdio: ["ignore", "pipe", "pipe"],
    },
  );
  const fields = output.split("\0");
  if (fields.at(-1) === "") {
    fields.pop();
  }
  if (fields.length % 4 !== 0) {
    fail("Git returned a malformed commit stream");
  }
  const commits = [];
  for (let index = 0; index < fields.length; index += 4) {
    const [hash, message, gitTags, committerDate] = fields.slice(index, index + 4);
    if (!FULL_HASH.test(hash)) {
      fail("Git returned a malformed commit ID");
    }
    commits.push({
      hash,
      message: message.trim(),
      gitTags: gitTags.trim(),
      committerDate: new Date(committerDate),
    });
  }
  return commits;
}

const { values } = parseArgs({
  options: {
    output: { type: "string" },
    "previous-tag": { type: "string" },
    "repository-url": { type: "string" },
    tag: { type: "string" },
  },
  strict: true,
});
for (const name of ["output", "previous-tag", "repository-url", "tag"]) {
  if (!values[name]) {
    fail(`--${name} is required`);
  }
}

const currentTag = values.tag;
const previousTag = values["previous-tag"];
const currentVersion = version(currentTag);
const previousVersion = version(previousTag);
if (
  currentVersion[0] !== previousVersion[0] ||
  compareVersions(previousVersion, currentVersion) >= 0
) {
  fail("the previous tag must be an earlier release on the same major line");
}

const currentCommit = resolveCommit(currentTag, ROOT);
const previousCommit = resolveCommit(previousTag, ROOT);
const allowed = firstParentHashes(currentCommit, ROOT);
if (!firstParentHashes("HEAD", ROOT).has(currentCommit)) {
  fail("the recovery tag is not on HEAD's first-parent line");
}
const commits = gitLog(previousCommit, currentCommit);
if (commits.length === 0) {
  fail("the release range contains no commits");
}

const environment = pinnedReleaseEnvironment();
const { generateNotes } = await importPinnedReleasePackage(
  "@semantic-release/release-notes-generator",
  environment,
);
const notes = await generateNotes(
  {
    preset: "conventionalcommits",
    writerOpts: firstParentWriterOptions(allowed),
  },
  {
    commits,
    cwd: ROOT,
    lastRelease: {
      gitHead: previousCommit,
      gitTag: previousTag,
      version: previousTag.slice(1),
    },
    logger: { log() {} },
    nextRelease: {
      gitHead: currentCommit,
      gitTag: currentTag,
      version: currentTag.slice(1),
    },
    options: { repositoryUrl: values["repository-url"] },
  },
);
if (!notes.trim()) {
  fail("the official generator returned empty notes");
}

const output = resolve(values.output);
mkdirSync(dirname(output), { recursive: true });
writeFileSync(output, notes, { encoding: "utf8", flag: "wx" });
console.log(`wrote first-parent release notes to ${output}`);
