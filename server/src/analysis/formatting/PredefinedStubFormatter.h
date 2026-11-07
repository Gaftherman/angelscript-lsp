#pragma once

#include "parser/AngelScriptParser.h"

#include <map>
#include <string>
#include <vector>

namespace angel_lsp::analysis::formatting
{
/**
 * @brief Rewrites a predefined stub so each namespace is declared once, as a block.
 *        A stub generated from an engine's registration table writes one declaration per registered
 * @param source     The stub's full text.
 * @param parser     Parser used to read it; the tree is created and released inside.
 * @param indent     One level of indentation, e.g. "\t" or four spaces.
 * @return The formatted text, or the source unchanged when it is already formatted, or when
 *        the file does not parse cleanly enough to be sure of what would move.
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
 *        formatting.
 */
struct FormattingPlan
{
    std::vector<Slot> slots;
    std::map<std::string, Group> groups;
    std::string tail;
};
} // namespace angel_lsp::analysis::formatting
