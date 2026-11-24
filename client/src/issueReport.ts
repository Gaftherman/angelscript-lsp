/**
 * @file issueReport.ts
 * Builds the body of a bug report from a server failure without leaking the reporter's machine or
 * code. Nothing here imports `vscode`, so every rule is testable on its own: the extension gathers
 * the raw inputs, and this module decides what may leave the machine.
 */

/** @brief Words that keep their spelling in a shared sample, because renaming them would change the program. */
const KEPT_WORDS: ReadonlySet<string> = new Set([
    'abstract', 'and', 'array', 'any', 'auto', 'bool', 'break', 'case', 'cast', 'catch', 'class', 'const',
    'const_weakref', 'continue', 'default', 'dictionary', 'do', 'double', 'else', 'enum', 'explicit',
    'external', 'false', 'final', 'float', 'for', 'foreach', 'from', 'funcdef', 'function', 'get', 'if',
    'import', 'in', 'inout', 'int', 'int16', 'int32', 'int64', 'int8', 'interface', 'is', 'mixin',
    'namespace', 'not', 'null', 'or', 'out', 'override', 'private', 'property', 'protected', 'return',
    'set', 'shared', 'string', 'super', 'switch', 'this', 'true', 'try', 'typedef', 'uint', 'uint16',
    'uint32', 'uint64', 'uint8', 'void', 'weakref', 'while', 'xor'
]);

/** @brief Lines kept on each side of the cursor in the shared sample. */
export const SNIPPET_RADIUS = 20;

/** @brief Log lines included in a report. */
export const LOG_TAIL_LINES = 60;

/** @brief Longest address a browser fallback may open. Browsers and GitHub reject much more. */
export const MAX_URL_LENGTH = 7000;

/** @brief A path or name to hide, and what to show in its place. */
export interface Redaction {
    readonly value: string;
    readonly placeholder: string;
}

const EMAIL = /[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}/g;

function escapeRegExp(text: string): string {
    return text.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
}

/**
 * @brief Hides the listed paths and names, and any email address, in a block of text.
 *
 * A path is matched with either slash, any case, and in its `file:///` form, because the same folder
 * appears all three ways in a server log. Longer values are replaced first so a workspace inside the
 * home directory shows as the workspace, not as `~/...`.
 *
 * @param text Text that may contain private values.
 * @param redactions Values to hide. Values shorter than three characters are ignored.
 * @return The text with every match replaced.
 */
export function redact(text: string, redactions: readonly Redaction[]): string {
    const ordered = redactions
        .filter(r => r.value.trim().length >= 3)
        .slice()
        .sort((a, b) => b.value.length - a.value.length);
    let result = text;
    for (const { value, placeholder } of ordered) {
        const forward = value.replace(/\\/g, '/');
        const slashes = forward.split('/').filter(part => part.length > 0).map(escapeRegExp);
        if (slashes.length === 0) {
            continue;
        }
        const isPath = value.includes('/') || value.includes('\\');
        if (!isPath) {
            result = result.replace(new RegExp(escapeRegExp(value), 'gi'), placeholder);
            continue;
        }
        const prefix = /^[\\/]/.test(value) ? '[\\\\/]+' : '';
        const plain = slashes.join('[\\\\/]+');
        const encoded = forward.split('/').filter(part => part.length > 0)
            .map(part => escapeRegExp(encodeURIComponent(part))).join('[\\\\/]+');
        for (const body of new Set([plain, encoded])) {
            result = result.replace(new RegExp(`(?:file:\\/\\/\\/?)?${prefix}${body}`, 'gi'), placeholder);
        }
    }
    return result.replace(EMAIL, '<email>');
}

/** @brief Keeps the last lines of a log, dropping blank ones from the end. */
export function tailLines(text: string, count: number): string {
    const lines = text.split(/\r?\n/);
    while (lines.length > 0 && lines[lines.length - 1].trim().length === 0) {
        lines.pop();
    }
    return lines.slice(-count).join('\n');
}

/**
 * @brief Rewrites a piece of AngelScript so its structure survives and its meaning does not.
 *
 * Identifiers become `T1`, `f2`, `v3` (consistently within one call), string contents are emptied and
 * comments are removed. Keywords, operators, numbers and layout stay, which is what a parsing or
 * analysis bug needs.
 *
 * @param source The text to rewrite.
 * @return The rewritten text.
 */
