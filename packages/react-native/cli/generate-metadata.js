#!/usr/bin/env node

const fs = require('fs');
const path = require('path');

function readConfig(projectRoot) {
  const configPath = path.join(projectRoot, 'nativescript.react-native.json');
  if (!fs.existsSync(configPath)) {
    return {};
  }
  return JSON.parse(fs.readFileSync(configPath, 'utf8'));
}

function generateMetadata(argv = process.argv.slice(2)) {
  const root = process.cwd();
  const config = readConfig(root);
  const metadata = config.reactNative?.metadata || {};
  const includePods = metadata.includePods || [];
  const includeSystemFrameworks = metadata.includeSystemFrameworks || [];

  console.log('NativeScript React Native metadata configuration:');
  console.log(`  project: ${root}`);
  console.log(`  pods: ${includePods.length ? includePods.join(', ') : '(none)'}`);
  console.log(
    `  system frameworks: ${
      includeSystemFrameworks.length
        ? includeSystemFrameworks.join(', ')
        : 'UIKit, Foundation'
    }`,
  );

  if (argv.includes('--check')) {
    return;
  }

  console.log(
    'Metadata is bundled with @nativescript/react-native for the default iOS SDK. ' +
      'Custom pod/framework metadata generation will use this configuration during native builds.',
  );
}

if (require.main === module) {
  generateMetadata();
}

module.exports = {
  generateMetadata,
};
