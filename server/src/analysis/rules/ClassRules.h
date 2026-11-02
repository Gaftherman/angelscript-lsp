#pragma once

#include "analysis/diagnostics/DiagnosticContext.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/Symbol.h"
#include "analysis/index/SymbolTable.h"
#include "analysis/rules/RuleIndex.h"
#include "analysis/types/VirtualMixinContext.h"
#include "document/Document.h"
#include "parser/AngelScriptParser.h"

#include <ankerl/unordered_dense.h>
#include <memory>
#include <string>

namespace angel_lsp::analysis
{

namespace rules
{
using TransparentStringSet = ankerl::unordered_dense::set<std::string, TransparentStringHash, std::equal_to<>>;

/**

 * @brief Validates a class or interface declaration.
 *        Covers what a declaration says about itself - modifiers, mixin restrictions, template
 * @param sym Class or interface symbol to validate.
 * @param ctx Diagnostic sink; also carries the SymbolTable every lookup goes through.
 * @param mixinCache Optional shared cache of parsed mixin ASTs across class checks.
 */
void ValidateClass(const Symbol& sym, const DiagnosticContext& ctx, MixinFileCache* mixinCache = nullptr);

/**
 * @brief Reports whether a type participates in an inheritance cycle.
 * @param typeName Type to start from.
 * @param table Symbol table to resolve bases through.
 * @return True when typeName is reachable from itself.
 * @note Tracks the path being walked rather than every type ever seen. A shared visited set
 *        reports diamond inheritance - two bases with a base in common, which is ordinary in
 */
bool HasInheritanceCycle(const std::string& typeName, const SymbolTable& table);

/**
 * @brief Parameters for validating base class inheritance legality, accessibility, and cycle detection.
 */
struct BaseCheckRequest
{
    const Symbol& sym;
    const ClassSignature& sig;
    const DiagnosticContext& ctx;
};

/**
 * @brief Parameters for checking mixin inclusions, conflict detection, and interface requirements.
 */
struct MixinCheckContext
{
    std::string hostClassName;
    const Symbol* mixinSym = nullptr;
    SourceRange hostRange;
    const TransparentStringSet& hostMembers;
    const TransparentStringSet& mixinSelfMembers;
    TransparentStringSet& reported;
    const NodeIndex* nodeIndex = nullptr;
};

/**
 * @brief Tracks function signature validity, override correctness, and default argument rules.
 */
struct FunctionCheckState
{
    const ankerl::unordered_dense::set<std::string_view>& localNames;
    MixinCheckContext& mctx;
};

/**
 * @brief Aggregated member symbols and signatures collected from a mixin inheritance hierarchy.
 */
struct MixinHierarchyMembers
{
    std::string hostClassName;
    TransparentStringSet hostMembers;
};

/**
 * @brief Carries property accessor getter/setter lookup state and candidate symbols.
 */
struct AccessorLookupContext
{
    std::string_view name;
    std::string_view getView;
    std::string_view setView;
};

/**
 * @brief Parameters for verifying that a class correctly instantiates and satisfies all mixin contracts.
 */
struct MixinInstantiationCheckRequest
{
    const std::string& mixinName;
    const Symbol& hostSym;
    const MixinHierarchyMembers& baseMembers;
    const DiagnosticContext& ctx;
    ankerl::unordered_dense::map<std::string, MixinFileCacheEntry>& fileCache;
};

/**
 * @brief Identifies a mixin declaration AST node and target class for inline mixin expansion.
 */
struct MixinAstTarget
{
    std::string_view source;
    const TSTree* tree = nullptr;
    const NodeIndex* nodeIndex = nullptr;
    std::unique_ptr<NodeIndex> localIndex;
};
} // namespace rules
} // namespace angel_lsp::analysis
