"use strict";

const assert = require("node:assert/strict");
const {
  currentBranch,
  nextVersion,
  releaseForCommit,
  rulesForBranch,
} = require("../release/release_policy.cjs");

assert.equal(
  releaseForCommit("main", {type: "docs", breaking: false}),
  "minor",
);
assert.equal(nextVersion("8.0.0", "minor"), "8.1.0");
assert.equal(
  releaseForCommit("8.3.x", {type: "fix", breaking: false}),
  "patch",
);
assert.equal(nextVersion("8.3.0", "patch"), "8.3.1");
assert.equal(
  releaseForCommit("8.3.x", {type: "feat", breaking: false}),
  "minor",
);
assert.throws(() => rulesForBranch("release"), /unsupported/);

delete process.env.ATRINIK_RELEASE_BRANCH;
process.env.GITHUB_REF_NAME = "421/merge";
process.env.GITHUB_BASE_REF = "8.3.x";
assert.equal(currentBranch(), "8.3.x");
delete process.env.GITHUB_REF_NAME;
delete process.env.GITHUB_BASE_REF;

console.log("Classic release policy: mainline and maintenance transitions verified");
