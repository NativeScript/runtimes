"use strict";

const fs = require("fs");
const path = require("path");
const { spawnSync } = require("child_process");

const testDir = __dirname;
const testFiles = fs.readdirSync(testDir)
  .filter((name) => name.endsWith(".test.js"))
  .sort();

for (const testFile of testFiles) {
  const result = spawnSync(process.execPath, [path.join(testDir, testFile)], {
    stdio: "inherit",
  });
  if (result.error) {
    throw result.error;
  }
  if (result.status !== 0) {
    process.exit(result.status ?? 1);
  }
}
