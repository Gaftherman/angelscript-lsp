#include "analysis/rules/UnusedSymbolChecker.h"

#include "analysis/ASTUtils.h"
#include "analysis/DiagnosticCodes.h"
#include "analysis/DiagnosticContext.h"
#include "analysis/NodeIndex.h"
#include "analysis/ScopeTree.h"
#include "analysis/SemanticHelpers.h"
#include "analysis/SymbolTable.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarNames.h"
#include "utils/Utils.h"

#include <algorithm>
#include <ankerl/unordered_dense.h>
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
                          ankerl::unordered_dense::set<std::string>& refNames, int depth = 0)
{
    if (!scope || depth > k_maxAstDepth)
        return;
    for (const auto& ref : scope->references)
    {
        if (!defLocs.contains(PackLoc(ref.startLine, ref.startCharacter)))
            refNames.insert(ref.name);
    }
    for (const auto& child : scope->children)
        CollectDocReferences(child.get(), defLocs, refNames, depth + 1);
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
                CollectDocReferences(root.get(), defs, refNames);
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

void RegisterReflectedSymbol(const std::string& str, const SemanticAnalysisRequest& req,
                             ankerl::unordered_dense::set<std::string>& outReflections)
{
    if (str.empty())
        return;

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

ankerl::unordered_dense::set<std::string> CollectStringReflection(const SemanticAnalysisRequest& req)
{
    ankerl::unordered_dense::set<std::string> result;
    if (!req.nodeIndex || !req.engineRules)
        return result;

    const auto& callees = req.engineRules->unusedRules.stringReflectionCallees;
    if (callees.empty())
        return result;

    for (TSNode callNode : req.nodeIndex->Nodes(parser::nodes::CallExpression))
    {
        TSNode funcNode = ts_node_child_by_field_name(callNode, "function", 8);
        if (ts_node_is_null(funcNode))
            continue;

        std::string funcText = parser::GetNodeText(funcNode, req.sourceCode);
        for (const auto& c : callees)
        {
            if (!MatchCallee(funcText, c))
                continue;

            auto callArgs = ExtractCallArguments(callNode, req.sourceCode);
            for (size_t idx : c.argIndices)
            {
                if (idx < callArgs.size())
                    RegisterReflectedSymbol(ExtractStringValue(callArgs[idx].exprNode, req), req, result);
            }
        }
    }
    return result;
}

bool IsEngineEntity(const std::string& className, const SemanticAnalysisRequest& request)
{
    if (!request.engineRules)
        return false;
    const auto& ignored = request.engineRules->unusedRules.ignoredBaseClasses;
    if (ignored.empty())
        return false;
    if (std::find(ignored.begin(), ignored.end(), className) != ignored.end())
        return true;
    const auto hierarchy = GetInheritedTypeHierarchy(className, request.symbolTable);
    return std::any_of(hierarchy.begin(), hierarchy.end(), [&ignored](const std::string& b)
                       { return std::find(ignored.begin(), ignored.end(), b) != ignored.end(); });
}

bool IsLifecycleOrOverride(const Symbol& sym, const SemanticAnalysisRequest& req)
{
    if (sym.containerName.empty())
        return false;

    const auto hierarchy = GetInheritedTypeHierarchy(sym.containerName, req.symbolTable);
    for (const auto& ancestor : hierarchy)
    {
        if (ancestor != sym.containerName && req.symbolTable.FindMemberSymbolPtr(ancestor, sym.name) != nullptr)
            return true;
    }

    if (!req.engineRules)
        return false;

    const auto& unusedRules = req.engineRules->unusedRules;
    for (const auto& ancestor : hierarchy)
    {
        auto it = unusedRules.baseClassLifecycleMethods.find(ancestor);
        if (it != unusedRules.baseClassLifecycleMethods.end() &&
            std::find(it->second.begin(), it->second.end(), sym.name) != it->second.end())
            return true;
    }

    if (IsEngineEntity(sym.containerName, req))
    {
        const auto& methods = unusedRules.lifecycleMethods;
        if (std::find(methods.begin(), methods.end(), sym.name) != methods.end())
            return true;
    }

    return false;
}

bool IsIgnoredFunction(const std::string& name, const SemanticAnalysisRequest& request)
{
    if (!request.engineRules)
        return false;
    const auto& rules = request.engineRules->unusedRules;
    if (std::find(rules.ignoredGlobalFunctions.begin(), rules.ignoredGlobalFunctions.end(), name) !=
        rules.ignoredGlobalFunctions.end())
        return true;
    if (!rules.ignoredGlobalFunctionRegex.empty())
    {
        try
        {
            return std::regex_match(name, std::regex(rules.ignoredGlobalFunctionRegex));
        }
        catch (const std::regex_error&)
        {
        }
    }
    return false;
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

void CheckUnusedGlobals(const SemanticAnalysisRequest& req, const UnusedCheckContext& uCtx, DiagnosticContext& ctx)
{
    if (!uCtx.rules.checkGlobals)
        return;

    req.symbolTable.ForEachSymbolInFile(
        req.fileUri,
        [&]([[maybe_unused]] const std::string& qName, const std::vector<Symbol>& syms)
        {
            for (const auto& sym : syms)
            {
                if (sym.type == SymbolType::Variable && sym.containerName.empty() &&
                    !uCtx.refNames.contains(sym.name) && !uCtx.reflections.contains(sym.name) &&
                    !uCtx.reflections.contains(sym.qualifiedName))
                {
                    ctx.Emit(sym, diagnostics::codes::UnusedGlobalVariable, sym.name, DiagnosticSeverity::Warning);
                }
            }
        });
}

bool ShouldSkipFunction(const Symbol& sym, const SemanticAnalysisRequest& req,
                        const ankerl::unordered_dense::set<std::string>& reflections)
{
    if (IsIgnoredFunction(sym.name, req) || reflections.contains(sym.name) || reflections.contains(sym.qualifiedName) ||
        sym.name == sym.containerName || sym.name.rfind('~', 0) == 0 || sym.name.rfind("op", 0) == 0)
        return true;
    return IsLifecycleOrOverride(sym, req);
}

void CheckUnusedFunctions(const SemanticAnalysisRequest& req, const UnusedCheckContext& uCtx, DiagnosticContext& ctx)
{
    if (!uCtx.rules.checkFunctions)
        return;

    req.symbolTable.ForEachSymbolInFile(
        req.fileUri,
        [&]([[maybe_unused]] const std::string& qName, const std::vector<Symbol>& syms)
        {
            for (const auto& sym : syms)
            {
                if (sym.type != SymbolType::Function || ShouldSkipFunction(sym, req, uCtx.reflections) ||
                    uCtx.refNames.contains(sym.name))
                    continue;

                if (sym.containerName.empty())
                    ctx.Emit(sym, diagnostics::codes::UnusedFunction, sym.name, DiagnosticSeverity::Warning);
                else if (sym.GetFunction().modifiers.access == AccessModifier::Private)
                    ctx.Emit(sym, diagnostics::codes::UnusedMethod, sym.name, DiagnosticSeverity::Warning);
            }
        });
}

bool IsClassInherited(const std::string& className, const SymbolTable& table)
{
    bool inherited = false;
    table.ForEachSymbol(
        [&]([[maybe_unused]] const std::string& qName, const std::vector<Symbol>& syms)
        {
            if (inherited)
                return;
            for (const auto& s : syms)
            {
                if (s.type == SymbolType::Class)
                {
                    const auto& c = s.GetClass();
                    if (std::find(c.bases.begin(), c.bases.end(), className) != c.bases.end() ||
                        std::find(c.includedMixins.begin(), c.includedMixins.end(), className) !=
                            c.includedMixins.end())
                    {
                        inherited = true;
                        return;
                    }
                }
            }
        });
    return inherited;
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
                if (sym.type == SymbolType::Class && !IsEngineEntity(sym.name, req) &&
                    !uCtx.reflections.contains(sym.name) && !uCtx.reflections.contains(sym.qualifiedName) &&
                    !uCtx.refNames.contains(sym.name) && !IsClassInherited(sym.name, req.symbolTable))
                {
                    ctx.Emit(sym, diagnostics::codes::UnusedClass, sym.name, DiagnosticSeverity::Warning);
                }
            }
        });
}

} // namespace

