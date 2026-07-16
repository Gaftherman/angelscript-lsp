#include "tree_sitter/parser.h"
#include <stdbool.h>
#include <stdlib.h>

enum TokenType {
    TEMPLATE_OPEN,   // '<' as template opener
    TEMPLATE_CLOSE,  // '>' as template closer (consumes exactly one '>')
    ERROR_SENTINEL,  // never produced; valid only during error recovery
};

typedef struct {
    uint32_t template_depth;
} Scanner;

static bool is_alpha(int32_t c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool is_alnum(int32_t c) {
    return is_alpha(c) || (c >= '0' && c <= '9');
}

static bool is_eol(int32_t c) {
    return c == '\n' || c == '\r';
}

static void skip_ws(TSLexer *lexer) {
    while (lexer->lookahead == ' ' || lexer->lookahead == '\t' ||
           lexer->lookahead == '\n' || lexer->lookahead == '\r') {
        lexer->advance(lexer, true);
    }
}

// Skip spaces and tabs only, staying on the current line.
static void skip_spaces(TSLexer *lexer) {
    while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
        lexer->advance(lexer, true);
    }
}

// After consuming '<', scan forward (without consuming) to determine if this
// looks like a template argument list. We look for a balanced closing '>' where
// the content between is plausible as type arguments.
//
// Returns true if it looks like a template, false otherwise.
//
// This is a heuristic scan. We track '<'/'>' nesting depth and verify that
// the content consists of identifiers, primitive keywords, commas, colons,
// brackets, '@', 'const', '?', and nested templates - all things that appear
// in AngelScript type expressions. If we encounter something that can't appear
// in a type (like arithmetic operators, numbers, parentheses for calls, or
// semicolons), it's not a template.
//
// The scan is bounded to the current line. Reaching end-of-line or
// end-of-file before the closing '>' - having seen only plausible template
// content — most likely means the user is still typing the argument list, so
// we optimistically treat it as a template. The parser then builds a partial
// template_type_list (implicitly closed at end-of-line, see scan() below)
// instead of misparsing '<' as a comparison operator that can stitch this
// line together with the next one.
static bool scan_template_content(TSLexer *lexer) {
    int depth = 1;
    // Limit lookahead to avoid pathological cases
    int limit = 256;

    while (depth > 0 && limit > 0) {
        limit--;

        if (lexer->eof(lexer)) {
            // Incomplete code at end of file - optimistically a template
            return true;
        }

        int32_t c = lexer->lookahead;

        if (is_eol(c)) {
            // Incomplete line - optimistically a template. Argument lists
            // that legitimately continue after a ',' also end up here and
            // parse fine; only a closing '>' placed on its own line would
            // be misjudged (a style unused in real code).
            return true;
        }

        if (c == '<') {
            depth++;
            lexer->advance(lexer, false);
            continue;
        }

        if (c == '>') {
            depth--;
            if (depth == 0) {
                // Found the matching '>' — this is a template
                return true;
            }
            lexer->advance(lexer, false);
            continue;
        }

        // Valid inside template type arguments
        if (is_alnum(c) || c == '_') {
            // Skip identifier/keyword
            while (is_alnum(lexer->lookahead)) {
                lexer->advance(lexer, false);
            }
            continue;
        }

        if (c == ' ' || c == '\t') {
            lexer->advance(lexer, false);
            continue;
        }

        // ',' separates template arguments: Dict<string, int>
        // ':' for scope resolution: NS::Type
        // '[' ']' for array types: int[]
        // '@' for handle types: Type@
        // '?' for auto type
        if (c == ',' || c == ':' || c == '[' || c == ']' || c == '@' || c == '?') {
            lexer->advance(lexer, false);
            continue;
        }

        // Anything else (digits, operators, parens, semicolons, etc.)
        // means this is NOT a template argument list
        return false;
    }

    return false;
}

void *tree_sitter_angelscript_external_scanner_create(void) {
    Scanner *scanner = calloc(1, sizeof(Scanner));
    return scanner;
}

