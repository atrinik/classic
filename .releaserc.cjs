"use strict";

const {
  firstParentHashes,
  firstParentReleaseRules,
  firstParentWriterOptions,
} = require("./tools/release/first_parent_release.cjs");
const {
  currentBranch,
  rulesForBranch,
} = require("./tools/release/release_policy.cjs");

const firstParent = firstParentHashes("HEAD", __dirname);
const releaseBranch = currentBranch();

module.exports = {
  branches: ["+([0-9]).+([0-9]).x", "main"],
  tagFormat: "v${version}",
  plugins: [
    [
      "@semantic-release/commit-analyzer",
      {
        preset: "conventionalcommits",
        releaseRules: firstParentReleaseRules(
          firstParent,
          rulesForBranch(releaseBranch),
        ),
      },
    ],
    [
      "@semantic-release/release-notes-generator",
      {
        preset: "conventionalcommits",
        writerOpts: firstParentWriterOptions(firstParent),
      },
    ],
    [
      "@semantic-release/github",
      {
        draftRelease: true,
        failCommentCondition: false,
        successCommentCondition: false,
      },
    ],
    [
      "@semantic-release/exec",
      {
        verifyReleaseCmd:
          "python3 tools/release/verify_next_version.py ${nextRelease.version}",
        publishCmd:
          "tools/release/queue_package_release.sh ${nextRelease.version}",
      },
    ],
  ],
};