void CheckUnusedSymbols(const SemanticAnalysisRequest& request, DiagnosticContext& ctx)
{
    if (!request.scopeRoot || utils::IsPredefinedFile(request.fileUri, request.predefinedFileExtension))
        return;

    config::UnusedSymbolRules rules =
        request.engineRules ? request.engineRules->unusedRules : config::UnusedSymbolRules{};
    if (!rules.enabled)
    {
        rules.checkMembers = false;
        rules.checkGlobals = false;
        rules.checkFunctions = false;
        rules.checkClasses = false;
    }

    ankerl::unordered_dense::set<uint64_t> defLocs;
    CollectDefLocations(request.scopeRoot.get(), defLocs);

    ankerl::unordered_dense::set<std::string> refNames;
    CollectDocReferences(request.scopeRoot.get(), defLocs, refNames);
    CollectCrossFileReferences(request, refNames);

    ankerl::unordered_dense::set<const LocalDefinition*> usedLocals;
    CollectUsedLocals(request.scopeRoot.get(), usedLocals);

    const auto reflections = CollectStringReflection(request);
    const UnusedCheckContext uCtx{refNames, usedLocals, reflections, rules};

    CheckLocalsAndFields(request.scopeRoot.get(), uCtx, ctx);
    CheckUnusedGlobals(request, uCtx, ctx);
    CheckUnusedFunctions(request, uCtx, ctx);
    CheckUnusedClasses(request, uCtx, ctx);
}

} // namespace angel_lsp::analysis::rules
