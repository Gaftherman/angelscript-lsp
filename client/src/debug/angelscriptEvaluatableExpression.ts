import * as vscode from 'vscode';

/**
 * @brief Checks if a character is a valid AngelScript identifier character [a-zA-Z0-9_].
 * @param char Single character to test.
 * @return True if valid identifier character.
 */
function isIdentifierChar(char: string): boolean {
    const code = char.charCodeAt(0);
    return (
        (code >= 48 && code <= 57) ||
        (code >= 65 && code <= 90) ||
        (code >= 97 && code <= 122) ||
        code === 95
    );
}

/**
 * @brief Checks if a character can begin an AngelScript identifier [a-zA-Z_].
 * @param char Single character to test.
 * @return True if valid start character.
 */
function isIdentifierStartChar(char: string): boolean {
    const code = char.charCodeAt(0);
    return (
        (code >= 65 && code <= 90) ||
        (code >= 97 && code <= 122) ||
        code === 95
    );
}

/**
 * @brief Expands an expression range backwards from a start index, handling identifiers, member operators, and brackets.
 * @param lineText Source text line.
 * @param startIndex Starting character index.
 * @return Leftmost boundary index of the expression.
 */
function expandExpressionLeft(lineText: string, startIndex: number): number {
    let i = startIndex;
    while (i > 0) {
        const prev = lineText[i - 1];
        if (isIdentifierChar(prev)) {
            i--;
        } else if (prev === '.') {
            i--;
        } else if (prev === ':' && i >= 2 && lineText[i - 2] === ':') {
            i -= 2;
        } else if (prev === ']') {
            // Find matching opening bracket '[' backwards
            let depth = 1;
            let j = i - 2;
            while (j >= 0 && depth > 0) {
                if (lineText[j] === ']') {
                    depth++;
                } else if (lineText[j] === '[') {
                    depth--;
                }
                j--;
            }
            if (depth === 0) {
                i = j + 1;
            } else {
                break;
            }
        } else {
            break;
        }
    }
    return i;
}

/**
 * @brief Expands an expression range forwards from a start index, handling identifiers, member operators, and brackets.
 * @param lineText Source text line.
 * @param startIndex Starting character index.
 * @return Rightmost boundary index (exclusive) of the expression.
 */
function expandExpressionRight(lineText: string, startIndex: number): number {
    let i = startIndex;
    const len = lineText.length;
    while (i < len) {
        const curr = lineText[i];
        if (isIdentifierChar(curr)) {
            i++;
        } else if (curr === '.') {
            i++;
        } else if (curr === ':' && i + 1 < len && lineText[i + 1] === ':') {
            i += 2;
        } else if (curr === '[') {
            // Find matching closing bracket ']' forwards
            let depth = 1;
            let j = i + 1;
            while (j < len && depth > 0) {
                if (lineText[j] === '[') {
                    depth++;
                } else if (lineText[j] === ']') {
                    depth--;
                }
                j++;
            }
            if (depth === 0) {
                i = j;
            } else {
                break;
            }
        } else {
            break;
        }
    }
    return i;
}

/**
 * @brief Finds the anchor of the enclosing indexing expression if the position is within brackets.
 * @param lineText Source text line.
 * @param anchor Character position.
 * @return Adjusted anchor pointing to the container expression if enclosed in indexing brackets.
 */
function findEnclosingIndexAnchor(lineText: string, anchor: number): number {
    let curAnchor = anchor;
    while (curAnchor >= 0) {
        let depth = 0;
        let openPos = -1;
        const scanStart = (curAnchor < lineText.length && lineText[curAnchor] === ']') ? curAnchor - 1 : curAnchor;
        for (let i = scanStart; i >= 0; i--) {
            const c = lineText[i];
            if (c === ']') {
                depth++;
            } else if (c === '[') {
                if (depth === 0) {
                    openPos = i;
                    break;
                }
                depth--;
            } else if (c === ';' || c === '{' || c === '}' || c === '(' || c === ')' || c === ',') {
                break;
            }
        }

        if (openPos > 0) {
            let prev = openPos - 1;
            while (prev >= 0 && (lineText[prev] === ' ' || lineText[prev] === '\t')) {
                prev--;
            }
            if (prev >= 0 && (isIdentifierChar(lineText[prev]) || lineText[prev] === ']')) {
                curAnchor = prev;
                continue;
            }
        }
        break;
    }
    return curAnchor;
}

/**
 * @brief Extracts the full evaluatable AngelScript expression enclosing the given character offset.
 * @param lineText Line text containing the expression.
 * @param charIndex 0-based character index of the cursor/hover.
 * @return Range [start, end] and expression text, or undefined if no valid expression.
 */
export function extractAngelScriptEvaluatableExpression(
    lineText: string,
    charIndex: number
): { range: [number, number]; expression: string } | undefined {
    if (!lineText || charIndex < 0 || charIndex > lineText.length) {
        return undefined;
    }

    let anchor = charIndex;
    if (anchor >= lineText.length || (!isIdentifierChar(lineText[anchor]) && lineText[anchor] !== '[' && lineText[anchor] !== ']')) {
        if (anchor > 0 && isIdentifierChar(lineText[anchor - 1])) {
            anchor = anchor - 1;
        } else {
            return undefined;
        }
    }

    anchor = findEnclosingIndexAnchor(lineText, anchor);

    const start = expandExpressionLeft(lineText, anchor);
    const end = expandExpressionRight(lineText, anchor);

    if (start >= end) {
        return undefined;
    }

    const rawExpr = lineText.substring(start, end).trim();
    if (!rawExpr) {
        return undefined;
    }

    // Must start with an identifier character, not a numeric literal (e.g. 42 or 0x10)
    const firstWord = rawExpr.split(/[^a-zA-Z0-9_]/).find(part => part.length > 0);
    if (!firstWord || !isIdentifierStartChar(firstWord[0])) {
        return undefined;
    }

    return {
        range: [start, end],
        expression: rawExpr
    };
}

/**
 * @brief Provides expanded evaluatable expressions for hover and watch during debugging.
 */
export class AngelScriptEvaluatableExpressionProvider implements vscode.EvaluatableExpressionProvider {
    provideEvaluatableExpression(
        document: vscode.TextDocument,
        position: vscode.Position,
        _token: vscode.CancellationToken
    ): vscode.ProviderResult<vscode.EvaluatableExpression> {
        const line = document.lineAt(position.line);
        const result = extractAngelScriptEvaluatableExpression(line.text, position.character);
        if (!result) {
            return undefined;
        }

        const range = new vscode.Range(
            position.line,
            result.range[0],
            position.line,
            result.range[1]
        );
        return new vscode.EvaluatableExpression(range, result.expression);
    }
}
