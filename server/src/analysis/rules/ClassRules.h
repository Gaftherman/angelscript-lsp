#pragma once

#include "analysis/diagnostics/DiagnosticContext.h"
#include "analysis/index/ClassRuleFacts.h"
#include "analysis/index/MixinSourceIndex.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/RuleIndex.h"
#include "analysis/index/Symbol.h"
#include "analysis/index/SymbolTable.h"
#include "document/Document.h"

#include <ankerl/unordered_dense.h>
#include <memory>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

namespace rules
{
/// Set of member names borrowed from RuleIndex or SymbolTable string storage.
using MemberNameSet = ankerl::unordered_dense::set<std::string_view>;

/**
 * @brief Validates a class or interface declaration.
 *        Covers what a declaration says about itself - modifiers, mixin restrictions, template
 * @param sym Class or interface symbol to validate.
 * @param ctx Diagnostic sink; also carries the SymbolTable every lookup goes through.
 * @param mixinCache Optional shared cache of parsed mixin ASTs across class checks.
 */
void ValidateClass(const Symbol& sym, const DiagnosticContext& ctx, MixinFileCache* mixinCache = nullptr);

/**
 * @brief Depth-first walk over the base lists starting at current, keeping only the active path so diamonds are not
 *        mistaken for cycles.
 * @param[in,out] path Types on the current walk; current is pushed on entry and popped on exit.
 * @param[in] current Type whose bases are expanded next.
 * @param[in] table Symbol table used to resolve each base.
 * @return True when a base on the walk is already on path, i.e. a cycle closes.
 */
bool WalkInheritancePath(std::vector<std::string_view>& path, std::string_view current, const SymbolTable& table);

/**
 * @brief Reports whether a type participates in an inheritance cycle.
 * @param[in] typeName Type to start from.
 * @param[in] table Symbol table to resolve bases through.
 * @return True when typeName is reachable from itself.
 * @note Tracks the path being walked rather than every type ever seen. A shared visited set
 *        reports diamond inheritance - two bases with a base in common, which is ordinary in
 */
bool HasInheritanceCycle(std::string_view typeName, const SymbolTable& table);

/**
 * @brief Enforces the modifier and shape rules a class declaration must satisfy on its own: body presence,
 *        external declarations and the allowed mixin/abstract/final/shared combinations.
 * @param[in] sym Class symbol being validated.
 * @param[in] sig Class signature carrying the parsed modifiers and body flag.
 * @param[in] ctx Diagnostic context that receives the emitted errors.
 */
void CheckClassModifiers(const Symbol& sym, const ClassSignature& sig, const DiagnosticContext& ctx);

/**
 * @brief Validates every entry of a class base list: the base must exist, be a class or interface of a legal
 *        kind, appear at most once as a class base and not be final.
 * @param[in] sym Class symbol whose base list is checked; its line span locates the base_class_list facts.
 * @param[in] sig Class signature listing the spelled base names.
 * @param[in] ctx Diagnostic context that receives one diagnostic per offending base, ranged on that base.
 */
void CheckBases(const Symbol& sym, const ClassSignature& sig, const DiagnosticContext& ctx);

/**
 * @brief Reports methods and properties that replace a member some visible base class declared final.
 * @param[in] sym Class symbol whose members are compared against its inherited hierarchy.
 * @param[in] sig Class signature listing the direct bases.
 * @param[in] ctx Diagnostic context that receives the override errors.
 */
void CheckFinalOverrides(const Symbol& sym, const ClassSignature& sig, const DiagnosticContext& ctx);

/**
 * @brief Reports interface methods that a non-abstract class implements nowhere in its own hierarchy.
 * @param[in] sym Class symbol being validated.
 * @param[in] sig Class signature listing the interfaces it implements directly.
 * @param[in] ctx Diagnostic context that receives one diagnostic per missing method.
 */
void CheckInterfaceImplementation(const Symbol& sym, const ClassSignature& sig, const DiagnosticContext& ctx);

/**
 * @brief Validates the bases of an interface: each must resolve and must itself be an interface.
 * @param[in] sym Interface symbol whose base list is checked.
 * @param[in] sig Interface signature listing the spelled base names.
 * @param[in] ctx Diagnostic context that receives one diagnostic per invalid base.
 */
void CheckInterfaceBases(const Symbol& sym, const InterfaceSignature& sig, const DiagnosticContext& ctx);

/**
 * @brief Reports get_/set_ property accessor pairs of one class or interface whose value types disagree.
 * @param[in] sym Class or interface symbol whose accessor members are paired by property name.
 * @param[in] ctx Diagnostic context that receives the mismatch diagnostics.
 */
void CheckPropertyAccessors(const Symbol& sym, const DiagnosticContext& ctx);

/**
 * @brief Checks that the statements of every mixin a class includes resolve against the host class, its
 *        hierarchy and the mixin's own members, reporting each unresolved name once at the inclusion site.
 * @param[in] sym Host class symbol that includes the mixins.
 * @param[in] sig Host class signature listing the included mixin names.
 * @param[in] ctx Diagnostic context that receives the related diagnostics.
 * @param[in,out] sharedCache Optional cross-document cache of parsed mixin files; a local cache is used when null.
 */
void CheckMixinInstantiations(const Symbol& sym, const ClassSignature& sig, const DiagnosticContext& ctx,
                              MixinFileCache* sharedCache);

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
    /// Name of the host class; views string storage owned by SymbolTable.
    std::string_view hostClassName;
    const Symbol* mixinSym = nullptr;
    SourceRange hostRange;
    const MemberNameSet& hostMembers;
    const MemberNameSet& mixinSelfMembers;
    MemberNameSet& reported;
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
    /// Name of the host class; views string storage owned by SymbolTable.
    std::string_view hostClassName;
    /// Member names collected across hierarchy; views string storage owned by RuleIndex.
    MemberNameSet hostMembers;
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
    /// Mixin name; views string storage owned by ClassSignature or mixin source text.
    std::string_view mixinName;
    const Symbol& hostSym;
    const MixinHierarchyMembers& baseMembers;
    const DiagnosticContext& ctx;
    MixinFileCache& fileCache;
};

