#pragma once

#include "analysis/index/ScopeTree.h"
#include "analysis/index/SymbolTable.h"
#include "config/ServerConfig.h"
#include "i18n/i18n.h"
#include "parser/ASTUtils.h"
#include "utils/LspLogger.h"

#include <ankerl/unordered_dense.h>
#include <lsp/messages.h>
#include <lsp/types.h>
#include <optional>
#include <span>
#include <string>
#include <tree_sitter/api.h>
#include <tuple>
#include <vector>

namespace angel_lsp::features
{
using RangeKey = analysis::SourceRange;
using RangeKeyHash = analysis::SourceRangeHash;
using EnclosingClassCache = ankerl::unordered_dense::map<std::string, std::vector<parser::AstNodeSpan>>;
using DeclRangeSet = ankerl::unordered_dense::set<std::tuple<std::string, uint32_t, uint32_t>>;
using TargetIndexMap =
    ankerl::unordered_dense::map<std::string, std::vector<size_t>, analysis::TransparentStringHash, std::equal_to<>>;
using GroupMap = ankerl::unordered_dense::map<RangeKey, std::vector<analysis::Symbol>, RangeKeyHash>;
using MixinInclusionMap = ankerl::unordered_dense::map<std::string, std::vector<std::string>>;
using KeyTargetMap = ankerl::unordered_dense::map<RangeKey, size_t, RangeKeyHash>;
/**
 * @brief Context and input parameters for a Code Lens request.
 */
struct CodeLensRequest
{
    std::string uri;
    std::string sourceCode;
    TSTree* tree = nullptr;
    const analysis::SymbolTable& symbolTable;
    const analysis::ScopeIndex& scopeIndex;
    angel_lsp::utils::LspLogger* logger = nullptr;
    std::string predefinedExtension = ".as.predefined";
    const config::UnusedSymbolRules* unusedRules = nullptr;
    const i18n::I18n* i18n = nullptr;
    const config::ServerConfig* config = nullptr;
};

/**
 * @brief Context and input parameters for resolving a Code Lens item.
 */
struct CodeLensResolveRequest
{
    const lsp::CodeLens& codeLens;
    const analysis::SymbolTable& symbolTable;
    const analysis::ScopeIndex& scopeIndex;
    angel_lsp::utils::LspLogger* logger = nullptr;
};

/**
 * @brief Computes code lenses (such as reference counts and interface implementations) for a document.
 * @param request Immutable context for code lens generation.
 * @return List of code lenses for classes, interfaces, methods, and functions.
 */
std::optional<std::vector<lsp::CodeLens>> GetCodeLenses(const CodeLensRequest& request);

/**
 * @brief Resolves additional command or details for a given Code Lens.
 * @param request Immutable context for code lens resolution.
 * @return Resolved Code Lens.
 */
std::optional<lsp::CodeLens> ResolveCodeLens(const CodeLensResolveRequest& request);

/**
 * @brief Filter criteria for finding references in CodeLens computations.
 */
struct ReferenceCollectionCriteria
{
    std::string targetName;
    const std::vector<analysis::Symbol>& group;
    const ankerl::unordered_dense::set<std::string>& compatibleClasses;
    const DeclRangeSet& allDeclRanges;
    std::string targetNamespace;
    std::string targetEnumContainer;
    bool isEnumConstant = false;
    analysis::AccessModifier targetAccess = analysis::AccessModifier::Public;
    bool isFunction = false;
    size_t minArgs = 0;
    size_t maxArgs = 0;
    const analysis::SymbolTable& symbolTable;
    const CodeLensRequest& request;
    EnclosingClassCache* classCache = nullptr;
};

/**
 * @brief Target symbol and line range for computing reference and implementation CodeLens items.
 */
struct SymbolLensTarget
{
    RangeKey key;
    std::string targetName;
    const std::vector<analysis::Symbol>& group;
    ankerl::unordered_dense::set<std::string> compatibleClasses;
    DeclRangeSet allDeclRanges;
    std::string targetNamespace;
    std::string targetEnumContainer;
    bool isEnumConstant = false;
    analysis::AccessModifier targetAccess = analysis::AccessModifier::Public;
    bool isFunction = false;
    size_t minArgs = 0;
    size_t maxArgs = 0;
    ankerl::unordered_dense::set<std::pair<std::string, uint64_t>> seenRefs;
};

/**
 * @brief Parameters for batched cross-document reference collection in CodeLens.
 */
struct BatchReferenceContext
{
    BatchReferenceContext(std::span<SymbolLensTarget> t, const TargetIndexMap& map, const CodeLensRequest& req)
        : targets(t), targetsByName(map), request(req)
    {
    }

    std::span<SymbolLensTarget> targets;
    const TargetIndexMap& targetsByName;
    const CodeLensRequest& request;
    mutable EnclosingClassCache classCache;
};

/**
 * @brief Groups symbols by declaring type or namespace for batch reference analysis.
 */
struct SymbolGroups
{
    std::vector<RangeKey> rangeOrder;
    GroupMap groups;
};

/**
 * @brief Context for discovering derived classes compatible with an interface or base class in CodeLens.
 */
struct CompatibleClassContext
{
    const MixinInclusionMap& mixinMap;
    const analysis::SymbolTable& symbolTable;
    std::string_view sourceCode{};
};

/**
 * @brief Finished CodeLens data payload ready for serialization.
 */
struct AssembledLensContext
{
    const std::vector<SymbolLensTarget>& targets;
    const KeyTargetMap& keyToTarget;
    const CodeLensRequest& request;
};
} // namespace angel_lsp::features
