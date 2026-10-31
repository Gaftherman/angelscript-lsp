#pragma once

#include "analysis/NodeIndex.h"
#include "document/Document.h"
#include "parser/AngelScriptParser.h"

#include <ankerl/unordered_dense.h>
#include <memory>
#include <string>

namespace angel_lsp::analysis
{
struct DiagnosticContext;
class NodeIndex;
struct Symbol;
class SymbolTable;

namespace rules
{
/**
 * @brief Cache entry for parsed mixin source and AST tree.
 */
struct MixinFileCacheEntry
{
    std::string source;
    std::unique_ptr<parser::AngelScriptParser> parser;
    document::TreePtr tree;
    std::unique_ptr<NodeIndex> nodeIndex;
};

using MixinFileCache = ankerl::unordered_dense::map<std::string, MixinFileCacheEntry>;

/**
 * @brief Validates a class or interface declaration.
 *
 * Covers what a declaration says about itself - modifiers, mixin restrictions, template
 * placement - and what it says about its bases: they must exist, be inheritable, be at most
 * one class, not form a cycle, and have their interface methods implemented.
 *
 * @param sym Class or interface symbol to validate.
 * @param ctx Diagnostic sink; also carries the SymbolTable every lookup goes through.
 * @param mixinCache Optional shared cache of parsed mixin ASTs across class checks.
 */
void ValidateClass(const Symbol& sym, const DiagnosticContext& ctx, MixinFileCache* mixinCache = nullptr);

/**
 * @brief Reports whether a type participates in an inheritance cycle.
 *
 * @param typeName Type to start from.
 * @param table Symbol table to resolve bases through.
 * @return True when typeName is reachable from itself.
 * @note Tracks the path being walked rather than every type ever seen. A shared visited set
 *       reports diamond inheritance - two bases with a base in common, which is ordinary in
 *       interface hierarchies - as a cycle.
 */
bool HasInheritanceCycle(const std::string& typeName, const SymbolTable& table);
} // namespace rules
} // namespace angel_lsp::analysis
