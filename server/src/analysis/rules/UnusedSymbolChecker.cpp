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

ankerl::unordered_dense::set<std::string> CollectStringReflection(const SemanticAnalysisRequest& request)
{
    ankerl::unordered_dense::set<std::string> result;
    if (!request.nodeIndex)
        return result;
    for (TSNode strNode : request.nodeIndex->Nodes(parser::nodes::StringLiteral))
    {
        std::string raw = parser::GetNodeText(strNode, request.sourceCode);
        if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"')
            raw = raw.substr(1, raw.size() - 2);
        if (!raw.empty())
        {
            result.insert(raw);
            const auto pos = raw.rfind("::");
            if (pos != std::string::npos && pos + 2 < raw.size())
                result.insert(raw.substr(pos + 2));
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

bool IsLifecycleMethod(const std::string& methodName, const std::string& className,
                       const SemanticAnalysisRequest& request)
{
    if (!request.engineRules || !IsEngineEntity(className, request))
        return false;
    const auto& methods = request.engineRules->unusedRules.lifecycleMethods;
    return std::find(methods.begin(), methods.end(), methodName) != methods.end();
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
                    !uCtx.refNames.contains(sym.name) && !uCtx.reflections.contains(sym.name))
                {
                    ctx.Emit(sym, diagnostics::codes::UnusedGlobalVariable, sym.name, DiagnosticSeverity::Warning);
                }
            }
        });
}

bool ShouldSkipFunction(const Symbol& sym, const SemanticAnalysisRequest& req,
                        const ankerl::unordered_dense::set<std::string>& reflections)
{
    if (IsIgnoredFunction(sym.name, req) || reflections.contains(sym.name) || sym.name == sym.containerName ||
        sym.name.rfind('~', 0) == 0 || sym.name.rfind("op", 0) == 0)
        return true;
    return !sym.containerName.empty() && IsLifecycleMethod(sym.name, sym.containerName, req);
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