/**
 * @brief Source text and first-pass index that a mixin body is checked against.
 */
struct MixinAstTarget
{
    /// Mixin document text; borrows the current request or a file cache entry.
    std::string_view source;
    /// Index over source; points at the request index, the cache entry index or localIndex.
    const NodeIndex* nodeIndex = nullptr;
    /// Index built on demand when the current document has a tree but no request index.
    std::unique_ptr<NodeIndex> localIndex;
};

/**
 * @brief Checks every member reference of the mixin functions against the host class, the mixin itself and the
 *        visible global, namespace and local names, reporting each unresolved name once.
 * @param[in] facts Mixin body facts listing each function's local names and references in diagnostic order.
 * @param[in] mixinSource Text of the mixin document; reference and local-name spans index into it.
 * @param[in,out] mctx Check context whose reported set records the names already diagnosed.
 * @param[in] ctx Diagnostic context that receives the related diagnostics.
 */
void CheckMixinFunctions(const MixinBodyFacts& facts, std::string_view mixinSource, MixinCheckContext& mctx,
                         const DiagnosticContext& ctx);

/**
 * @brief Decides whether a bare name used inside a mixin function resolves once the mixin is expanded in the host:
 *        as a local, a mixin or host member, a property accessor, a keyword or built-in, an engine or global
 *        symbol, or a member of an enclosing or imported namespace.
 * @param[in] name Identifier text taken from the mixin source.
 * @param[in] state Function check state carrying the function's local names and the mixin check context.
 * @param[in] ctx Diagnostic context giving access to the symbol table and engine configuration.
 * @return True when the name resolves and no diagnostic is needed.
 */
bool IsValidMixinIdentifier(std::string_view name, const FunctionCheckState& state, const DiagnosticContext& ctx);
} // namespace rules
} // namespace angel_lsp::analysis
