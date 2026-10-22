const assert = require("assert");
const fs = require("fs");
const path = require("path");

const repoRoot = path.resolve(__dirname, "../../..");
const installSource = fs.readFileSync(
  path.join(repoRoot, "NativeScript/ffi/objc/shared/bridge/Install.mm"),
  "utf8",
);
const index = fs.readFileSync(path.join(__dirname, "../src/index.ts"), "utf8");

const match = /function isEnumMemberGlobalName\(enumName, memberName\) \{[\s\S]*?\n  \}\n/.exec(
  installSource,
);
assert(match, "Install.mm should define isEnumMemberGlobalName");
const isEnumMemberGlobalName = new Function(
  `${match[0]}; return isEnumMemberGlobalName;`,
)();

const cases = [
  ["NSTextAlignment", "Center", false],
  ["NSTextAlignment", "NSTextAlignmentCenter", true],
  ["NSTextAlignment", "kNSTextAlignmentCenter", true],
  ["NSTextAlignment", "1", false],
  ["CKRecordZoneCapabilities", "Zone", false],
  ["NSComparisonResult", "OrderedAscending", false],
  ["NSComparisonResult", "NSOrderedAscending", true],
  ["CLAuthorizationStatus", "kCLAuthorizationStatusRestricted", true],
  ["NSControlStateValue", "NSControlStateValueOn", true],
  ["UIViewAutoresizing", "FlexibleWidth", false],
  ["UIViewAutoresizing", "UIViewAutoresizingFlexibleWidth", true],
  ["NSURLErrorCode", "NSURLErrorCancelled", true],
  ["MTLPixelFormat", "Fill", false],
];
for (const [enumName, memberName, expected] of cases) {
  assert.strictEqual(
    isEnumMemberGlobalName(enumName, memberName),
    expected,
    `${enumName}.${memberName} as a global`,
  );
}

const tsMatch = /function isEnumMemberGlobalName\(enumName: string, memberName: string\): boolean \{[\s\S]*?\n\}\n/.exec(
  index,
);
assert(tsMatch, "index.ts should define isEnumMemberGlobalName");
const isEnumMemberGlobalNameTs = new Function(
  `${tsMatch[0].replace("(enumName: string, memberName: string): boolean", "(enumName, memberName)")}; return isEnumMemberGlobalName;`,
)();
for (const [enumName, memberName, expected] of cases) {
  assert.strictEqual(
    isEnumMemberGlobalNameTs(enumName, memberName),
    expected,
    `${enumName}.${memberName} as a global (index.ts)`,
  );
}
assert(
  /function installGlobals\(\)[\s\S]*?isEnumMemberGlobalName\(name, memberName\)/.test(index),
  "installGlobals should filter enum members through isEnumMemberGlobalName",
);

console.log("enum member global tests passed");
