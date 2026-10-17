#include "analysis/rules/UnusedSymbolChecker.h"

#include "analysis/ASTUtils.h"
#include "analysis/DiagnosticCodes.h"
#include "analysis/DiagnosticContext.h"
#include "analysis/NodeIndex.h"
#include "analysis/ScopeTree.h"
#include "analysis/SemanticHelpers.h"
#include "analysis/SymbolTable.h"
#include "analysis/rules/RuleIndex.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarNames.h"
#include "parser/GrammarSymbols.h"
#include "utils/Utils.h"

#include <algorithm>
#include <ankerl/unordered_dense.h>
#include <cctype>
#include <regex>
#include <string>
#include <vector>

namespace angel_lsp::analysis::rules
{
namespace
{

struct UnusedCheckContext
{
    const ankerl::unordered_dense::set<std::string>& refNames;
    const ankerl::unordered_dense::set<const LocalDefinition*>& usedLocals;
    const ankerl::unordered_dense::set<std::string>& reflections;
    const config::UnusedSymbolRules& rules;
    const ankerl::unordered_dense::set<std::string, TransparentStringHash, std::equal_to<>>& inheritedOrMixedInClasses;
    const ankerl::unordered_dense::map<std::string, std::vector<std::string>, TransparentStringHash, std::equal_to<>>&
        derivedClassesByBase;
    const RuleIndex& ruleIndex;
};

inline uint64_t PackLoc(uint32_t line, uint32_t col) noexcept
{
    return (static_cast<uint64_t>(line) << 32) | static_cast<uint64_t>(col);
}

void CollectDefLocations(const Scope* scope, ankerl::unordered_dense::set<uint64_t>& defLocs, int depth = 0)
{
    if (!scope || depth > k_maxAstDepth)
        return;
    for (const auto& def : scope->definitions)
        defLocs.insert(PackLoc(def.startLine, def.startCharacter));
    for (const auto& child : scope->children)
        CollectDefLocations(child.get(), defLocs, depth + 1);
}

void CollectDocReferences(const Scope* scope, const ankerl::unordered_dense::set<uint64_t>& defLocs,
                          ankerl::unordered_dense::set<std::string>& refNames, std::string_view sourceCode,
                          std::span<const std::string> delegateProps, int depth = 0)
{
    if (!scope || depth > k_maxAstDepth)
        return;
    for (const auto& ref : scope->references)
    {
        if (defLocs.contains(PackLoc(ref.startLine, ref.startCharacter)))
            continue;
        if (!delegateProps.empty() && ref.isMemberAccess && ref.receiverEndByte > ref.receiverStartByte &&
            !sourceCode.empty() && ref.receiverEndByte <= sourceCode.size())
        {
            std::string_view recv =
                sourceCode.substr(ref.receiverStartByte, ref.receiverEndByte - ref.receiverStartByte);
            if (std::find(delegateProps.begin(), delegateProps.end(), recv) != delegateProps.end())
            {
                continue;
            }
        }
        refNames.insert(ref.name);
    }
    for (const auto& child : scope->children)
        CollectDocReferences(child.get(), defLocs, refNames, sourceCode, delegateProps, depth + 1);
}

void CollectUsedLocals(const Scope* scope, ankerl::unordered_dense::set<const LocalDefinition*>& usedLocals,
                       int depth = 0)
{
    if (!scope || depth > k_maxAstDepth)
        return;
    for (const auto& ref : scope->references)
    {
        if (ref.isMemberAccess || ref.isNamedArgument)
            continue;
        const LocalDefinition* def = ResolveInScope(scope, ref.name);
        if (def && (ref.startLine != def->startLine || ref.startCharacter != def->startCharacter))
            usedLocals.insert(def);
    }
    for (const auto& child : scope->children)
        CollectUsedLocals(child.get(), usedLocals, depth + 1);
}

void CollectCrossFileReferences(const SemanticAnalysisRequest& req, ankerl::unordered_dense::set<std::string>& refNames)
{
    if (!req.scopeIndex || req.moduleFileUris.empty())
        return;
    for (const auto& uri : req.moduleFileUris)
    {
        if (uri != req.fileUri)
        {
            if (const auto root = req.scopeIndex->GetRoot(uri))
            {
                ankerl::unordered_dense::set<uint64_t> defs;
                CollectDefLocations(root.get(), defs);
                CollectDocReferences(root.get(), defs, refNames, "", {});
            }
        }
    }
}

std::string ExtractStringValue(TSNode exprNode, const SemanticAnalysisRequest& req, int depth = 0)
{
    if (ts_node_is_null(exprNode) || depth > 8)
        return {};

    const std::string_view type = ts_node_type(exprNode);
    if (type == parser::nodes::StringLiteral)
    {
        std::string text = parser::GetNodeText(exprNode, req.sourceCode);
        if (text.size() >= 2 && text.front() == '"' && text.back() == '"')
            return text.substr(1, text.size() - 2);
        return text;
    }
    if (type == parser::nodes::BinaryExpression)
    {
        TSNode left = ts_node_child_by_field_name(exprNode, "left", 4);
        TSNode right = ts_node_child_by_field_name(exprNode, "right", 5);
        return ExtractStringValue(left, req, depth + 1) + ExtractStringValue(right, req, depth + 1);
    }
    return {};
}

bool MatchCallee(std::string_view funcText, const config::StringReflectionCallee& c)
{
    std::string_view receiver;
    std::string_view method = funcText;
    const auto dotPos = funcText.find('.');
    const auto colonPos = funcText.rfind("::");

    if (dotPos != std::string_view::npos)
    {
        receiver = funcText.substr(0, dotPos);
        method = funcText.substr(dotPos + 1);
    }
    else if (colonPos != std::string_view::npos)
    {
        receiver = funcText.substr(0, colonPos);
        method = funcText.substr(colonPos + 2);
    }

    if (!c.callee.empty() && receiver != c.callee)
        return false;

    return std::any_of(c.methods.begin(), c.methods.end(), [&](const std::string& m) { return m == method; });
}

bool MatchesRegexSafe(const std::string& candidate, const std::string& pattern)
{
    if (pattern.empty() || candidate.empty())
    {
        return false;
    }
    thread_local ankerl::unordered_dense::map<std::string, std::optional<std::regex>> regexCache;
    auto it = regexCache.find(pattern);
    if (it == regexCache.end())
    {
        try
        {
            it = regexCache.emplace(pattern, std::regex(pattern)).first;
        }
        catch (const std::regex_error&)
        {
            it = regexCache.emplace(pattern, std::nullopt).first;
        }
    }
    if (!it->second.has_value())
    {
        return false;
    }
    return std::regex_match(candidate, *it->second);
}

void RegisterReflectedSymbol(const std::string& str, const SemanticAnalysisRequest& req,
                             ankerl::unordered_dense::set<std::string>& outReflections)
{
    if (str.empty())
        return;

    outReflections.insert(str);

    if (const auto sym = req.symbolTable.FindFirstSymbol(str))
    {
        outReflections.insert(sym->name);
        outReflections.insert(sym->qualifiedName);
        return;
    }

    if (str.find("::") == std::string::npos)
    {
        auto types = req.symbolTable.FindTypeSymbolsByShortName(str);
        for (const auto& t : types)
        {
            outReflections.insert(t.name);
            outReflections.insert(t.qualifiedName);
        }
    }
}

void MatchConfiguredReflectionCallees(TSNode callNode, const SemanticAnalysisRequest& req,
                                      const std::vector<config::StringReflectionCallee>& callees,
                                      ankerl::unordered_dense::set<std::string>& result)
{
    TSNode funcNode = parser::GetChildByField(callNode, parser::fields::Function);
    if (ts_node_is_null(funcNode))
        return;

    std::string_view funcText = analysis::GetNodeTextView(funcNode, req.sourceCode);
    std::optional<std::vector<analysis::CallArgumentInfo>> callArgs;

    for (const auto& c : callees)
    {
        if (!MatchCallee(funcText, c))
            continue;

        if (!callArgs.has_value())
        {
            callArgs = ExtractCallArguments(callNode, req.sourceCode);
        }

        for (size_t idx : c.argIndices)
        {
            if (idx < callArgs->size())
                RegisterReflectedSymbol(ExtractStringValue((*callArgs)[idx].exprNode, req), req, result);
        }
    }
}

void CollectScopeStringLiteralRefs(const Scope* root, ankerl::unordered_dense::set<std::string>& result)
{
    if (!root)
    {
        return;
    }
    std::vector<const Scope*> stack{root};
    while (!stack.empty())
    {
        const Scope* s = stack.back();
        stack.pop_back();
        for (const auto& strRef : s->stringLiteralCallRefs)
        {
            result.insert(strRef.name);
        }
        for (const auto& child : s->children)
        {
            if (child)
            {
                stack.push_back(child.get());
            }
        }
    }
}

ankerl::unordered_dense::set<std::string> CollectStringReflection(const SemanticAnalysisRequest& req)
{
    ankerl::unordered_dense::set<std::string> result;
    if (req.scopeRoot)
    {
        CollectScopeStringLiteralRefs(req.scopeRoot.get(), result);
    }
    if (req.scopeIndex)
    {
        req.scopeIndex->ForEachScopeTree(
            [&](const std::string& uri, const std::shared_ptr<const Scope>& root)
            {
                if (root && uri != req.fileUri && !utils::IsPredefinedFile(uri, req.predefinedFileExtension))
                {
                    CollectScopeStringLiteralRefs(root.get(), result);
                }
            });
    }

    const std::vector<config::StringReflectionCallee>* callees =
        req.engineRules ? &req.engineRules->unusedRules.stringReflectionCallees : nullptr;

    if (callees && !callees->empty() && req.nodeIndex)
    {
        const auto& syms = parser::GetGrammarSymbols();
        for (TSNode callNode : req.nodeIndex->Nodes(syms.symCallExpression))
        {
            MatchConfiguredReflectionCallees(callNode, req, *callees, result);
        }
    }
    return result;
}

std::string NormalizeParamType(std::string_view type)
{
    std::string res;
    res.reserve(type.size());
    for (char c : type)
    {
        if (!std::isspace(static_cast<unsigned char>(c)))
            res.push_back(c);
    }
    std::string_view sv = res;
    if (sv.starts_with("const"))
        sv.remove_prefix(5);
    if (sv.ends_with("&inout"))
        sv.remove_suffix(6);
    else if (sv.ends_with("&in") || sv.ends_with("&out"))
        sv.remove_suffix(3);
    else if (sv.ends_with('&'))
        sv.remove_suffix(1);

    return std::string(sv);
}

} // namespace

bool MatchesEngineCallbackSignature(const Symbol& sym, const std::vector<std::vector<std::string>>& signatures)
{
    if (sym.type != SymbolType::Function || signatures.empty() ||
        !std::holds_alternative<FunctionSignature>(sym.signature))
    {
        return false;
    }
    const auto& fn = sym.GetFunction();
    for (const auto& sig : signatures)
    {
        if (fn.parameters.size() != sig.size())
            continue;
        bool match = true;
        for (size_t i = 0; i < sig.size(); ++i)
        {
            if (NormalizeParamType(fn.parameters[i].typeName) != NormalizeParamType(sig[i]))
            {
                match = false;
                break;
            }
        }
        if (match)
            return true;
    }
    return false;
}

bool IsEngineEntity(const std::string& className, const SymbolTable& symbolTable,
                    const config::UnusedSymbolRules& rules)
{
    const auto& ignored = rules.ignoredBaseClasses;
    if (ignored.empty())
        return false;
    if (std::find(ignored.begin(), ignored.end(), className) != ignored.end())
        return true;
    std::vector<std::string> hierarchy = GetInheritedTypeHierarchy(className, symbolTable);
    if (const auto ruleIndex = symbolTable.GetRuleIndex())
    {
        auto it = ruleIndex->hostClassesByMixin.find(className);
        if (it != ruleIndex->hostClassesByMixin.end())
        {
            for (const auto& host : it->second)
            {
                std::string hostKey = host.qualifiedName.empty() ? host.name : host.qualifiedName;
                auto hostHier = GetInheritedTypeHierarchy(hostKey, symbolTable);
                hierarchy.insert(hierarchy.end(), hostHier.begin(), hostHier.end());
            }
        }
    }
    return std::any_of(hierarchy.begin(), hierarchy.end(), [&ignored](const std::string& b)
                       { return std::find(ignored.begin(), ignored.end(), b) != ignored.end(); });
}

namespace
{

bool CheckBaseClassLifecycleMethods(std::string_view className, std::string_view methodName,
                                    const config::UnusedSymbolRules& unusedRules)
{
    auto it = unusedRules.baseClassLifecycleMethods.find(className);
    if (it == unusedRules.baseClassLifecycleMethods.end() && className.find("::") != std::string_view::npos)
    {
        it = unusedRules.baseClassLifecycleMethods.find(LastScopeSegment(className));
    }
    return it != unusedRules.baseClassLifecycleMethods.end() &&
           std::find(it->second.begin(), it->second.end(), methodName) != it->second.end();
}

} // namespace

bool IsLifecycleOrOverride(const Symbol& sym, const SymbolTable& symbolTable,
                           const config::UnusedSymbolRules& unusedRules)
{
    if (sym.containerName.empty())
        return false;

    LinkedBaseOptions options;
    options.delegateProps = unusedRules.baseClassDelegateProperties;
    options.proxyMap = &unusedRules.baseClassProxyMap;

    auto overridden = FindOverriddenBaseMethodSymbols(sym.containerName, sym.name, symbolTable, options);
    if (!overridden.empty())
    {
        return true;
    }

    std::vector<std::string> hierarchy = GetInheritedTypeHierarchy(std::string(sym.containerName), symbolTable);
    ExpandMixinHostHierarchies(sym.containerName, symbolTable, hierarchy);

    const bool hasIgnoredBases = !unusedRules.ignoredBaseClasses.empty();
    bool isEntity = !hasIgnoredBases;
    const bool hasBaseMethods = !unusedRules.baseClassLifecycleMethods.empty();
    const bool hasOptions = options.HasActiveOptions();

    for (const auto& ancestor : hierarchy)
    {
        if (hasIgnoredBases && !isEntity)
        {
            if (std::find(unusedRules.ignoredBaseClasses.begin(), unusedRules.ignoredBaseClasses.end(), ancestor) !=
                unusedRules.ignoredBaseClasses.end())
            {
                isEntity = true;
            }
        }
        if (hasBaseMethods && CheckBaseClassLifecycleMethods(ancestor, sym.name, unusedRules))
        {
            return true;
        }
        if (hasOptions && hasBaseMethods)
        {
            auto linkedBases = ResolveLinkedBaseClasses(ancestor, symbolTable, options);
            for (const auto& lb : linkedBases)
            {
                if (CheckBaseClassLifecycleMethods(lb, sym.name, unusedRules))
                {
                    return true;
                }
            }
        }
    }

    if (isEntity)
    {
        const auto& methods = unusedRules.lifecycleMethods;
        if (std::find(methods.begin(), methods.end(), sym.name) != methods.end())
        {
            return true;
        }
    }

    return false;
}

bool IsEngineManagedFunction(const Symbol& sym, const SymbolTable& symbolTable,
                             const config::UnusedSymbolRules& unusedRules)
{
    if (sym.type != SymbolType::Function)
        return false;

    if (sym.containerName.empty() || sym.containerKind == ContainerKind::Namespace)
    {
        if (std::find(unusedRules.ignoredGlobalFunctions.begin(), unusedRules.ignoredGlobalFunctions.end(), sym.name) !=
                unusedRules.ignoredGlobalFunctions.end() ||
            (!sym.qualifiedName.empty() &&
             std::find(unusedRules.ignoredGlobalFunctions.begin(), unusedRules.ignoredGlobalFunctions.end(),
                       sym.qualifiedName) != unusedRules.ignoredGlobalFunctions.end()))
        {
            return true;
        }
        if (MatchesEngineCallbackSignature(sym, unusedRules.engineCallbackSignatures))
        {
            return true;
        }
        return MatchesRegexSafe(sym.name, unusedRules.ignoredGlobalFunctionRegex) ||
               (!sym.qualifiedName.empty() &&
                MatchesRegexSafe(sym.qualifiedName, unusedRules.ignoredGlobalFunctionRegex));
    }

    return IsLifecycleOrOverride(sym, symbolTable, unusedRules);
}

namespace
{

bool IsIgnoredByDiagnosticsConfig(const Symbol& sym, const SemanticAnalysisRequest& request)
{
    if (!request.diagnostics)
    {
        return false;
    }
    const auto& diag = *request.diagnostics;
    if (diag.ignoredUnusedFunctions.contains(sym.name) ||
        (!sym.qualifiedName.empty() && diag.ignoredUnusedFunctions.contains(sym.qualifiedName)))
    {
        return true;
    }
    for (const auto& pattern : diag.ignoredUnusedFunctionPatterns)
    {
        if (MatchesRegexSafe(sym.name, pattern) ||
            (!sym.qualifiedName.empty() && MatchesRegexSafe(sym.qualifiedName, pattern)))
        {
            return true;
        }
    }
    return false;
}

bool IsIgnoredFunction(const Symbol& sym, const SemanticAnalysisRequest& request,
                       const config::UnusedSymbolRules& rules)
{
    if (IsIgnoredByDiagnosticsConfig(sym, request))
    {
        return true;
    }
    return IsEngineManagedFunction(sym, request.symbolTable, rules);
}

void CheckLocalsAndFields(const Scope* scope, const UnusedCheckContext& uCtx, DiagnosticContext& ctx, int depth = 0)
{
    if (!scope || depth > k_maxAstDepth)
        return;

    bool isFunc = false;
    for (const Scope* cur = scope; cur != nullptr && !isFunc; cur = cur->parent)
        isFunc = cur->isFunctionScope;

    for (const auto& def : scope->definitions)
    {
        if (uCtx.rules.checkLocals && isFunc && def.kind == LocalDefinitionKind::Variable &&
            !uCtx.usedLocals.contains(&def))
        {
            ctx.EmitAtRange({def.startLine, def.startCharacter, def.endLine, def.endCharacter},
                            diagnostics::codes::UnusedVariable, def.name, DiagnosticSeverity::Warning);
        }
        else if (uCtx.rules.checkMembers && def.kind == LocalDefinitionKind::Field &&
                 !uCtx.refNames.contains(def.name) && !uCtx.reflections.contains(def.name))
        {
            ctx.EmitAtRange({def.startLine, def.startCharacter, def.endLine, def.endCharacter},
                            diagnostics::codes::UnusedField, def.name, DiagnosticSeverity::Warning);
        }
    }

    for (const auto& child : scope->children)
        CheckLocalsAndFields(child.get(), uCtx, ctx, depth + 1);
}

static bool IsExemptGlobalType(const VariableSignature& var, const std::vector<std::string>& exemptTypes)
{
    if (exemptTypes.empty())
    {
        return false;
    }
    const std::string_view cleanType = CleanBaseTypeView(var.typeName);
    for (const auto& exempt : exemptTypes)
    {
        if (cleanType == exempt || var.baseTypeName == exempt)
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief Checks if a symbol represents a candidate global variable for unused checking.
 * @param[in] sym Symbol under test.
 * @param[in] ignoreCallInit True to ignore variables initialized with a call.
 * @param[in] exemptTypes Collection of type names exempted from unused checking.
 * @param[in] req Analysis request containing diagnostic settings.
 * @return True if symbol is a candidate global variable, false otherwise.
 */
[[nodiscard]] bool IsCandidateGlobalVariable(const Symbol& sym, bool ignoreCallInit,
                                             const std::vector<std::string>& exemptTypes,
                                             const SemanticAnalysisRequest& req)
{
    if (sym.type != SymbolType::Variable || !sym.containerName.empty() ||
        !std::holds_alternative<VariableSignature>(sym.signature))
    {
        return false;
    }
    const auto& var = sym.GetVariable();
    if (var.isEnumConstant || (ignoreCallInit && var.hasCallInitializer))
    {
        return false;
    }
    return !IsExemptGlobalType(var, exemptTypes) && !IsIgnoredByDiagnosticsConfig(sym, req);
}

/**
 * @brief Checks if a symbol is unreferenced across document references and reflection calls.
 * @param[in] name Short symbol name.
 * @param[in] qualifiedName Qualified symbol name.
 * @param[in] uCtx Context holding reference and reflection sets.
 * @return True if symbol is unreferenced, false otherwise.
 */
[[nodiscard]] bool IsSymbolUnreferenced(const std::string& name, const std::string& qualifiedName,
                                        const UnusedCheckContext& uCtx)
{
    return !uCtx.refNames.contains(name) && !uCtx.reflections.contains(name) &&
           !uCtx.reflections.contains(qualifiedName);
}

void CheckUnusedGlobals(const SemanticAnalysisRequest& req, const UnusedCheckContext& uCtx, DiagnosticContext& ctx)
{
    if (!uCtx.rules.checkGlobals)
        return;

    const bool ignoreCallInit = uCtx.rules.ignoreUnusedGlobalsWithCallInitializer ||
                                (req.diagnostics && req.diagnostics->ignoreUnusedGlobalsWithCallInitializer);

    req.symbolTable.ForEachSymbolInFile(
        req.fileUri,
        [&]([[maybe_unused]] const std::string& qName, const std::vector<Symbol>& syms)
        {
            for (const auto& sym : syms)
            {
                if (!IsCandidateGlobalVariable(sym, ignoreCallInit, uCtx.rules.unusedGlobalExemptTypes, req))
                {
                    continue;
                }
                if (IsSymbolUnreferenced(sym.name, sym.qualifiedName, uCtx))
                {
                    ctx.Emit(sym, diagnostics::codes::UnusedGlobalVariable, sym.name, DiagnosticSeverity::Warning);
                }
            }
        });
}

bool IsPropertyAccessorUsed(std::string_view name, const ankerl::unordered_dense::set<std::string>& refNames)
{
    if (name.size() <= 4 || (!name.starts_with("get_") && !name.starts_with("set_")))
        return false;
    return refNames.contains(std::string(name.substr(4)));
}

bool IsExemptMethodName(std::string_view name, std::string_view containerName) noexcept
{
    return name == containerName || name.starts_with('~') || name.starts_with("op") || name.starts_with("get_") ||
           name.starts_with("set_");
}

bool IsClassInherited(std::string_view className, const UnusedCheckContext& uCtx)
{
    return uCtx.inheritedOrMixedInClasses.contains(className);
}

bool IsMethodOverriddenInDerived(const Symbol& sym, const UnusedCheckContext& uCtx)
{
    if (sym.containerName.empty())
        return false;
    auto it = uCtx.derivedClassesByBase.find(sym.containerName);
    if (it == uCtx.derivedClassesByBase.end())
        return false;
    for (const auto& derived : it->second)
    {
        if (uCtx.ruleIndex.Members(derived).methodNames.contains(sym.name))
        {
            return true;
        }
    }
    return false;
}

bool ShouldSkipFunction(const Symbol& sym, const SemanticAnalysisRequest& req, const UnusedCheckContext& uCtx)
{
    if (sym.isSynthesized)
        return true;
    if (std::holds_alternative<FunctionSignature>(sym.signature) && sym.GetFunction().modifiers.isAbstract)
        return true;
    if (!sym.containerName.empty())
    {
        if (const auto contSym = req.symbolTable.FindFirstSymbol(sym.containerName))
        {
            if (contSym->type == SymbolType::Interface)
                return true;
        }
    }
    if (IsIgnoredFunction(sym, req, uCtx.rules) || uCtx.reflections.contains(sym.name) ||
        uCtx.reflections.contains(sym.qualifiedName))
        return true;
    if (IsExemptMethodName(sym.name, sym.containerName) || IsPropertyAccessorUsed(sym.name, uCtx.refNames))
        return true;
    if (IsLifecycleOrOverride(sym, req.symbolTable, uCtx.rules) || IsMethodOverriddenInDerived(sym, uCtx))
        return true;
    return false;
}

void CheckUnusedFunctions(const SemanticAnalysisRequest& req, const UnusedCheckContext& uCtx, DiagnosticContext& ctx)
{
    if (!uCtx.rules.checkFunctions && !uCtx.rules.checkMembers)
        return;

    req.symbolTable.ForEachSymbolInFile(
        req.fileUri,
        [&]([[maybe_unused]] const std::string& qName, const std::vector<Symbol>& syms)
        {
            for (const auto& sym : syms)
            {
                if (sym.type != SymbolType::Function || ShouldSkipFunction(sym, req, uCtx) ||
                    uCtx.refNames.contains(sym.name))
                    continue;

                if (sym.containerName.empty() || sym.containerKind == ContainerKind::Namespace)
                {
                    if (uCtx.rules.checkFunctions)
                    {
                        ctx.Emit(sym, diagnostics::codes::UnusedFunction, sym.name, DiagnosticSeverity::Warning);
                    }
                }
                else
                {
                    if (uCtx.rules.checkFunctions || uCtx.rules.checkMembers)
                    {
                        ctx.Emit(sym, diagnostics::codes::UnusedMethod, sym.name, DiagnosticSeverity::Warning);
                    }
                }
            }
        });
}

void CheckUnusedClasses(const SemanticAnalysisRequest& req, const UnusedCheckContext& uCtx, DiagnosticContext& ctx)
{
    if (!uCtx.rules.checkClasses)
        return;

    req.symbolTable.ForEachSymbolInFile(
        req.fileUri,
        [&]([[maybe_unused]] const std::string& qName, const std::vector<Symbol>& syms)
        {
            for (const auto& sym : syms)
            {
                if (sym.type == SymbolType::Class && !IsEngineEntity(sym.name, req.symbolTable, uCtx.rules) &&
                    !IsIgnoredByDiagnosticsConfig(sym, req) && !uCtx.reflections.contains(sym.name) &&
                    !uCtx.reflections.contains(sym.qualifiedName) && !uCtx.refNames.contains(sym.name) &&
                    !IsClassInherited(sym.name, uCtx))
                {
                    ctx.Emit(sym, diagnostics::codes::UnusedClass, sym.name, DiagnosticSeverity::Warning);
                }
            }
        });
}

config::UnusedSymbolRules ResolveEffectiveRules(const SemanticAnalysisRequest& request)
{
    config::UnusedSymbolRules rules =
        request.engineRules ? request.engineRules->unusedRules : config::UnusedSymbolRules{};
    if (!rules.enabled)
    {
        rules.checkMembers = request.diagnostics && request.diagnostics->reportUnusedMembers;
        rules.checkGlobals = request.diagnostics && request.diagnostics->reportUnusedGlobalVariables;
        rules.checkFunctions = request.diagnostics && request.diagnostics->reportUnusedFunctions;
        rules.checkClasses = request.diagnostics && request.diagnostics->reportUnusedClasses;
        return rules;
    }
    if (request.diagnostics)
    {
        const auto& diag = *request.diagnostics;
        if (diag.reportUnusedGlobalVariablesExplicit)
        {
            rules.checkGlobals = diag.reportUnusedGlobalVariables;
        }
        if (diag.reportUnusedFunctionsExplicit)
        {
            rules.checkFunctions = diag.reportUnusedFunctions;
        }
        if (diag.reportUnusedClassesExplicit)
        {
            rules.checkClasses = diag.reportUnusedClasses;
        }
        if (diag.reportUnusedMembersExplicit)
        {
            rules.checkMembers = diag.reportUnusedMembers;
        }
    }
    return rules;
}

} // namespace

void CheckUnusedSymbols(const SemanticAnalysisRequest& request, DiagnosticContext& ctx)
{
    if (!request.scopeRoot || utils::IsPredefinedFile(request.fileUri, request.predefinedFileExtension))
        return;

    config::UnusedSymbolRules rules = ResolveEffectiveRules(request);

    ankerl::unordered_dense::set<uint64_t> defLocs;
    CollectDefLocations(request.scopeRoot.get(), defLocs);

    ankerl::unordered_dense::set<std::string> refNames;
    CollectDocReferences(request.scopeRoot.get(), defLocs, refNames, request.sourceCode,
                         rules.baseClassDelegateProperties);
    CollectCrossFileReferences(request, refNames);

    ankerl::unordered_dense::set<const LocalDefinition*> usedLocals;
    CollectUsedLocals(request.scopeRoot.get(), usedLocals);

    const auto& ruleIndex = request.GetRuleIndex();

    ankerl::unordered_dense::set<std::string, TransparentStringHash, std::equal_to<>> inheritedOrMixedInClasses;
    ankerl::unordered_dense::map<std::string, std::vector<std::string>, TransparentStringHash, std::equal_to<>>
        derivedClassesByBase;

    for (const auto& [base, derivedList] : ruleIndex.derivedByBase)
    {
        inheritedOrMixedInClasses.insert(base);
        auto& targets = derivedClassesByBase[base];
        for (const auto& d : derivedList)
        {
            targets.push_back(d.qualifiedName.empty() ? d.name : d.qualifiedName);
            if (!d.name.empty() && d.name != d.qualifiedName)
            {
                targets.push_back(d.name);
            }
        }
    }
    for (const auto& [mixin, hostList] : ruleIndex.hostClassesByMixin)
    {
        inheritedOrMixedInClasses.insert(mixin);
        auto& targets = derivedClassesByBase[mixin];
        for (const auto& h : hostList)
        {
            targets.push_back(h.qualifiedName.empty() ? h.name : h.qualifiedName);
            if (!h.name.empty() && h.name != h.qualifiedName)
            {
                targets.push_back(h.name);
            }
        }
    }

    const auto reflections = CollectStringReflection(request);
    const UnusedCheckContext uCtx{
        refNames, usedLocals, reflections, rules, inheritedOrMixedInClasses, derivedClassesByBase, ruleIndex};

    CheckLocalsAndFields(request.scopeRoot.get(), uCtx, ctx);
    CheckUnusedGlobals(request, uCtx, ctx);
    CheckUnusedFunctions(request, uCtx, ctx);
    CheckUnusedClasses(request, uCtx, ctx);
}

} // namespace angel_lsp::analysis::rules
