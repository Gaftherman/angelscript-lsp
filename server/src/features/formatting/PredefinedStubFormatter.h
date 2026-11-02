#pragma once

#include <map>
#include <string>
#include <vector>

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
std::string FormatPredefinedStub(const std::string& source, angel_lsp::parser::AngelScriptParser& parser,
                                 const std::string& indent = "\t");

/**
 * @brief Records top-level declaration text and preserved comments in a predefined stub source file.
 */
struct Occurrence
{
    /// Text between the previous top-level declaration and this one: its comments, and any
    /// blank lines. Carried across so a comment never parts company with what it describes.
    std::string lead;

    /// Everything between the body's braces, exactly as written.
    std::string body;
};

/**
 * @brief Groups related namespace declarations and member occurrences for unified stub emission.
 */
struct Group
{
    std::string name;
    std::vector<Occurrence> occurrences;

    /// Index into the output plan, so a merged namespace lands where it was first written
    /// rather than being sorted somewhere new.
    size_t firstSlot = 0;
};

/**
 * @brief Output section slot in a predefined stub formatting plan, mapping namespace chunks to emission targets.
 */
struct Slot
{
    bool isGroup = false;
    std::string literal;   ///< used when isGroup is false
    std::string groupName; ///< used when isGroup is true
};

/**
 * @brief Execution plan detailing merged namespaces, reordered declarations, and whitespace layouts for stub
 * formatting.
 */
struct FormattingPlan
{
    std::vector<Slot> slots;
    std::map<std::string, Group> groups;
    std::string tail;
};
} // namespace angel_lsp::features::formatting