export function anonymizeSource(source: string): string {
    const names = new Map<string, string>();
    const counters = { T: 0, f: 0, v: 0 };
    const nameFor = (word: string, followedByCall: boolean): string => {
        const known = names.get(word);
        if (known !== undefined) {
            return known;
        }
        const kind = followedByCall ? 'f' : /^[A-Z]/.test(word) ? 'T' : 'v';
        counters[kind] += 1;
        const label = `${kind}${counters[kind]}`;
        names.set(word, label);
        return label;
    };

    let out = '';
    let i = 0;
    while (i < source.length) {
        const rest = source.slice(i, i + 3);
        const ch = source[i];
        if (rest === '"""') {
            const end = source.indexOf('"""', i + 3);
            out += '""""""';
            i = end < 0 ? source.length : end + 3;
        } else if (ch === '"' || ch === '\'') {
            let j = i + 1;
            while (j < source.length && source[j] !== ch && source[j] !== '\n') {
                j += source[j] === '\\' ? 2 : 1;
            }
            out += ch + ch;
            i = Math.min(j + 1, source.length);
        } else if (rest.startsWith('//')) {
            const end = source.indexOf('\n', i);
            i = end < 0 ? source.length : end;
        } else if (rest.startsWith('/*')) {
            const end = source.indexOf('*/', i + 2);
            const block = end < 0 ? source.slice(i) : source.slice(i, end + 2);
            out += block.replace(/[^\n]/g, '');
            i += block.length;
        } else if (/[A-Za-z_]/.test(ch)) {
            let j = i + 1;
            while (j < source.length && /[A-Za-z0-9_]/.test(source[j])) {
                j += 1;
            }
            const word = source.slice(i, j);
            if (KEPT_WORDS.has(word)) {
                out += word;
            } else {
                const after = /^\s*\(/.test(source.slice(j, j + 40));
                out += nameFor(word, after);
            }
            i = j;
        } else if (/[0-9]/.test(ch)) {
            let j = i + 1;
            while (j < source.length && /[0-9A-Za-z_.]/.test(source[j])) {
                j += 1;
            }
            out += source.slice(i, j);
            i = j;
        } else {
            out += ch;
            i += 1;
        }
    }
    return out;
}

/**
 * @brief Cuts the lines around a cursor and anonymizes them.
 *
 * @param text Whole document text.
 * @param line Zero-based cursor line.
 * @return The anonymized window, trimmed of leading and trailing blank lines.
 */
export function anonymizeSnippet(text: string, line: number): string {
    const lines = text.split(/\r?\n/);
    const first = Math.max(0, line - SNIPPET_RADIUS);
    const window = lines.slice(first, line + SNIPPET_RADIUS + 1).join('\n');
    return anonymizeSource(window).replace(/^\s*\n/, '').trimEnd();
}

/** @brief Facts and text collected by the extension for one report. */
export interface IssueReportInput {
    readonly extensionVersion: string;
    readonly vscodeVersion: string;
    readonly platform: string;
    readonly summary: string;
    readonly logTail: string;
    readonly snippet?: string;
    readonly redactions: readonly Redaction[];
}

/** @brief Title and Markdown body for an issue. */
export interface IssueReport {
    readonly title: string;
    readonly body: string;
}

/**
 * @brief Assembles the report the user will review before it is sent.
 *
 * @param input Versions, the failure summary, the raw log tail and an optional sample.
 * @return The title and the body, with every private value in `input.redactions` hidden.
 */
export function buildIssueReport(input: IssueReportInput): IssueReport {
    const hide = (text: string): string => redact(text, input.redactions);
    const summary = hide(input.summary).split(/\r?\n/)[0].slice(0, 120);
    const sections = [
        '<!-- Review everything below before sending. Paths and names were replaced and the code sample was rewritten with generic names. Delete anything you do not want to share. -->',
        '',
        '## What happened',
        '',
        hide(input.summary),
        '',
        '## Environment',
        '',
        `- Extension: ${input.extensionVersion}`,
        `- VS Code: ${input.vscodeVersion}`,
        `- OS: ${input.platform}`,
        '',
        '## Log',
        '',
        '```text',
        hide(input.logTail) || '(no log lines were found)',
        '```'
    ];
    if (input.snippet !== undefined && input.snippet.trim().length > 0) {
        sections.push('', '## Anonymized code near the cursor', '', '```angelscript', input.snippet, '```');
    }
    return { title: `[Bug]: ${summary}`, body: sections.join('\n') };
}

/**
 * @brief Builds the GitHub address that pre-fills the bug form, for editors without the VS Code reporter.
 *
 * The start of the body (what happened, versions) is kept and the rest is cut from its beginning until
 * the address fits, because the end of a log is where the failure is.
 *
 * @param newIssueUrl Address of the repository's new-issue page.
 * @param report The report to send.
 * @return An address no longer than `MAX_URL_LENGTH`.
 */
export function buildIssueUrl(newIssueUrl: string, report: IssueReport): string {
    const make = (body: string): string =>
        `${newIssueUrl}?template=bug_report.yml&title=${encodeURIComponent(report.title)}&body=${encodeURIComponent(body)}`;
    const head = report.body.slice(0, 500);
    let tail = report.body.slice(500);
    let url = make(report.body);
    while (url.length > MAX_URL_LENGTH && tail.length > 0) {
        tail = tail.slice(Math.ceil(tail.length * 0.1));
        url = make(`${head}\n\n(truncated)\n${tail}`);
    }
    return url.length > MAX_URL_LENGTH ? make('') : url;
}
