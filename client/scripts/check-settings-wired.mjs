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

import { readFileSync, readdirSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const clientDir = join(dirname(fileURLToPath(import.meta.url)), '..');

const manifest = JSON.parse(readFileSync(join(clientDir, 'package.json'), 'utf8'));
const extensionSource = readFileSync(join(clientDir, 'src', 'extension.ts'), 'utf8');

const declared = Object.keys(manifest.contributes?.configuration?.properties ?? {});
const problems = [];

for (const key of declared) {
    const name = key.replace(/^angelscript\./, '');

    // `features.X` is wired through the FEATURE_FLAGS table, keyed by the bare X.
    if (name.startsWith('features.')) {
        const feature = name.slice('features.'.length);
        if (!new RegExp(`['"]${feature}['"]\\s*,`).test(extensionSource)) {
            problems.push(`${key} has no entry in FEATURE_FLAGS`);
        }
        continue;
    }

    // `engine.X` booleans go through ENGINE_PROPERTIES; `engine.profile` is an enum with its own line.
    if (name.startsWith('engine.')) {
        const property = name.slice('engine.'.length);
        if (!extensionSource.includes(`'${property}'`) && !extensionSource.includes(`engine.${property}`)) {
            problems.push(`${key} is never read by buildServerArgs`);
        }
        continue;
    }

    // `preprocessor.X` booleans go through PREPROCESSOR_FEATURES the same way; pragmaMode is an
    // enum with its own line and falls through to the general case below.
    if (name.startsWith('preprocessor.')) {
        const feature = name.slice('preprocessor.'.length);
        if (!extensionSource.includes(`'${feature}'`) && !extensionSource.includes(`preprocessor.${feature}`)) {
            problems.push(`${key} is never read by buildServerArgs`);
        }
        continue;
    }

    // Everything else is read by its own `config.get(...)` call.
    if (!extensionSource.includes(`'${name}'`)) {
        problems.push(`${key} is never read by buildServerArgs`);
    }
}

// Every `%key%` in the manifest has to exist in package.nls.json, and every translation has to
// carry the same key set. VS Code does not complain about a placeholder it cannot resolve: it shows
// the raw `%config.features.hover.description%` to the user, in the settings UI, forever. A missing
// key in a translation is quieter still - that entry silently falls back to English, so half a
// settings page ends up bilingual with nothing to say why.
const nlsProblems = [];
const placeholders = new Set();

for (const match of JSON.stringify(manifest).matchAll(/"%([^%"]+)%"/g)) {
    placeholders.add(match[1]);
}

const base = JSON.parse(readFileSync(join(clientDir, 'package.nls.json'), 'utf8'));

for (const key of placeholders) {
    if (!(key in base)) {
        nlsProblems.push(`package.json uses %${key}% but package.nls.json does not define it`);
    }
}

for (const key of Object.keys(base)) {
    if (!placeholders.has(key)) {
        nlsProblems.push(`package.nls.json defines ${key} but nothing in package.json uses it`);
    }
}

for (const entry of readdirSync(clientDir)) {
    const locale = /^package\.nls\.([\w-]+)\.json$/.exec(entry);
    if (locale === null) {
        continue;
    }

    const translated = JSON.parse(readFileSync(join(clientDir, entry), 'utf8'));
    for (const key of Object.keys(base)) {
        if (!(key in translated)) {
            nlsProblems.push(`${entry} is missing ${key}`);
        }
    }
    for (const key of Object.keys(translated)) {
        if (!(key in base)) {
            nlsProblems.push(`${entry} defines ${key}, which package.nls.json does not`);
        }
    }
}

// The same rule for strings the extension shows at runtime. VS Code keys those on the English
// source string itself, so a translation whose key does not match the source character for
// character silently shows English - and nothing anywhere says which of the two drifted.
const runtimeStrings = new Set();
for (const match of extensionSource.matchAll(/l10n\.t\(\s*'((?:[^'\\]|\\.)*)'/g)) {
    runtimeStrings.add(match[1].replace(/\\'/g, "'"));
}

if (runtimeStrings.size > 0) {
    const l10nDir = join(clientDir, 'l10n');
    for (const entry of readdirSync(l10nDir)) {
        if (!entry.startsWith('bundle.l10n.') || !entry.endsWith('.json')) {
            continue;
        }

        const bundle = JSON.parse(readFileSync(join(l10nDir, entry), 'utf8'));
        for (const text of runtimeStrings) {
            if (!(text in bundle)) {
                nlsProblems.push(`l10n/${entry} has no entry for "${text}"`);
            }
        }
        for (const key of Object.keys(bundle)) {
            if (!runtimeStrings.has(key)) {
                nlsProblems.push(`l10n/${entry} translates "${key}", which no l10n.t() call passes`);
            }
        }
    }
}

if (nlsProblems.length > 0) {
    console.error('Localisation keys are out of step:');
    for (const problem of nlsProblems) {
        console.error(`  - ${problem}`);
    }
    console.error('\nA placeholder with no key is shown to the user verbatim; a key missing from a');
    console.error('translation falls back to English without saying so.');
    process.exit(1);
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

console.log(`All ${declared.length} declared settings reach the server, and `
    + `${placeholders.size} manifest and ${runtimeStrings.size} runtime strings line up `
    + `across every translation.`);
