const addon = require(__dirname + "/addon.dylib");
const fs = require("fs");

function assert(condition, message) {
  if (!condition) {
    throw new Error(message);
  }
}

const markerFile = process.env.NS_NODE_API_CLEANUP_FILE;
assert(
  typeof markerFile === "string" && markerFile.length > 0,
  "NS_NODE_API_CLEANUP_FILE is required"
);

try {
  fs.unlinkSync(markerFile);
} catch (_) {}

addon.registerTeardownHooks(markerFile);
console.log(`registered teardown hooks: ${markerFile}`);
