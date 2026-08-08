#!/usr/bin/env node

import { constants, accessSync, existsSync, readFileSync, realpathSync } from "node:fs";
import { createRequire } from "node:module";
import { delimiter, dirname, join, parse } from "node:path";
import { pathToFileURL } from "node:url";

export const PINNED_RELEASE_PACKAGES = Object.freeze({
  "semantic-release": "25.0.9",
  "@semantic-release/commit-analyzer": "13.0.1",
  "@semantic-release/release-notes-generator": "14.1.1",
  "@semantic-release/exec": "7.1.0",
  "conventional-changelog-conventionalcommits": "9.3.1",
});

function findExecutable(name) {
  for (const directory of (process.env.PATH || "").split(delimiter)) {
    if (!directory) {
      continue;
    }
    const candidate = join(directory, name);
    try {
      accessSync(candidate, constants.X_OK);
      return realpathSync(candidate);
    } catch {
      // Continue through PATH.
    }
  }
  throw new Error(`${name} is not available in the pinned npx environment`);
}

function packageManifest(entry, expectedName) {
  let directory = dirname(entry);
  const root = parse(directory).root;
  while (directory !== root) {
    const manifestPath = join(directory, "package.json");
    if (existsSync(manifestPath)) {
      const manifest = JSON.parse(readFileSync(manifestPath, "utf8"));
      if (manifest.name === expectedName) {
        return { manifest, manifestPath };
      }
    }
    directory = dirname(directory);
  }
  throw new Error(`cannot locate the ${expectedName} package manifest`);
}

export function pinnedReleaseEnvironment() {
  const semanticRelease = findExecutable("semantic-release");
  const requireFromRelease = createRequire(pathToFileURL(semanticRelease));
  const packages = new Map();

  for (const [name, expectedVersion] of Object.entries(PINNED_RELEASE_PACKAGES)) {
    const entry = requireFromRelease.resolve(name);
    const { manifest, manifestPath } = packageManifest(entry, name);
    if (manifest.version !== expectedVersion) {
      throw new Error(
        `${name} resolved to ${manifest.version}; expected ${expectedVersion}`,
      );
    }
    packages.set(name, { entry, manifestPath, version: manifest.version });
  }

  return { packages, semanticRelease };
}

export async function importPinnedReleasePackage(name, environment) {
  const selected = environment.packages.get(name);
  if (!selected) {
    throw new Error(`${name} is not part of the pinned release environment`);
  }
  return import(pathToFileURL(selected.entry).href);
}
