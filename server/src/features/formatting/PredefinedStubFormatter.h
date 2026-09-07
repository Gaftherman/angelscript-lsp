#pragma once

#include <string>

#include "parser/AngelScriptParser.h"

namespace angel_lsp::features::formatting
{
    /**
     * @brief Rewrites a predefined stub so each namespace is declared once, as a block.
     *
     * A stub generated from an engine's registration table writes one declaration per registered
     * entity, so a namespace with twenty members arrives as twenty namespaces:
     *
     *     //Empty string. Useful when a reference to a string is needed.;
     *     namespace String { const string EMPTY_STRING; }
     *     //Default comparison type.;
     *     namespace String { const CompareType DEFAULT_COMPARE; }
     *
     * That is legal and the analyzer reads it, but nobody can read it. This gathers them:
     *
     *     namespace String
     *     {
     *         //Empty string. Useful when a reference to a string is needed.;
     *         const string EMPTY_STRING;
     *         //Default comparison type.;
     *         const CompareType DEFAULT_COMPARE;
     *     }
     *
     * Every namespace comes out in that block form, whether or not it had to be merged, because a
     * file where some are blocks and others are one-liners is not formatted. Nothing else in the
     * file is touched at all: a class, an enum, a funcdef and every global keep their bytes, so a
     * formatter the user points at a hand-written stub cannot rewrite the parts it was not asked
     * about.
     *
     * Comments travel with the declaration they sit above, which is the whole point - the text
     * between one declaration and the next is copied across unchanged, indented to its new depth.
     * Nothing is dropped, including blank lines and anything the parser did not recognise.
     *
     * @param source     The stub's full text.
     * @param parser     Parser used to read it; the tree is created and released inside.
     * @param indent     One level of indentation, e.g. "\t" or four spaces.
     * @return The formatted text, or the source unchanged when it is already formatted, or when
     *         the file does not parse cleanly enough to be sure of what would move.
     */
    std::string FormatPredefinedStub(const std::string &source,
                                     angel_lsp::parser::AngelScriptParser &parser,
                                     const std::string &indent = "\t");
}
