#include "analysis/documentation/DocComment.h"
#include "analysis/documentation/HoverMarkdown.h"
#include "analysis/resolution/CallableAndLambdaResolver.h"
#include "analysis/resolution/HoverResolution.h"
#include "analysis/resolution/ScopeSymbolLookup.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/types/TypeSanitization.h"
#include "i18n/i18n.h"
#include "parser/Primitives.h"

#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{
void FormatParameterHover(const LocalDefinition& def, std::string_view typeName, const SymbolTable& symbolTable,
                          std::string& md)
{
    md += "(parameter) ";
    const std::string_view effectiveType = !typeName.empty() ? typeName : std::string_view(def.typeName);
    if (!effectiveType.empty())
    {
        md += effectiveType;
        md += " ";
    }
    md += def.name;
    if (!def.defaultValue.empty())
    {
        md += " = ";
        md += def.defaultValue;
    }

    TryAppendFuncdefSignature(typeName, symbolTable, md);
}

std::string FormatNamedArgHover(const std::vector<Symbol>& candidates, std::string_view nodeText,
                                const angel_lsp::i18n::I18n* i18n)
{
    for (const auto& sym : candidates)
    {
        if (sym.type != SymbolType::Function || !std::holds_alternative<FunctionSignature>(sym.signature))
        {
            continue;
        }
        for (const auto& param : sym.GetFunction().parameters)
        {
            if (param.name != nodeText)
            {
                continue;
            }
            std::string md = "```angelscript\n(parameter) " + param.typeName + " " + param.name;
            if (!param.defaultValue.empty())
            {
                md += " = " + param.defaultValue;
            }
            md += "\n```\n" +
                  angel_lsp::i18n::FormatMessage(i18n, "hover-named-arg-parameter-of", "Parameter of `{}`", sym.name);
            return md;
        }
    }
    return "";
}

void FormatGlobalVariableHover(const LocalDefinition& def, std::string_view typeName, const Symbol* globalSym,
                               std::string& md)
{
    if (globalSym)
    {
        md += FormatVariableSignature(*globalSym, "(global variable) ");
        return;
    }
    md += "(global variable) ";
    if (!typeName.empty())
    {
        md += typeName;
        md += " ";
    }
    md += def.name;
    if (!def.defaultValue.empty())
    {
        md += " = ";
        md += def.defaultValue;
    }
}

std::string FormatParameterAstHover(std::string_view rawDecl)
{
    return "```angelscript\n(parameter) " + CollapseWhitespace(rawDecl) + "\n```";
}

std::string FormatScopeFallbackHover(const LocalDefinition& def, std::string_view sourceCode)
{
    std::ostringstream oss;
    oss << "```angelscript\n";
    if (def.kind == LocalDefinitionKind::Field)
    {
        oss << "(field) ";
    }
    if (!def.typeName.empty())
    {
        oss << def.typeName << " ";
    }
    oss << def.name << "\n```";
    std::string doc = ExtractDocComment(sourceCode, def.startLine);
    if (!doc.empty())
    {
        oss << "\n\n---\n\n" << doc;
    }
    return oss.str();
}

static const Scope* FindDefinitionScope(const Scope* scope, const Scope* rootScope, const LocalDefinition& def)
{
    ScopeTraversalGuard cycleGuard;
    for (const Scope* s = scope; s != nullptr; s = s->parent)
    {
        if (!cycleGuard.CheckAndInsert(s))
        {
            break;
        }
        for (const auto& d : s->definitions)
        {
            if (d.name == def.name && d.startLine == def.startLine && d.startCharacter == def.startCharacter)
            {
                return s;
            }
        }
    }
    return FindScopeDeclaringDefinition(rootScope, def);
}

