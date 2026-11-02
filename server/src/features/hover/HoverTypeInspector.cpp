#include "features/hover/HoverTypeInspector.h"

#include "analysis/documentation/SignatureFormatter.h"
#include "analysis/resolution/CallableAndLambdaResolver.h"
#include "analysis/resolution/ExpressionTypeResolver.h"
#include "analysis/resolution/ScopeSymbolLookup.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/types/ExpressionTypeCache.h"
#include "analysis/types/TypeSanitization.h"
#include "features/hover/HoverDocumentationRenderer.h"
#include "features/hover/HoverSymbolFormatter.h"
#include "i18n/i18n.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"
#include "parser/Primitives.h"
#include "utils/Constants.h"
#include "utils/Utils.h"

#include <algorithm>
#include <filesystem>
#include <iomanip>
#include <spdlog/fmt/fmt.h>
#include <sstream>
#include <vector>

namespace angel_lsp::features
{
static std::string_view UnquoteStringLiteral(std::string_view txt)
{
    return parser::UnquoteStringView(txt);
}

static std::string FormatResolvedAssetMarkdown(std::string_view unquoted, const std::string& resolved, bool showLength,
                                               const angel_lsp::i18n::I18n* i18n = nullptr)
{
    std::error_code ec;
    auto fsize = std::filesystem::file_size(resolved, ec);
    std::string sizeStr = ec ? "Unknown" : FormatFileSize(fsize);
    std::string normalizedPath = resolved;
    std::replace(normalizedPath.begin(), normalizedPath.end(), '\\', '/');

    std::string statusBullet =
        angel_lsp::i18n::FormatMessage(i18n, "hover-asset-status-exists", "- **Status**: Exists");
    std::string sizeBullet = angel_lsp::i18n::FormatMessage(i18n, "hover-asset-size", "- **Size**: {}", sizeStr);
    std::string pathBullet =
        angel_lsp::i18n::FormatMessage(i18n, "hover-asset-path", "- **Path**: `{}`", normalizedPath);

    std::string md =
        "*(asset)* `" + std::string(unquoted) + "`\n\n" + statusBullet + "\n" + sizeBullet + "\n" + pathBullet;
    if (showLength)
    {
        md += "\n" + angel_lsp::i18n::FormatMessage(i18n, "hover-string-length", "- **Length**: {} characters",
                                                    unquoted.size());
    }
    return md;
}

static TSNode FindTopmostBinaryPlus(TSNode node)
{
    const auto& gs = parser::GetGrammarSymbols();
    TSNode current = node;
    parser::ForEachAncestor(parser::GetParent(node),
                            [&](TSNode parent) -> bool
                            {
                                if (ts_node_symbol(parent) == gs.symBinaryExpression)
                                {
                                    TSNode opNode = parser::GetChildByField(parent, gs.fieldOperator);
                                    if (!ts_node_is_null(opNode) && ts_node_symbol(opNode) == gs.tokPlus)
                                    {
                                        current = parent;
                                        return true;
                                    }
                                }
                                return false;
                            });
    return current;
}

static std::optional<std::string> ResolveIdentifierDefaultValue(const std::string& name, TSNode node,
                                                                const HoverRequest& request,
                                                                const analysis::Scope* scope)
{
    if (scope)
    {
        const analysis::LocalDefinition* def = analysis::ResolveInScopeAt(scope, name, ts_node_start_byte(node));
        if (def && !def->defaultValue.empty())
        {
            return std::string(UnquoteStringLiteral(def->defaultValue));
        }
    }
    auto candidates = analysis::FindSymbolsInScope(name, node, request.sourceCode, request.symbolTable);
    for (const auto& sym : candidates)
    {
        if (sym.type == analysis::SymbolType::Variable && !sym.GetVariable().defaultValue.empty())
        {
            return std::string(UnquoteStringLiteral(sym.GetVariable().defaultValue));
        }
    }
    if (const auto globals = request.symbolTable.FindSymbolsPtr(name))
    {
        for (const auto& sym : *globals)
        {
            if (sym.type == analysis::SymbolType::Variable && !sym.GetVariable().defaultValue.empty())
            {
                return std::string(UnquoteStringLiteral(sym.GetVariable().defaultValue));
            }
        }
    }
    return std::nullopt;
}

static std::optional<std::string> EvaluateConstantStringExpr(TSNode node, const HoverRequest& request,
                                                             const analysis::Scope* scope)
{
    if (ts_node_is_null(node))
    {
        return std::nullopt;
    }

    const auto& gs = parser::GetGrammarSymbols();
    std::string result;
    std::vector<TSNode> stack{node};
    while (!stack.empty())
    {
        TSNode current = stack.back();
        stack.pop_back();
        if (ts_node_is_null(current))
        {
            return std::nullopt;
        }

        const TSSymbol type = ts_node_symbol(current);
        if (type == gs.symStringLiteral)
        {
            std::string text = parser::GetNodeText(current, request.sourceCode);
            result += UnquoteStringLiteral(text);
        }
        else if (type == gs.symBinaryExpression)
        {
            TSNode opNode = parser::GetChildByField(current, gs.fieldOperator);
            if (ts_node_is_null(opNode) || ts_node_symbol(opNode) != gs.tokPlus)
            {
                return std::nullopt;
            }
            TSNode left = parser::GetChildByField(current, gs.fieldLeft);
            TSNode right = parser::GetChildByField(current, gs.fieldRight);
            if (ts_node_is_null(left) || ts_node_is_null(right))
            {
                return std::nullopt;
            }
            stack.push_back(right);
            stack.push_back(left);
        }
        else if (type == gs.symIdentifier || type == gs.symScopedIdentifier)
        {
            std::string name = parser::GetNodeText(current, request.sourceCode);
            auto val = ResolveIdentifierDefaultValue(name, current, request, scope);
            if (!val.has_value())
            {
                return std::nullopt;
            }
            result += *val;
        }
        else
        {
            return std::nullopt;
        }
    }
    return result;
}

static bool IsPathLikeString(std::string_view str)
{
    if (str.find('/') != std::string_view::npos || str.find('\\') != std::string_view::npos)
    {
        return true;
    }
    auto dot = str.rfind('.');
    if (dot != std::string_view::npos && dot + 1 < str.size())
    {
        std::string_view ext = str.substr(dot + 1);
        if (ext.size() >= 2 && ext.size() <= 4)
        {
            return true;
        }
    }
    return false;
}

static bool IsStringOrBinaryLiteral(const HoverTarget& target)
{
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol type = ts_node_symbol(target.node);
    if (type == gs.symStringLiteral || type == gs.symBinaryExpression)
    {
        return target.text.size() >= 2 && ((target.text.front() == '"' && target.text.back() == '"') ||
                                           (target.text.front() == '\'' && target.text.back() == '\''));
    }
    return false;
}

static TSNode FindEnclosingLambda(TSNode node)
{
    return parser::FindAncestorWithSymbol(node, parser::GetGrammarSymbols().symLambdaExpression);
}

static void FormatLambdaParameters(TSNode lambdaNode, std::string_view sourceCode, std::ostringstream& oss)
{
    oss << "(anonymous function) function";
    TSNode paramListNode = ts_node_child_by_field_name(lambdaNode, "parameters", 10);
    if (ts_node_is_null(paramListNode))
    {
        parser::ForEachNamedChildNodeUntil(lambdaNode,
                                           [&](TSNode child)
                                           {
                                               if (ts_node_symbol(child) ==
                                                   parser::GetGrammarSymbols().symParameterList)
                                               {
                                                   paramListNode = child;
                                                   return false;
                                               }
                                               return true;
                                           });
    }
    if (!ts_node_is_null(paramListNode))
    {
        uint32_t sb = ts_node_start_byte(paramListNode);
        uint32_t eb = ts_node_end_byte(paramListNode);
        if (sb < eb && eb <= sourceCode.size())
        {
            oss << sourceCode.substr(sb, eb - sb);
            return;
        }
    }
    oss << "()";
}

static TSNode FindEnclosingAutoDeclaration(TSNode node)
{
    const auto& gs = parser::GetGrammarSymbols();
    const std::array<TSSymbol, 2> declSyms = {gs.symVariableDeclaration, gs.symForeachStatement};
    return parser::FindAncestorWithAnySymbol(node, declSyms);
}

std::string FormatPrimitiveDoc(std::string_view typeName, const angel_lsp::i18n::I18n* i18n)
{
    auto info = parser::primitives::GetPrimitiveDocInfo(typeName);
    if (!info)
    {
        return {};
    }
    std::string key = "hover-primitive-desc-";
    key += typeName;
    std::string desc = angel_lsp::i18n::FormatMessage(i18n, key, std::string(info->description));
    if (info->range.empty())
    {
        return desc;
    }
    if (typeName == "bool")
    {
        return desc + "\n\n" +
               angel_lsp::i18n::FormatMessage(i18n, "hover-primitive-values", "Values: `{}`", info->range);
    }
    return desc + "\n\n" + angel_lsp::i18n::FormatMessage(i18n, "hover-primitive-range", "Range: `{}`", info->range);
}

std::optional<lsp::Hover> TryHoverPrimitiveType(std::string_view nodeText, const lsp::Range& range,
                                                const angel_lsp::i18n::I18n* i18n, HoverProfiler& profiler)
{
    if (!analysis::IsPrimitiveTypeName(nodeText) || nodeText == "auto")
    {
        return std::nullopt;
    }
    profiler.nodeType = "primitive_type";
    if (profiler.IsActive())
    {
        profiler.pathText = std::string(nodeText);
        profiler.symbolName = std::string(nodeText);
    }
    utils::HighResTimer fmtTimer;
    std::string md = "```angelscript\n(primitive type) " + std::string(nodeText) + "\n```";
    std::string doc = FormatPrimitiveDoc(nodeText, i18n);
    if (!doc.empty())
    {
        md += "\n\n" + doc;
    }
    profiler.fmtMs += fmtTimer.ElapsedMs();
    return lsp::Hover{lsp::MarkupContent{lsp::MarkupKindEnum(lsp::MarkupKind::Markdown), std::move(md)}, range};
}

std::optional<lsp::Hover> TryHoverWildcardType(std::string_view nodeText, const lsp::Range& range,
                                               const angel_lsp::i18n::I18n* i18n, HoverProfiler& profiler)
{
    if (nodeText != "?")
    {
        return std::nullopt;
    }
    profiler.nodeType = "wildcard_type";
    if (profiler.IsActive())
    {
        profiler.pathText = "?";
        profiler.symbolName = "?";
    }
    utils::HighResTimer fmtTimer;
    std::string desc = angel_lsp::i18n::FormatMessage(
        i18n, "hover-wildcard-type-desc",
        "The variable type `?` represents any type. It is used in parameter declarations "
        "to accept arguments of any type by reference (`?& in`, `?& out`, `?& inout`).");
    std::string md = "```angelscript\n(variable type) ?\n```\n\n" + desc;
    profiler.fmtMs += fmtTimer.ElapsedMs();
    return lsp::Hover{lsp::MarkupContent{lsp::MarkupKindEnum(lsp::MarkupKind::Markdown), std::move(md)}, range};
}

std::optional<lsp::Hover> TryHoverThis(const HoverQueryContext& ctx)
{
    if (ctx.nodeText != "this")
    {
        return std::nullopt;
    }

    std::string className;
    if (ctx.vctx.isVirtual && !ctx.vctx.hostClass.empty())
    {
        className = ctx.vctx.hostClass;
    }
    else
    {
        auto containers = analysis::GetEnclosingContainers(ctx.node, ctx.request.sourceCode);
        for (const auto& c : containers)
        {
            if (c.kind == analysis::ContainerKind::Class || c.kind == analysis::ContainerKind::Interface)
            {
                className = c.name;
                break;
            }
        }
    }

    if (className.empty())
    {
        return std::nullopt;
    }

    ctx.profiler.nodeType = "this";
    if (ctx.profiler.IsActive())
    {
        ctx.profiler.pathText = "this";
        ctx.profiler.symbolName = className;
    }

    utils::HighResTimer fmtTimer;
    std::string md = fmt::format("```angelscript\n(self) {} {}\n```", className, ctx.nodeText);
    ctx.profiler.fmtMs += fmtTimer.ElapsedMs();
    return lsp::Hover{lsp::MarkupContent{lsp::MarkupKindEnum(lsp::MarkupKind::Markdown), std::move(md)}, ctx.range};
}

std::optional<lsp::Hover> TryHoverLambda(const HoverQueryContext& ctx)
{
    if (ctx.nodeText != "function")
    {
        return std::nullopt;
    }

    TSNode lambdaNode = FindEnclosingLambda(ctx.node);
    if (ts_node_is_null(lambdaNode))
    {
        return std::nullopt;
    }

    utils::HighResTimer fmtTimer;
    std::ostringstream oss;
    oss << "```angelscript\n";

    auto targetFuncdef = analysis::FuncdefTargetOfLambda(lambdaNode, ctx.request.symbolTable, ctx.request.sourceCode);
    if (targetFuncdef)
    {
        oss << "(anonymous function) -> " << targetFuncdef->name << "\n";
        oss << analysis::FormatFunctionDeclaration(*targetFuncdef);
    }
    else
    {
        FormatLambdaParameters(lambdaNode, ctx.request.sourceCode, oss);
    }
    oss << "\n```";

    if (targetFuncdef)
    {
        std::string doc = DocCommentForSymbol(ctx.request, *targetFuncdef);
        if (!doc.empty())
        {
            oss << "\n\n---\n\n" << doc;
        }
    }

    ctx.profiler.fmtMs += fmtTimer.ElapsedMs();
    return lsp::Hover{lsp::MarkupContent{lsp::MarkupKindEnum(lsp::MarkupKind::Markdown), oss.str()}, ctx.range};
}

std::string InferTypeFromAst(TSNode node, const std::string& sourceCode)
{
    const auto& gs = parser::GetGrammarSymbols();
    const std::array<TSSymbol, 2> declSyms = {gs.symParameter, gs.symVariableDeclaration};
    TSNode cur = parser::FindAncestorWithAnySymbol(node, declSyms);
    if (!ts_node_is_null(cur))
    {
        TSNode typeNode = parser::GetChildByField(cur, gs.fieldParamType);
        if (ts_node_is_null(typeNode))
        {
            typeNode = parser::GetChildByField(cur, gs.fieldVarType);
        }
        if (ts_node_is_null(typeNode))
        {
            typeNode = parser::GetChildByField(cur, gs.fieldType);
        }
        if (!ts_node_is_null(typeNode))
        {
            uint32_t tStart = ts_node_start_byte(typeNode);
            uint32_t tEnd = ts_node_end_byte(typeNode);
            if (tStart < sourceCode.size() && tEnd <= sourceCode.size() && tStart < tEnd)
            {
                return sourceCode.substr(tStart, tEnd - tStart);
            }
        }
    }
    return "";
}

std::string DeduceLocalDefinitionHoverType(const analysis::LocalDefinition& def, const HoverQueryContext& ctx)
{
    if (!def.typeName.empty() && analysis::CleanBaseTypeView(def.typeName) != "auto")
    {
        return def.typeName;
    }
    analysis::ExpressionTypeContext exprCtx(ctx.scope, ctx.request.symbolTable, ctx.request.sourceCode, ctx.request.uri,
                                            ctx.request.tree ? ts_tree_root_node(ctx.request.tree) : TSNode{});
    std::string inferred = analysis::ResolveLocalDefinitionType(def, exprCtx);
    if (inferred.empty())
    {
        inferred = InferTypeFromAst(ctx.node, ctx.request.sourceCode);
    }
    if (inferred.empty() && def.kind == analysis::LocalDefinitionKind::Parameter)
    {
        inferred = analysis::InferLambdaParamType(ctx.node, def.name, ctx.request.symbolTable, ctx.request.sourceCode);
    }
    return !inferred.empty() ? inferred : def.typeName;
}

std::string DeduceAutoDeclaratorType(TSNode declarator, const HoverQueryContext& ctx)
{
    TSNode nameNode = parser::GetChildByField(declarator, parser::GetGrammarSymbols().fieldName);
    if (!ts_node_is_null(nameNode) && ctx.scope)
    {
        std::string varName = parser::GetNodeText(nameNode, ctx.request.sourceCode);
        if (const auto* def = analysis::ResolveInScopeAt(ctx.scope, varName, ts_node_start_byte(nameNode)))
        {
            std::string deduced = DeduceLocalDefinitionHoverType(*def, ctx);
            if (!deduced.empty() && deduced != "auto")
            {
                return deduced;
            }
        }
    }

    TSNode valNode = parser::GetChildByField(declarator, parser::GetGrammarSymbols().fieldValue);
    if (ts_node_is_null(valNode))
    {
        valNode = parser::GetChildByField(declarator, parser::GetGrammarSymbols().fieldArguments);
    }
    if (!ts_node_is_null(valNode))
    {
        analysis::ExpressionTypeContext exprCtx(ctx.scope, ctx.request.symbolTable, ctx.request.sourceCode,
                                                ctx.request.uri,
                                                ctx.request.tree ? ts_tree_root_node(ctx.request.tree) : TSNode{});
        std::string res = analysis::ResolveExpressionType(valNode, exprCtx);
        if (!res.empty() && res != "void" && res != "null" && res != constants::k_initializerListTypeSentinel &&
            !analysis::ExpressionTypeCache::IsPoisonType(res))
        {
            return res;
        }
    }
    return "";
}

std::optional<lsp::Hover> TryHoverAutoType(const HoverQueryContext& ctx)
{
    if (ctx.nodeText != "auto" && ctx.nodeText != "auto@")
    {
        return std::nullopt;
    }

    TSNode declNode = FindEnclosingAutoDeclaration(ctx.node);
    if (ts_node_is_null(declNode))
    {
        return std::nullopt;
    }

    std::string deduced;
    if (ts_node_symbol(declNode) == parser::GetGrammarSymbols().symVariableDeclaration)
    {
        parser::ForEachChildNodeUntil(declNode,
                                      [&](TSNode child)
                                      {
                                          if (ts_node_symbol(child) ==
                                              parser::GetGrammarSymbols().symVariableDeclarator)
                                          {
                                              deduced = DeduceAutoDeclaratorType(child, ctx);
                                              return false;
                                          }
                                          return true;
                                      });
    }

    if (!deduced.empty() && deduced != "auto")
    {
        TSNode typeNode = parser::GetChildByField(declNode, parser::GetGrammarSymbols().fieldVarType);
        if (ts_node_is_null(typeNode))
        {
            typeNode = parser::GetChildByField(declNode, parser::GetGrammarSymbols().fieldType);
        }
        bool isHandleType = analysis::IsHandleTypeView(ctx.nodeText);
        if (!ts_node_is_null(typeNode) &&
            analysis::IsHandleTypeView(parser::GetNodeTextView(typeNode, ctx.request.sourceCode)))
        {
            isHandleType = true;
        }
        if (isHandleType && !analysis::IsHandleTypeView(deduced))
        {
            deduced += '@';
        }
        std::string md = "```angelscript\n" + std::string(isHandleType ? "auto@" : "auto") + ": " + deduced + "\n```";
        return lsp::Hover{lsp::MarkupContent{lsp::MarkupKindEnum(lsp::MarkupKind::Markdown), std::move(md)}, ctx.range};
    }

    return std::nullopt;
}

std::string FormatFileSize(uintmax_t bytes)
{
    std::ostringstream ss;
    if (bytes < 1024)
    {
        ss << bytes << " B";
    }
    else if (bytes < 1024 * 1024)
    {
        ss << std::fixed << std::setprecision(1) << (static_cast<double>(bytes) / 1024.0) << " KB";
    }
    else
    {
        ss << std::fixed << std::setprecision(1) << (static_cast<double>(bytes) / (1024.0 * 1024.0)) << " MB";
    }
    return ss.str();
}

std::string ResolveStringLiteralPath(std::string_view unquoted, const HoverRequest& request)
{
    std::string unquotedStr(unquoted);
    if (request.resolveInclude)
    {
        std::string resolved = request.resolveInclude(unquotedStr);
        if (!resolved.empty())
        {
            return resolved;
        }
    }

    std::error_code ec;
    std::filesystem::path docPath(utils::UriToPath(request.uri));
    std::filesystem::path candidate = docPath.parent_path() / unquotedStr;
    if (std::filesystem::exists(candidate, ec) && !std::filesystem::is_directory(candidate, ec))
    {
        return candidate.lexically_normal().string();
    }

    if (request.config)
    {
        for (const auto& searchDir : request.config->features.assetSearchPaths)
        {
            std::filesystem::path assetPath = std::filesystem::path(searchDir) / unquotedStr;
            if (std::filesystem::exists(assetPath, ec) && !std::filesystem::is_directory(assetPath, ec))
            {
                return assetPath.lexically_normal().string();
            }
        }
    }
    return "";
}

std::optional<lsp::Hover> TryHoverConcatenatedString(const HoverTarget& target, const HoverRequest& request,
                                                     bool showLength)
{
    const auto& gs = parser::GetGrammarSymbols();
    TSNode topPlus = FindTopmostBinaryPlus(target.node);
    if (ts_node_is_null(topPlus) || ts_node_symbol(topPlus) != gs.symBinaryExpression)
    {
        return std::nullopt;
    }

    auto rootScope = request.scopeIndex.GetRoot(request.uri);
    TSPoint pt = ts_node_start_point(target.node);
    const analysis::Scope* scope = rootScope ? FindInnermostScope(rootScope.get(), pt.row, pt.column) : nullptr;
    auto evalStr = EvaluateConstantStringExpr(topPlus, request, scope);
    if (!evalStr.has_value() || !IsPathLikeString(*evalStr))
    {
        return std::nullopt;
    }

    const std::string resolved = ResolveStringLiteralPath(*evalStr, request);
    if (resolved.empty())
    {
        return std::nullopt;
    }

    std::string md = FormatResolvedAssetMarkdown(*evalStr, resolved, showLength, request.i18n);
    return lsp::Hover{lsp::MarkupContent{lsp::MarkupKindEnum(lsp::MarkupKind::Markdown), std::move(md)}, target.range};
}

std::optional<lsp::Hover> TryHoverStringLiteral(const HoverTarget& target, const HoverRequest& request)
{
    const bool showLength = !request.config || request.config->features.hoverStringLiteralLength;
    const bool enablePathRes = !request.config || request.config->features.hoverStringLiteralPathResolution;

    if (enablePathRes)
    {
        if (auto concatHover = TryHoverConcatenatedString(target, request, showLength))
        {
            return concatHover;
        }
    }

    if (ts_node_symbol(target.node) == parser::GetGrammarSymbols().symBinaryExpression)
    {
        return std::nullopt;
    }

    const std::string_view txt = target.text;
    const std::string_view unquoted = UnquoteStringLiteral(txt);

    std::string md;
    if (enablePathRes && IsPathLikeString(unquoted))
    {
        const std::string resolved = ResolveStringLiteralPath(unquoted, request);
        if (!resolved.empty())
        {
            md = FormatResolvedAssetMarkdown(unquoted, resolved, showLength, request.i18n);
            return lsp::Hover{lsp::MarkupContent{lsp::MarkupKindEnum(lsp::MarkupKind::Markdown), std::move(md)},
                              target.range};
        }
    }

    md = "```angelscript\n" + std::string(txt) + "\n```";
    if (showLength)
    {
        md += "\n\n" + angel_lsp::i18n::FormatMessage(request.i18n, "hover-string-length",
                                                      "- **Length**: {} characters", unquoted.size());
    }
    return lsp::Hover{lsp::MarkupContent{lsp::MarkupKindEnum(lsp::MarkupKind::Markdown), std::move(md)}, target.range};
}

std::optional<lsp::Hover> TryHoverLiteralOrType(const HoverTarget& target, const HoverRequest& request,
                                                HoverProfiler& profiler)
{
    if (IsStringOrBinaryLiteral(target))
    {
        if (auto strHover = TryHoverStringLiteral(target, request))
        {
            return strHover;
        }
    }

    if (auto primHover = TryHoverPrimitiveType(target.text, target.range, request.i18n, profiler))
    {
        return primHover;
    }

    return TryHoverWildcardType(target.text, target.range, request.i18n, profiler);
}
} // namespace angel_lsp::features
