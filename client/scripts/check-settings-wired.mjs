// Every setting this extension declares must actually reach the server.
//
// The extension passes configuration as command-line arguments, so a setting is only live if
// buildServerArgs() explicitly emits a flag for it. Nothing enforced that, and three settings drifted
// out of the wiring while staying in package.json, documented and inert:
// `features.codeLens`, `features.onTypeFormatting` and `engine.profile` - the last of which is how a
// user selects the Sven Co-op / Urho3D / OpenXRay dialect.
//
// Declaring a setting the server never hears is worse than not having it: the UI promises a control
// that does nothing. This runs as part of `npm run pretest`, which CI already invokes.

import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const clientDir = join(dirname(fileURLToPath(import.meta.url)), '..');

const manifest = JSON.parse(readFileSync(join(clientDir, 'package.json'), 'utf8'));
const source = readFileSync(join(clientDir, 'src', 'extension.ts'), 'utf8');

const declared = Object.keys(manifest.contributes?.configuration?.properties ?? {});
const problems = [];

for (const key of declared) {
    const name = key.replace(/^angelscript\./, '');

    // `features.X` is wired through the FEATURE_FLAGS table, keyed by the bare X.
    if (name.startsWith('features.')) {
        const feature = name.slice('features.'.length);
        if (!new RegExp(`['"]${feature}['"]\\s*,`).test(source)) {
            problems.push(`${key} has no entry in FEATURE_FLAGS`);
        }
        continue;
    }

    // `engine.X` booleans go through ENGINE_PROPERTIES; `engine.profile` is an enum with its own line.
    if (name.startsWith('engine.')) {
        const property = name.slice('engine.'.length);
        if (!source.includes(`'${property}'`) && !source.includes(`engine.${property}`)) {
            problems.push(`${key} is never read by buildServerArgs`);
        }
        continue;
    }

    // `preprocessor.X` booleans go through PREPROCESSOR_FEATURES the same way; pragmaMode is an
    // enum with its own line and falls through to the general case below.
    if (name.startsWith('preprocessor.')) {
        const feature = name.slice('preprocessor.'.length);
        if (!source.includes(`'${feature}'`) && !source.includes(`preprocessor.${feature}`)) {
            problems.push(`${key} is never read by buildServerArgs`);
        }
        continue;
    }

    // Everything else is read by its own `config.get(...)` call.
    if (!source.includes(`'${name}'`)) {
        problems.push(`${key} is never read by buildServerArgs`);
    }
}

if (problems.length > 0) {
    console.error('Settings declared in package.json but not passed to the server:');
    for (const problem of problems) {
        console.error(`  - ${problem}`);
    }
    console.error('\nAdd the flag in client/src/extension.ts (buildServerArgs / FEATURE_FLAGS), or');
    console.error('remove the setting from package.json. A setting the server never hears is a');
    console.error('control that silently does nothing.');
    process.exit(1);
}

console.log(`All ${declared.length} declared settings reach the server.`);
