"""Every string the extension shows must exist in both languages, and English must be the base.

The fifth guard, and it exists for the same reason as the other four: a control that enforces
nothing passes in green. VS Code silently falls back to the string literal in the source when a key
is missing from a bundle, so an untranslated message looks exactly like a translated one to anyone
running in English - which is everyone writing the code. Nothing else in this repository notices.

Four questions:

  1. Does every `l10n.t('...')` literal in the extension have an entry in bundle.l10n.json?
     Missing here means the English bundle is not actually the source of truth for that string.
  2. Does it have one in bundle.l10n.es.json?
     Missing here means a Spanish user sees English.
  3. Does every `%key%` the manifest references exist in package.nls.json AND package.nls.es.json?
     A `%key%` with no entry renders as the literal `%key%` in the command palette.
  4. Do the two bundles of each pair hold the same key set?
     An entry in Spanish with none in English is a string that was translated and then renamed.

English being the *base* is what makes it the default: VS Code reads package.nls.json and
bundle.l10n.json for any locale it has no file for, so a key present in English and missing in
Spanish degrades to English, and the reverse degrades to nothing.
"""
import io
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CLIENT = os.path.normpath(os.path.join(HERE, '..', '..', 'client'))

NLS_EN = os.path.join(CLIENT, 'package.nls.json')
NLS_ES = os.path.join(CLIENT, 'package.nls.es.json')
L10N_EN = os.path.join(CLIENT, 'l10n', 'bundle.l10n.json')
L10N_ES = os.path.join(CLIENT, 'l10n', 'bundle.l10n.es.json')
MANIFEST = os.path.join(CLIENT, 'package.json')
SOURCE_DIR = os.path.join(CLIENT, 'src')

# A single-quoted TypeScript string, with escaped quotes allowed inside it.
CALL = re.compile(r"l10n\.t\(\s*'((?:[^'\\]|\\.)*)'")
PLACEHOLDER = re.compile(r'"%([^%"]+)%"')


def load(path):
    with io.open(path, encoding='utf-8') as handle:
        return json.load(handle)


def unescape(literal):
    """Turns a TypeScript literal back into the string the bundle is keyed by."""
    return literal.replace("\\'", "'").replace('\\"', '"').replace('\\\\', '\\')


def main():
    problems = []

    nls_en, nls_es = load(NLS_EN), load(NLS_ES)
    l10n_en, l10n_es = load(L10N_EN), load(L10N_ES)

    used = set()
    for root, _dirs, files in os.walk(SOURCE_DIR):
        for name in files:
            if not name.endswith('.ts'):
                continue
            with io.open(os.path.join(root, name), encoding='utf-8') as handle:
                for literal in CALL.findall(handle.read()):
                    used.add(unescape(literal))

    for message in sorted(used):
        if message not in l10n_en:
            problems.append('l10n.t("%s") has no entry in bundle.l10n.json' % message)
        if message not in l10n_es:
            problems.append('l10n.t("%s") has no entry in bundle.l10n.es.json' % message)

    with io.open(MANIFEST, encoding='utf-8') as handle:
        referenced = set(PLACEHOLDER.findall(handle.read()))

    for key in sorted(referenced):
        if key not in nls_en:
            problems.append('package.json uses %%%s%% with no entry in package.nls.json' % key)
        if key not in nls_es:
            problems.append('package.json uses %%%s%% with no entry in package.nls.es.json' % key)

    for label, english, spanish in (('package.nls', nls_en, nls_es),
                                    ('bundle.l10n', l10n_en, l10n_es)):
        for key in sorted(set(spanish) - set(english)):
            problems.append('%s.es.json translates "%s", which %s.json does not declare'
                            % (label, key, label))

    if problems:
        print('Client localisation is incomplete:')
        for problem in problems:
            print('  - %s' % problem)
        return 1

    print('%d l10n.t strings and %d manifest keys, all present in English and Spanish.'
          % (len(used), len(referenced)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
