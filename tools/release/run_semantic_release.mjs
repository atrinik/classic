#!/usr/bin/env node

import { spawnSync } from "node:child_process";

import { pinnedReleaseEnvironment } from "./semantic_release_environment.mjs";

const environment = pinnedReleaseEnvironment();
const result = spawnSync(
  process.execPath,
  [environment.semanticRelease, ...process.argv.slice(2)],
  { env: process.env, stdio: "inherit" },
);

if (result.error) {
  throw result.error;
}
process.exit(result.status === null ? 1 : result.status);