static std::optional<Symbol> FindMatchingGlobalSymbol(const LocalDefinition& def, const SymbolTable& symbolTable,
                                                      std::string_view uri, std::string_view sourceCode,
                                                      const NodeIndex* nodeIndex)
{
    auto containers =
        nodeIndex ? nodeIndex->CollectEnclosingContainers(def.startLine, sourceCode) : std::vector<ContainerInfo>{};
    auto candidates = FindSymbolsInScope(symbolTable, containers, def.name);
    for (const auto& cand : candidates)
    {
        if (cand.type == SymbolType::Variable && cand.fileUri == uri)
        {
            return cand;
        }
    }
    for (const auto& cand : candidates)
    {
        if (cand.type == SymbolType::Variable)
        {
            return cand;
        }
    }
    auto exactCandidates = symbolTable.FindSymbolsPtr(def.name);
    if (exactCandidates)
    {
        for (const auto& cand : *exactCandidates)
        {
            if (cand.type == SymbolType::Variable && cand.fileUri == uri)
            {
                return cand;
            }
        }
    }
    return std::nullopt;
}

std::string FormatLocalDefinitionHover(const LocalDefinitionHoverQuery& query, const ExpressionTypeContext& exprCtx,
                                       const HoverDocLookupContext& docCtx)
{
    const std::string deducedType = DeduceLocalDefinitionHoverType(query.def, query.hoverNode, exprCtx);
    const std::string_view typeName = deducedType;

    std::string md;
    md.reserve(128);
    md += "```angelscript\n";

    if (query.def.kind == LocalDefinitionKind::Parameter)
    {
        FormatParameterHover(query.def, typeName, exprCtx.symbolTable, md);
    }
    else
    {
        const Scope* declaringScope = FindDefinitionScope(query.scope, query.rootScope, query.def);
        bool isInsideFunction = false;
        ScopeTraversalGuard cycleGuard;
        for (const Scope* s = declaringScope; s != nullptr; s = s->parent)
        {
            if (!cycleGuard.CheckAndInsert(s))
            {
                break;
            }
            if (s->isFunctionScope)
            {
                isInsideFunction = true;
                break;
            }
        }

        if (!isInsideFunction)
        {
            auto globalSym = FindMatchingGlobalSymbol(query.def, exprCtx.symbolTable, exprCtx.uri, exprCtx.sourceCode,
                                                      exprCtx.nodeIndex);
            FormatGlobalVariableHover(query.def, typeName, globalSym ? &*globalSym : nullptr, md);
        }
        else
        {
            md += "(local variable) ";
            if (!typeName.empty())
            {
                md += typeName;
                md += " ";
            }
            md += query.def.name;
            if (IsSimpleLiteralExpression(query.def.defaultValue))
            {
                md += " = ";
                md += query.def.defaultValue;
            }
        }

        const size_t prevLen = md.size();
        TryAppendFuncdefSignature(typeName, exprCtx.symbolTable, md);
        if (md.size() == prevLen && query.isDirectInit)
        {
            std::string ctorSig =
                ResolveConstructorSignature(query.hoverNode.startByte, query.hoverNode.endByte, typeName, exprCtx);
            if (!ctorSig.empty())
            {
                md += "\n";
                md += ctorSig;
            }
        }
    }
    md += "\n```";

    std::string doc = ExtractDocComment(docCtx.sourceCode, query.def.startLine);
    if (doc.empty() && query.isDirectInit)
    {
        if (auto ctorSym =
                ResolveConstructorForDeclarator(query.hoverNode.startByte, query.hoverNode.endByte, typeName, exprCtx))
        {
            doc = DocCommentForSymbol(docCtx, *ctorSym);
        }
    }
    if (doc.empty())
    {
        const std::string_view cleanType = CleanBaseTypeView(typeName);
        if (!cleanType.empty() && !parser::primitives::IsPrimitive(cleanType))
        {
            if (auto funcdefSym = FindFuncdefSymbol(cleanType, *docCtx.symbolTable))
            {
                doc = DocCommentForSymbol(docCtx, *funcdefSym);
            }
        }
    }
    if (!doc.empty())
    {
        md += "\n\n---\n\n";
        md += doc;
    }

    return md;
}
} // namespace angel_lsp::analysis
