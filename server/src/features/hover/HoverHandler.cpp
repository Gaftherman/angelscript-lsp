#include "features/hover/HoverHandler.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/types/VirtualMixinContext.h"
#include "features/hover/HoverSymbolFormatter.h"
#include "features/hover/HoverTypeInspector.h"
#include "i18n/i18n.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"
#include "parser/Primitives.h"
#include "utils/IncludeResolver.h"
#include "utils/LspLogger.h"
#include "utils/MultiFileLogger.h"
#include "utils/Timer.h"
#include <spdlog/fmt/fmt.h>
#include <string>
#include <string_view>
#include <utility>

namespace angel_lsp::features
{
bool HoverProfiler::IsActive() const noexcept
{
    return m_logger != nullptr || utils::MultiFileLogger::Instance().IsInitialized();
}

void HoverProfiler::Emit()
{
    if (emitted)
    {
        return;
    }
    emitted = true;
    double totalMs = totalTimer.ElapsedMs();
    if (m_logger)
    {
        std::string lspMsg =
            fmt::format("[Hover Profile] Total: {:.2f} ms (NodeLookup: {:.2f} ms, SymbolResolve: {:.2f} ms, "
                        "Formatting: {:.2f} ms) at {}:{}",
                        totalMs, nodeMs, symMs, fmtMs, line, character);
        m_logger->LogInfo(lspMsg);
    }
    if (utils::MultiFileLogger::Instance().IsInitialized())
    {
        std::string fileLogMsg =
            fmt::format("Pos({}:{}) Node='{}' Path='{}' Symbol='{}' [NodeLookup={:.2f}ms SymbolResolve={:.2f}ms "
                        "Formatting={:.2f}ms Total={:.2f}ms]",
                        line, character, nodeType, pathText, symbolName, nodeMs, symMs, fmtMs, totalMs);
        utils::MultiFileLogger::Instance().LogHover(utils::MultiFileLogLevel::Info, fileLogMsg, totalMs);
    }
}

HoverProfiler::~HoverProfiler()
{
    Emit();
}

/**
 * @brief Finds the byte span of a target line in the source buffer.
 * @param[in] sourceCode Source text buffer.
 * @param[in] targetLine 0-indexed line number.
 * @return Pair of start and end byte indices, or (npos, npos) if out of bounds.
 */
static std::pair<size_t, size_t> FindLineSpan(const std::string& sourceCode, uint32_t targetLine)
{
    size_t start = 0;
    for (uint32_t current = 0; current < targetLine; ++current)
    {
        const size_t nextBreak = sourceCode.find('\n', start);
        if (nextBreak == std::string::npos)
        {
            return {std::string::npos, std::string::npos};
        }
        start = nextBreak + 1;
    }
    const size_t end = sourceCode.find('\n', start);
    return {start, end == std::string::npos ? sourceCode.size() : end};
}

/**
 * @brief Parses include directive on a line and returns range and path if cursor is on it.
 * @param[in] line Line text.
 * @param[in] character Cursor column offset.
 * @return Parsed include directive info or std::nullopt.
 */
static std::optional<IncludeDirectiveInfo> ParseIncludeDirective(std::string_view line, size_t character)
{
    const auto info = angel_lsp::utils::IncludeResolver::ParseSingleLineInclude(line, character);
    if (!info)
    {
        return std::nullopt;
    }
    return IncludeDirectiveInfo{info->hashPos, info->closeDelim, info->rawPath};
}

/**
 * @brief Produces hover tooltip for #include directives.
 * @param[in] request Hover request context.
 * @return Hover result if on an include line, std::nullopt otherwise.
 */
static std::optional<lsp::Hover> HoverIncludeDirective(const HoverRequest& request)
{
    if (request.sourceCode.find('#') == std::string::npos)
    {
        return std::nullopt;
    }

    const auto [lineStart, lineEnd] = FindLineSpan(request.sourceCode, request.position.line);
    if (lineStart == std::string::npos)
    {
        return std::nullopt;
    }

    const std::string_view line(request.sourceCode.data() + lineStart, lineEnd - lineStart);
    auto directive = ParseIncludeDirective(line, static_cast<size_t>(request.position.character));
    if (!directive)
    {
        return std::nullopt;
    }

    std::string markdown = "```angelscript\n#include \"" + directive->rawPath + "\"\n```";

    if (request.resolveInclude)
    {
        const std::string resolved = request.resolveInclude(directive->rawPath);
        if (resolved.empty())
        {
            markdown += "\n\n" + angel_lsp::i18n::FormatMessage(
                                     request.i18n, "hover-include-unresolved",
                                     "Does not resolve to a file. Checked this file's own directory, then each "
                                     "`angelscript.searchDirectories` entry in order.");
        }
        else
        {
            markdown += "\n\n" + resolved;
        }
    }

    lsp::Range range{};
    range.start.line = request.position.line;
    range.start.character = static_cast<lsp::uint>(directive->hash);
    range.end.line = request.position.line;
    range.end.character = static_cast<lsp::uint>(directive->closeQuote + 1);

    return lsp::Hover{lsp::MarkupContent{lsp::MarkupKindEnum(lsp::MarkupKind::Markdown), std::move(markdown)}, range};
}

/**
 * @brief Checks if a token text and symbol match hoverable identifier rules.
 * @param[in] txt Token string view.
 * @param[in] sym Grammar symbol ID.
 * @param[in] gs Precompiled grammar symbols.
 * @return True if hoverable, false otherwise.
 */
static bool IsHoverableIdentifier(std::string_view txt, TSSymbol sym, const parser::GrammarSymbols& gs)
{
    if (txt == "?" || sym == gs.symDatatype || sym == gs.symIdentifier || sym == gs.symScopedIdentifier ||
        sym == gs.symPrimitiveType || sym == gs.symStringLiteral || analysis::IsPrimitiveTypeName(std::string(txt)) ||
        analysis::IsReservedKeyword(std::string(txt)))
    {
        return true;
    }

    if (!txt.empty() && (isalpha(static_cast<unsigned char>(txt[0])) || txt[0] == '_'))
    {
        for (char c : txt)
        {
            if (!isalnum(static_cast<unsigned char>(c)) && c != '_')
            {
                return false;
            }
        }
        return true;
    }
    return false;
}

/**
 * @brief Attempts to extract hover target descriptor from an AST node.
 * @param[in] node AST candidate node.
 * @param[in] sourceCode Source text buffer.
 * @return Hover target if valid, std::nullopt otherwise.
 */
static std::optional<HoverTarget> TryExtractHoverTarget(TSNode node, const std::string& sourceCode)
{
    if (ts_node_is_null(node))
    {
        return std::nullopt;
    }
    const uint32_t sb = ts_node_start_byte(node);
    const uint32_t eb = ts_node_end_byte(node);
    if (sb >= sourceCode.size() || eb > sourceCode.size() || sb >= eb)
    {
        return std::nullopt;
    }

    std::string txt = sourceCode.substr(sb, eb - sb);
    if (!IsHoverableIdentifier(txt, ts_node_symbol(node), parser::GetGrammarSymbols()))
    {
        return std::nullopt;
    }

    TSPoint sp = ts_node_start_point(node);
    TSPoint ep = ts_node_end_point(node);
    return HoverTarget{node, std::move(txt), lsp::Range{{sp.row, sp.column}, {ep.row, ep.column}}};
}

/**
 * @brief Extracts the hover target AST node under the cursor point.
 * @param[in] rootNode Tree root node.
 * @param[in] sourceCode Source text buffer.
 * @param[in] point Cursor position coordinates.
 * @return Hover target if found, std::nullopt otherwise.
 */
static std::optional<HoverTarget> ExtractHoverNode(TSNode rootNode, const std::string& sourceCode, TSPoint point)
{
    TSNode node = ts_node_descendant_for_point_range(rootNode, point, point);
    if (ts_node_is_null(node))
    {
        return std::nullopt;
    }

    if (auto target = TryExtractHoverTarget(node, sourceCode))
    {
        return target;
    }

    if (auto target = TryExtractHoverTarget(parser::GetParent(node), sourceCode))
    {
        return target;
    }

    if (point.column > 0)
    {
        TSPoint prevPt = {point.row, point.column - 1};
        TSNode prevNode = ts_node_descendant_for_point_range(rootNode, prevPt, prevPt);
        if (auto target = TryExtractHoverTarget(prevNode, sourceCode))
        {
            return target;
        }
        if (!ts_node_is_null(prevNode))
        {
            if (auto target = TryExtractHoverTarget(parser::GetParent(prevNode), sourceCode))
            {
                return target;
            }
        }
    }

    return std::nullopt;
}

static HoverQueryContext BuildHoverQueryContext(const HoverRequest& request, HoverProfiler& profiler,
                                                HoverTarget&& target)
{
    analysis::VirtualMixinContext vctx = analysis::ResolveVirtualMixinContext(request.uri, request.symbolTable);
    auto rootScope = request.scopeIndex.GetRoot(analysis::ResolvePhysicalUri(vctx, request.uri));
    uint32_t queryLine = analysis::ResolvePhysicalLine(vctx, request.position.line);
    const analysis::Scope* scope =
        rootScope ? FindInnermostScope(rootScope.get(), queryLine, request.position.character) : nullptr;
    TSNode parent = parser::GetParent(target.node);

    return HoverQueryContext{request,      profiler,        target.node,          std::move(target.text),
                             target.range, std::move(vctx), std::move(rootScope), queryLine,
                             scope,        parent};
}

std::optional<lsp::Hover> GetHover(const HoverRequest& request)
{
    if (!request.tree || request.sourceCode.empty())
    {
        return std::nullopt;
    }

    HoverProfiler profiler;
    profiler.m_logger = request.logger;
    profiler.line = request.position.line;
    profiler.character = request.position.character;

    if (auto includeHover = HoverIncludeDirective(request))
    {
        profiler.nodeType = "include";
        profiler.pathText = "include";
        profiler.symbolName = "include";
        return includeHover;
    }

    TSNode rootNode = ts_tree_root_node(request.tree);
    utils::HighResTimer nodeTimer;
    auto target = ExtractHoverNode(rootNode, request.sourceCode, {request.position.line, request.position.character});
    profiler.nodeMs = nodeTimer.ElapsedMs();
    if (!target)
    {
        return std::nullopt;
    }

    profiler.nodeType = parser::GetNodeType(target->node);
    if (profiler.IsActive())
    {
        profiler.pathText = target->text;
        profiler.symbolName = target->text;
    }

    if (auto earlyHover = TryHoverLiteralOrType(*target, request, profiler))
    {
        return earlyHover;
    }

    HoverQueryContext ctx = BuildHoverQueryContext(request, profiler, std::move(*target));

    if (auto thisHover = TryHoverThis(ctx))
    {
        return thisHover;
    }

    if (auto lambdaHover = TryHoverLambda(ctx))
    {
        return lambdaHover;
    }

    if (auto exprHover = TryHoverExpressionOrLocal(ctx))
    {
        return exprHover;
    }

    return TryHoverSymbolCandidates(ctx);
}
} // namespace angel_lsp::features