void tree_sitter_angelscript_external_scanner_destroy(void *payload) {
    free(payload);
}

unsigned tree_sitter_angelscript_external_scanner_serialize(
    void *payload, char *buffer
) {
    Scanner *scanner = (Scanner *)payload;
    buffer[0] = (char)(scanner->template_depth & 0xFF);
    buffer[1] = (char)((scanner->template_depth >> 8) & 0xFF);
    return 2;
}

void tree_sitter_angelscript_external_scanner_deserialize(
    void *payload, const char *buffer, unsigned length
) {
    Scanner *scanner = (Scanner *)payload;
    if (length >= 2) {
        scanner->template_depth =
            (unsigned char)buffer[0] |
            ((unsigned char)buffer[1] << 8);
    } else {
        scanner->template_depth = 0;
    }
}

bool tree_sitter_angelscript_external_scanner_scan(
    void *payload, TSLexer *lexer, const bool *valid_symbols
) {
    Scanner *scanner = (Scanner *)payload;

    // Error recovery mode: tree-sitter marks every symbol (including the
    // never-referenced sentinel) as valid. Only perform the one unambiguous
    // action — closing a tracked template on a real '>' - so that stray
    // '<'/'>' inside ERROR regions can't corrupt template_depth.
    if (valid_symbols[ERROR_SENTINEL]) {
        if (scanner->template_depth > 0) {
            skip_ws(lexer);
            if (lexer->lookahead == '>') {
                lexer->advance(lexer, false);
                // '>=' is a comparison operator, not a template close
                if (lexer->lookahead == '=') {
                    return false;
                }
                lexer->result_symbol = TEMPLATE_CLOSE;
                scanner->template_depth--;
                return true;
            }
        }
        return false;
    }

    // Implicit (zero-width) template close at end-of-line / end-of-file.
    // If the parser is inside a template argument list that could close here
    // but the line ends first, the code is incomplete (still being typed):
    // emit a zero-width TEMPLATE_CLOSE - the same pattern as Python scanners
    // emitting DEDENT at EOF - so the template_type_list is finished and the
    // rest of the block parses normally. Bounded: each emission decrements
    // template_depth, which only real '<' opens increment.
    // Tradeoff: a closing '>' on its own line (`TArray<int\n>`) no longer
    // parses; that style does not occur in real code.
    if (valid_symbols[TEMPLATE_CLOSE] && scanner->template_depth > 0) {
        skip_spaces(lexer);
        if (lexer->eof(lexer) || is_eol(lexer->lookahead)) {
            lexer->mark_end(lexer);
            lexer->result_symbol = TEMPLATE_CLOSE;
            scanner->template_depth--;
            return true;
        }
    }

    skip_ws(lexer);

    // Template open: '<' when parser expects it could be a template
    if (valid_symbols[TEMPLATE_OPEN] && lexer->lookahead == '<') {
        lexer->advance(lexer, false);

        // Reject '<<' and '<=' — these are shift/comparison operators
        if (lexer->lookahead == '<' || lexer->lookahead == '=') {
            return false;
        }

        // Mark end at '<' — further reads are lookahead only
        lexer->mark_end(lexer);

        // Scan forward to check if the content between < and > looks like
        // template type arguments. This is the key heuristic that prevents
        // `a < b` from being parsed as a template open.
        if (!scan_template_content(lexer)) {
            return false;
        }

        lexer->result_symbol = TEMPLATE_OPEN;
        scanner->template_depth++;
        return true;
    }

    // Template close: '>' when inside a template context
    if (valid_symbols[TEMPLATE_CLOSE] && lexer->lookahead == '>') {
        lexer->advance(lexer, false);

        // Reject '>=' — this is a comparison operator, not a template close
        if (lexer->lookahead == '=') {
            return false;
        }

        // Consume exactly ONE '>', even if next char is also '>'
        // This handles '>>' being split as two template closes
        lexer->result_symbol = TEMPLATE_CLOSE;
        if (scanner->template_depth > 0) {
            scanner->template_depth--;
        }
        return true;
    }

    return false;
}
