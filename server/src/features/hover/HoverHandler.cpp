#include "features/hover/HoverHandler.h"
#include "analysis/DocComment.h"
#include "analysis/OverloadResolver.h"
#include "analysis/SemanticHelpers.h"
#include "analysis/SignatureFormatter.h"
#include "analysis/VirtualMixinContext.h"
#include "analysis/overload/ConversionRankingEngine.h"
#include "parser/GrammarNames.h"
#include "utils/IncludeResolver.h"
#include "utils/LspLogger.h"
#include "utils/MultiFileLogger.h"
#include "utils/Timer.h"
#include "utils/Utils.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <vector>

namespace angel_lsp::features
{
namespace
{
/**
 * @brief The documentation comment above a symbol's declaration, read out of its own file.
 *
 * `request.sourceCode` is the file being hovered over, and a symbol's `startLine` counts
 * lines in the file that *declares* it. Pairing the two showed whatever happened to be at
 * that line number here, which for a cross-file hover is an unrelated comment about
 * something else. Empty rather than wrong when the declaring file's text is not held: the
 * declaration may have been indexed and released, and a hover with no documentation is a
 * hover that simply says less.
 *
 * The same-file case still goes through `request.sourceCode` directly, without a lookup -
 * that is the common path and the URIs match by construction.
 */
std::string OwnDocComment(const HoverRequest& request, const analysis::Symbol& symbol)
{
    if (symbol.fileUri.empty() || symbol.fileUri == request.uri)
    {
        return analysis::ExtractDocComment(request.sourceCode, symbol.startLine);
    }

    if (!request.readDocument)
    {
        return "";
    }

    const std::string* declaringText = request.readDocument(symbol.fileUri);
    if (!declaringText)
    {
        return "";
    }

    return analysis::ExtractDocComment(*declaringText, symbol.startLine);
}

/**
 * @brief Retrieves doc comment from a companion property or getter/setter.
 * @param[in] request Hover request context.
 * @param[in] symbol Symbol being hovered.
 * @return Companion doc comment string, or empty if none found.
 */
static std::string CompanionDocComment(const HoverRequest& request, const analysis::Symbol& symbol)
{
    if (symbol.containerName.empty())
    {
        return "";
    }
    std::string propName = symbol.name;
    if (propName.starts_with("get_") || propName.starts_with("set_"))
    {
        propName = propName.substr(4);
    }
    const std::string candidates[] = {symbol.containerName + "::" + propName,
                                      symbol.containerName + "::get_" + propName,
                                      symbol.containerName + "::set_" + propName};
    for (const auto& candidateName : candidates)
    {
        if (candidateName == symbol.qualifiedName)
        {
            continue;
        }
        const auto companionSyms = request.symbolTable.FindSymbolsPtr(candidateName);
        if (!companionSyms)
        {
            continue;
        }
        for (const auto& comp : *companionSyms)
        {
            std::string doc = OwnDocComment(request, comp);
            if (!doc.empty())
            {
                return doc;
            }
        }
    }
    return "";
}

/**
 * @brief Retrieves doc comment from an overridden method in ancestor types.
 * @param[in] request Hover request context.
 * @param[in] symbol Symbol being hovered.
 * @return Inherited doc comment string, or empty if none found.
 */
static std::string InheritedDocComment(const HoverRequest& request, const analysis::Symbol& symbol)
{
    if (symbol.type != analysis::SymbolType::Function || symbol.containerName.empty())
    {
        return "";
    }

    for (const auto& ancestor : analysis::GetInheritedTypeHierarchy(symbol.containerName, request.symbolTable))
    {
        if (ancestor == symbol.containerName)
        {
            continue;
        }

        const auto inherited = request.symbolTable.FindSymbolsPtr(ancestor + "::" + symbol.name);
        if (!inherited)
        {
            continue;
        }

        for (const auto& candidate : *inherited)
        {
            std::string doc = OwnDocComment(request, candidate);
            if (!doc.empty())
            {
                return doc;
            }
        }
    }

    return "";
}

/**
 * @brief The comment on a symbol, or the one on the declaration it overrides.
 *
 * An implementation carries no comment of its own far more often than not - the interface
 * is where the contract is written, and repeating it on every implementer is exactly what
 * nobody does. Inherited only when the method has none itself, and only from an ancestor
 * of its own container, so an unrelated method with the same name elsewhere is never
 * consulted. The hierarchy walk returns the container first, so the loop skips it: it was
 * already asked, and it answered nothing.
 */
std::string DocCommentForSymbol(const HoverRequest& request, const analysis::Symbol& symbol)
{
    std::string own = OwnDocComment(request, symbol);
    if (!own.empty())
    {
        return own;
    }

    std::string companion = CompanionDocComment(request, symbol);
    if (!companion.empty())
    {
        return companion;
    }

    return InheritedDocComment(request, symbol);
}

std::string FormatFunctionSignature(const analysis::Symbol& sym)
{
    return analysis::FormatFunctionDeclaration(sym);
}

std::string FormatClassSignature(const analysis::Symbol& sym)
{
    return analysis::FormatTypeDeclaration(sym);
}

/** @brief Squeezes runs of whitespace into single spaces so a declaration that was wrapped
 *         across lines in source appears on one line in the hover tooltip. */
std::string CollapseWhitespace(std::string_view text)
{
    std::string result;
    result.reserve(text.size());

    bool inWhitespace = false;
    for (char c : text)
    {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
        {
            if (!inWhitespace)
            {
                result.push_back(' ');
                inWhitespace = true;
            }
        }
        else
        {
            result.push_back(c);
            inWhitespace = false;
        }
    }

    auto start = result.find_first_not_of(' ');
    if (start == std::string::npos)
    {
        return "";
    }
    auto end = result.find_last_not_of(' ');
    return result.substr(start, end - start + 1);
}

/** @brief Finds the declaration node enclosing a point and reads its text back.
 *  @param root Tree-sitter root node of the file containing the declaration.
 *  @param sourceCode Source text of that file.
 *  @param point 0-based row and column of the declaration name.
 *  @param wantedNodeType AST node type to climb to, e.g. "parameter".
 *  @return Whitespace-collapsed declaration text, or an empty string if not found.
 *  @note LocalDefinition only records a type for variables, so a parameter hovered at a use
 *        site inside the body has nothing to show. The declaration position it does record
 *        is enough to find the declaring node and read it back with every modifier the user
 *        wrote ('const', '@', '&in'/'&out'/'&inout') still attached. */
std::string ExtractDeclarationTextAt(TSNode root, const std::string& sourceCode, TSPoint point,
                                     std::string_view wantedNodeType)
{
    if (ts_node_is_null(root))
    {
        return "";
    }

    TSNode current = ts_node_descendant_for_point_range(root, point, point);

    for (int depth = 0; depth < 8 && !ts_node_is_null(current); ++depth, current = ts_node_parent(current))
    {
        if (std::string_view(ts_node_type(current)) != wantedNodeType)
        {
            continue;
        }

        const uint32_t start = ts_node_start_byte(current);
        const uint32_t end = ts_node_end_byte(current);
        if (start < end && end <= sourceCode.size())
        {
            return CollapseWhitespace(sourceCode.substr(start, end - start));
        }
        break;
    }
    return "";
}

/** @brief Renders the hover line for a variable-like symbol, tagged with its role.
 *  @param sym Symbol of type Variable or Property.
 *  @param role Prefix shown to the user, e.g. "(property) " or "(global variable) ". */
std::string FormatVariableSignature(const analysis::Symbol& sym, const char* role)
{
    if (sym.type != analysis::SymbolType::Variable && sym.type != analysis::SymbolType::Property)
    {
        return std::string(role) + sym.name;
    }
    if (!std::holds_alternative<analysis::VariableSignature>(sym.signature))
    {
        return std::string(role) + sym.name;
    }
    return std::string(role) + analysis::FormatVariableDeclaration(sym.GetVariable(), sym.name);
}

bool IsContainerProperty(std::string_view containerName, const analysis::SymbolTable* symbolTable)
{
    if (containerName.empty() || !symbolTable)
    {
        return !containerName.empty();
    }
    auto containerSyms = symbolTable->FindSymbolsPtr(std::string(containerName));
    if (!containerSyms)
    {
        return false;
    }
    for (const auto& cs : *containerSyms)
    {
        if (cs.type == analysis::SymbolType::Class || cs.type == analysis::SymbolType::Interface ||
            cs.type == analysis::SymbolType::Enum)
        {
            return true;
        }
    }
    return false;
}

bool IsEnumType(std::string_view typeName, const analysis::SymbolTable* symbolTable)
{
    if (typeName.empty() || !symbolTable)
    {
        return false;
    }
    auto typeSyms = symbolTable->FindSymbolsPtr(std::string(typeName));
    if (!typeSyms)
    {
        return false;
    }
    for (const auto& ts : *typeSyms)
    {
        if (ts.type == analysis::SymbolType::Enum)
        {
            return true;
        }
    }
    return false;
}

bool IsVariableProperty(const analysis::Symbol& sym, const analysis::SymbolTable* symbolTable)
{
    if (IsContainerProperty(sym.containerName, symbolTable))
    {
        return true;
    }
    return IsEnumType(sym.GetVariable().typeName, symbolTable);
}

static std::string FormatNamespaceDeclaration(const analysis::Symbol& sym)
{
    if (!sym.qualifiedName.empty())
    {
        return "namespace " + sym.qualifiedName;
    }
    if (!sym.containerName.empty())
    {
        return "namespace " + sym.containerName + "::" + sym.name;
    }
    return "namespace " + sym.name;
}

/** @brief Renders the single hover line that describes a symbol of any kind. */
std::string FormatDeclarationText(const analysis::Symbol& sym, const analysis::SymbolTable* symbolTable = nullptr)
{
    switch (sym.type)
    {
    case analysis::SymbolType::Function:
    case analysis::SymbolType::Funcdef:
        return FormatFunctionSignature(sym);
    case analysis::SymbolType::Class:
    case analysis::SymbolType::Interface:
        return FormatClassSignature(sym);
    case analysis::SymbolType::Enum:
        return "enum " + sym.name;
    case analysis::SymbolType::Variable:
    {
        bool isProperty = IsVariableProperty(sym, symbolTable);
        return FormatVariableSignature(sym, isProperty ? "(property) " : "(global variable) ");
    }
    case analysis::SymbolType::Typedef:
        return "typedef " + sym.GetTypedef().baseType + " " + sym.name;
    case analysis::SymbolType::Namespace:
        return FormatNamespaceDeclaration(sym);
    case analysis::SymbolType::Property:
        if (std::holds_alternative<analysis::VariableSignature>(sym.signature))
        {
            return FormatVariableSignature(sym, "(property) ");
        }
        return "(property) " + sym.name;
    default:
        return sym.name;
    }
}

/** @brief Drops symbols that are the same declaration seen twice.
 *  @note A file reachable under two URI spellings (workspace scan vs. client didOpen) used
 *        to be collected once per spelling, which showed every overload twice in the hover.
 *        The server de-duplicates at index time; this is the last line of defence so a
 *        stale duplicate can never reach the user. */
void RemoveDuplicateSymbols(std::vector<analysis::Symbol>& symbols)
{
    std::vector<analysis::Symbol> unique;
    unique.reserve(symbols.size());
    for (const auto& sym : symbols)
    {
        const bool duplicate = std::any_of(unique.begin(), unique.end(),
                                           [&](const analysis::Symbol& kept)
                                           {
                                               if (kept.name != sym.name || kept.type != sym.type)
                                                   return false;
                                               if (!sym.fileUri.empty() && !kept.fileUri.empty() &&
                                                   sym.fileUri == kept.fileUri && sym.startLine == kept.startLine)
                                                   return true;
                                               return FormatDeclarationText(kept) == FormatDeclarationText(sym);
                                           });
        if (!duplicate)
        {
            unique.push_back(sym);
        }
    }
    symbols = std::move(unique);
}
} // namespace

namespace
{
struct HoverTarget
{
    TSNode node = {};
    std::string text;
    lsp::Range range{};
};

bool IsHoverableIdentifier(std::string_view txt, std::string_view type)
{
    if (type == "identifier" || type == "scoped_identifier" || type == "primitive_type" || type == "string_literal" ||
        analysis::IsPrimitiveTypeName(std::string(txt)) || analysis::IsReservedKeyword(std::string(txt)))
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

std::optional<HoverTarget> TryExtractHoverTarget(TSNode node, const std::string& sourceCode)
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
    if (!IsHoverableIdentifier(txt, ts_node_type(node)))
    {
        return std::nullopt;
    }

    TSPoint sp = ts_node_start_point(node);
    TSPoint ep = ts_node_end_point(node);
    return HoverTarget{node, std::move(txt), lsp::Range{{sp.row, sp.column}, {ep.row, ep.column}}};
}

std::optional<HoverTarget> ExtractHoverNode(TSNode rootNode, const std::string& sourceCode, TSPoint point)
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

    if (auto target = TryExtractHoverTarget(ts_node_parent(node), sourceCode))
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
            if (auto target = TryExtractHoverTarget(ts_node_parent(prevNode), sourceCode))
            {
                return target;
            }
        }
    }

    return std::nullopt;
}
} // namespace

namespace
{
/**
 * @brief The local definition whose own name spans this position.
 *
 * Null when the cursor is on a use rather than on a declaration, which is the ordinary case
 * and the one ResolveInScope answers. The ranges compared here are the name's, not the whole
 * declaration's, so a position inside one can only ever be that name.
 */
const analysis::LocalDefinition* DefinitionAtPosition(const analysis::Scope* scope, const lsp::Position& position)
{
    analysis::ScopeTraversalGuard cycleGuard;
    for (const analysis::Scope* current = scope; current != nullptr; current = current->parent)
    {
        if (!cycleGuard.CheckAndInsert(current))
        {
            break;
        }
        for (const analysis::LocalDefinition& def : current->definitions)
        {
            const bool afterStart = position.line > def.startLine ||
                                    (position.line == def.startLine && position.character >= def.startCharacter);
            const bool beforeEnd =
                position.line < def.endLine || (position.line == def.endLine && position.character <= def.endCharacter);

            if (afterStart && beforeEnd)
            {
                return &def;
            }
        }
    }

    return nullptr;
}
} // namespace

namespace
{
std::pair<size_t, size_t> FindLineSpan(const std::string& sourceCode, uint32_t targetLine)
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

struct IncludeDirectiveInfo
{
    size_t hash = 0;
    size_t closeQuote = 0;
    std::string rawPath;
};

std::optional<IncludeDirectiveInfo> ParseIncludeDirective(std::string_view line, size_t character)
{
    const auto info = angel_lsp::utils::IncludeResolver::ParseSingleLineInclude(line, character);
    if (!info)
    {
        return std::nullopt;
    }
    return IncludeDirectiveInfo{info->hashPos, info->closeDelim, info->rawPath};
}

/**
 * @brief Answers a hover on an `#include` line with the file it actually resolves to.
 *
 * Reported as a want: the line says `#include "helper.as"` and gives no hint which of the
 * search directories won, or whether it resolved at all. The path is the answer.
 *
 * Text-based rather than tree-based on purpose. The grammar produces one opaque
 * `preproc_directive` node for the whole line, so there is nothing inside it to locate a
 * cursor against. This finds the quotes on the cursor's line and answers only when the
 * cursor is on the directive.
 *
 * @return A hover, or nullopt when the line is not an `#include` this can answer.
 */
std::optional<lsp::Hover> HoverIncludeDirective(const HoverRequest& request)
{
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
        markdown += resolved.empty() ? "\n\nDoes not resolve to a file. Checked this file's own directory, then each "
                                       "`angelscript.searchDirectories` entry in order."
                                     : "\n\n" + resolved;
    }

    lsp::Range range{};
    range.start.line = request.position.line;
    range.start.character = static_cast<lsp::uint>(directive->hash);
    range.end.line = request.position.line;
    range.end.character = static_cast<lsp::uint>(directive->closeQuote + 1);

    return lsp::Hover{lsp::MarkupContent{lsp::MarkupKindEnum(lsp::MarkupKind::Markdown), std::move(markdown)}, range};
}

TSNode FindEnclosingCallNode(TSNode node)
{
    TSNode parent = ts_node_parent(node);
    if (ts_node_is_null(parent))
    {
        return TSNode{};
    }

    std::string_view pType = ts_node_type(parent);
    if (pType == "call_expression")
    {
        TSNode fn = parser::GetChildByField(parent, parser::fields::Function);
        if (!ts_node_is_null(fn) && (ts_node_eq(fn, node) || ts_node_start_byte(fn) == ts_node_start_byte(node)))
        {
            return parent;
        }
    }
    else if (pType == "member_expression" || pType == "scoped_identifier")
    {
        TSNode grandParent = ts_node_parent(parent);
        if (!ts_node_is_null(grandParent) && std::string_view(ts_node_type(grandParent)) == "call_expression")
        {
            TSNode fn = parser::GetChildByField(grandParent, parser::fields::Function);
            if (!ts_node_is_null(fn) &&
                (ts_node_eq(fn, parent) || ts_node_start_byte(fn) == ts_node_start_byte(parent)))
            {
                return grandParent;
            }
        }
    }
    return TSNode{};
}

/**
 * @brief If node represents the callee in a call_expression, extracts argument types
 * and resolves the best matching candidate from the overload set.
 */
std::optional<analysis::Symbol> ResolveCallOverload(TSNode node, const std::vector<analysis::Symbol>& candidates,
                                                    const HoverRequest& request, const analysis::Scope* scope)
{
    if (candidates.size() <= 1)
    {
        return std::nullopt;
    }

    TSNode callNode = FindEnclosingCallNode(node);
    if (ts_node_is_null(callNode))
    {
        return std::nullopt;
    }

    std::vector<std::string> argTypes =
        analysis::ExtractCallArgumentTypes(callNode, {scope, request.symbolTable, request.sourceCode, request.uri});

    auto match = analysis::ResolveBestOverload(candidates, argTypes, request.symbolTable);
    if (match.bestCandidate != nullptr)
    {
        return *match.bestCandidate;
    }

    const analysis::Symbol* fallback = analysis::FindBestFallbackOverload(candidates, argTypes, request.symbolTable);
    if (fallback != nullptr)
    {
        return *fallback;
    }

    return std::nullopt;
}

struct HoverProfiler
{
    utils::HighResTimer totalTimer;
    angel_lsp::utils::LspLogger* m_logger = nullptr;
    uint32_t line = 0;
    uint32_t character = 0;
    const char* nodeType = "";
    std::string pathText;
    std::string symbolName;
    double nodeMs = 0.0;
    double symMs = 0.0;
    double fmtMs = 0.0;
    bool emitted = false;

    [[nodiscard]] bool IsActive() const noexcept
    {
        return m_logger != nullptr || utils::MultiFileLogger::Instance().IsInitialized();
    }

    void Emit()
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

    ~HoverProfiler()
    {
        Emit();
    }
};

struct HoverQueryContext
{
    const HoverRequest& request;
    HoverProfiler& profiler;
    TSNode node = {};
    std::string nodeText;
    lsp::Range range{};
    analysis::VirtualMixinContext vctx;
    std::shared_ptr<const analysis::Scope> rootScope;
    uint32_t queryLine = 0;
    const analysis::Scope* scope = nullptr;
    TSNode parent = {};
};

std::optional<lsp::Hover> TryHoverPrimitiveType(std::string_view nodeText, const lsp::Range& range,
                                                HoverProfiler& profiler)
{
    if (!analysis::IsPrimitiveTypeName(nodeText))
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
    std::string md = fmt::format("```angelscript\n{} {}\n```", className, ctx.nodeText);
    ctx.profiler.fmtMs += fmtTimer.ElapsedMs();
    return lsp::Hover{lsp::MarkupContent{lsp::MarkupKindEnum(lsp::MarkupKind::Markdown), std::move(md)}, ctx.range};
}

bool IsMemberChildOfExpression(TSNode node, TSNode parent)
{
    if (ts_node_is_null(parent) || std::string_view(ts_node_type(parent)) != "member_expression")
    {
        return false;
    }
    TSNode memNode = parser::GetChildByField(parent, parser::fields::Member);
    return !ts_node_is_null(memNode) &&
           (ts_node_eq(memNode, node) || ts_node_start_byte(memNode) == ts_node_start_byte(node));
}

std::string ResolveReceiverTypeName(TSNode objectNode, const HoverQueryContext& ctx)
{
    std::string receiverTypeName = analysis::ResolveReceiverType(
        objectNode, ctx.request.sourceCode, ctx.request.symbolTable, {ctx.scope, ctx.vctx.hostClass, ctx.request.uri});
    if (!receiverTypeName.empty() && ctx.request.config && !ctx.request.config->types.arrayTypeName.empty())
    {
        receiverTypeName = analysis::MemberOwnerType(receiverTypeName, ctx.request.config->types.arrayTypeName);
    }

    if (!receiverTypeName.empty())
    {
        receiverTypeName = ctx.request.symbolTable.QualifyShortTypeName(receiverTypeName);
    }
    return receiverTypeName;
}

std::vector<analysis::Symbol> CollectReceiverMemberSymbols(const std::string& receiverTypeName,
                                                           std::string_view memberName,
                                                           const analysis::SymbolTable& symbolTable)
{
    std::vector<analysis::Symbol> memberSymbols;
    auto hierarchy = analysis::GetInheritedTypeHierarchy(receiverTypeName, symbolTable);
    for (const auto& typeName : hierarchy)
    {
        std::string qualifiedMember = typeName + "::" + std::string(memberName);
        auto found = symbolTable.FindSymbolsPtr(qualifiedMember);
        if (found)
        {
            for (const auto& sym : *found)
            {
                if (sym.type == analysis::SymbolType::Function)
                {
                    bool overriddenLower =
                        std::any_of(memberSymbols.begin(), memberSymbols.end(), [&](const analysis::Symbol& kept)
                                    { return analysis::HasSameParameterList(kept, sym); });
                    if (!overriddenLower)
                    {
                        memberSymbols.push_back(sym);
                    }
                }
                else
                {
                    memberSymbols.push_back(sym);
                }
            }
        }
    }
    std::stable_partition(memberSymbols.begin(), memberSymbols.end(),
                          [](const analysis::Symbol& s)
                          {
                              if (s.type == analysis::SymbolType::Variable &&
                                  std::holds_alternative<analysis::VariableSignature>(s.signature))
                              {
                                  return !s.GetVariable().isEnumConstant;
                              }
                              return true;
                          });
    return memberSymbols;
}

/** @brief Gathers and deduplicates doc comments for overload sets, prioritizing resolved call overloads. */
void CollectOverloadDocs(const HoverRequest& request, const std::vector<analysis::Symbol>& symbols,
                         const std::optional<analysis::Symbol>& best, std::vector<std::string>& docs)
{
    if (best)
    {
        std::string d = DocCommentForSymbol(request, *best);
        if (!d.empty())
        {
            docs.push_back(std::move(d));
            return;
        }
        for (const auto& sym : symbols)
        {
            std::string fallbackDoc = DocCommentForSymbol(request, sym);
            if (!fallbackDoc.empty())
            {
                docs.push_back(std::move(fallbackDoc));
                return;
            }
        }
        return;
    }

    for (const auto& sym : symbols)
    {
        std::string d = DocCommentForSymbol(request, sym);
        if (d.empty())
        {
            continue;
        }
        std::string trimmedD = CollapseWhitespace(d);
        bool alreadyPresent = std::any_of(docs.begin(), docs.end(), [&](const std::string& existing)
                                          { return CollapseWhitespace(existing) == trimmedD; });
        if (!alreadyPresent)
        {
            docs.push_back(std::move(d));
        }
    }
}

std::optional<lsp::Hover> FormatMemberHover(std::vector<analysis::Symbol>& memberSymbols,
                                            std::string_view accessorPropertyType, const HoverQueryContext& ctx)
{
    auto best = ResolveCallOverload(ctx.node, memberSymbols, ctx.request, ctx.scope);
    if (best)
    {
        auto it = std::find_if(memberSymbols.begin(), memberSymbols.end(), [&](const analysis::Symbol& s)
                               { return s.name == best->name && analysis::HasSameParameterList(s, *best); });
        if (it != memberSymbols.end())
        {
            std::rotate(memberSymbols.begin(), it, it + 1);
        }
    }

    RemoveDuplicateSymbols(memberSymbols);
    if (!memberSymbols.empty() && ctx.profiler.IsActive())
    {
        ctx.profiler.symbolName = memberSymbols.front().name;
    }

    utils::HighResTimer fmtTimer;
    std::ostringstream oss;
    oss << "```angelscript\n";

    if (!accessorPropertyType.empty())
    {
        oss << "(property) " << accessorPropertyType << " " << ctx.nodeText << "\n";
    }
    constexpr size_t kMaxMemberSymbols = 16;
    const size_t displayCount = std::min(memberSymbols.size(), kMaxMemberSymbols);
    for (size_t i = 0; i < displayCount; ++i)
    {
        if (i > 0)
        {
            oss << "\n";
        }
        oss << FormatDeclarationText(memberSymbols[i]);
    }
    if (memberSymbols.size() > kMaxMemberSymbols)
    {
        oss << "\n// ... and " << (memberSymbols.size() - kMaxMemberSymbols) << " more overloads";
    }
    oss << "\n```";

    std::vector<std::string> docs;
    CollectOverloadDocs(ctx.request, memberSymbols, best, docs);
    for (const auto& d : docs)
    {
        oss << "\n\n" << d;
    }

    ctx.profiler.fmtMs += fmtTimer.ElapsedMs();
    return lsp::Hover{lsp::MarkupContent{lsp::MarkupKindEnum(lsp::MarkupKind::Markdown), oss.str()}, ctx.range};
}

std::optional<lsp::Hover> TryHoverMemberAccess(const HoverQueryContext& ctx)
{
    utils::HighResTimer symTimer;
    if (ts_node_is_null(ctx.parent) || std::string_view(ts_node_type(ctx.parent)) != "member_expression")
    {
        ctx.profiler.symMs += symTimer.ElapsedMs();
        return std::nullopt;
    }

    TSNode objectNode = parser::GetChildByField(ctx.parent, parser::fields::Object);
    if (ts_node_is_null(objectNode))
    {
        ctx.profiler.symMs += symTimer.ElapsedMs();
        return std::nullopt;
    }

    std::string receiverTypeName = ResolveReceiverTypeName(objectNode, ctx);
    if (receiverTypeName.empty())
    {
        ctx.profiler.symMs += symTimer.ElapsedMs();
        return std::nullopt;
    }

    std::vector<analysis::Symbol> memberSymbols =
        CollectReceiverMemberSymbols(receiverTypeName, ctx.nodeText, ctx.request.symbolTable);

    std::string accessorPropertyType;
    if (memberSymbols.empty())
    {
        const int accessorMode = ctx.request.config ? ctx.request.config->engine.propertyAccessorMode : 2;
        if (accessorMode >= 2)
        {
            memberSymbols = analysis::FindPropertyAccessors(receiverTypeName, ctx.nodeText, ctx.request.symbolTable,
                                                            accessorMode == 3);
            accessorPropertyType = analysis::PropertyTypeFromAccessors(memberSymbols);
        }
    }

    if (memberSymbols.empty())
    {
        ctx.profiler.symMs += symTimer.ElapsedMs();
        return std::nullopt;
    }

    if (ctx.profiler.IsActive())
    {
        ctx.profiler.pathText = receiverTypeName + "." + ctx.nodeText;
    }
    ctx.profiler.symMs += symTimer.ElapsedMs();
    return FormatMemberHover(memberSymbols, accessorPropertyType, ctx);
}

std::string InferTypeFromAst(TSNode node, const std::string& sourceCode)
{
    TSNode cur = node;
    for (int i = 0; i < 4 && !ts_node_is_null(cur); ++i, cur = ts_node_parent(cur))
    {
        std::string_view cType = ts_node_type(cur);
        if (cType == "parameter" || cType == "variable_declaration")
        {
            TSNode typeNode = parser::GetChildByField(cur, parser::fields::ParamType);
            if (ts_node_is_null(typeNode))
            {
                typeNode = parser::GetChildByField(cur, parser::fields::VarType);
            }
            if (ts_node_is_null(typeNode))
            {
                typeNode = parser::GetChildByField(cur, parser::fields::Type);
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
    }
    return "";
}

void FormatParameterHover(const analysis::LocalDefinition& def, std::string_view typeName, const HoverQueryContext& ctx,
                          std::string& md)
{
    md += "(parameter) ";
    TSNode rootNode = ts_tree_root_node(ctx.request.tree);
    const std::string declText =
        ExtractDeclarationTextAt(rootNode, ctx.request.sourceCode, {def.startLine, def.startCharacter}, "parameter");
    if (!declText.empty())
    {
        md += declText;
    }
    else
    {
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
}

std::optional<analysis::Symbol> FindMatchingGlobalSymbol(const analysis::LocalDefinition& def,
                                                         const HoverQueryContext& ctx)
{
    auto candidates = analysis::FindSymbolsInScope(def.name, ctx.node, ctx.request.sourceCode, ctx.request.symbolTable);
    for (const auto& cand : candidates)
    {
        if (cand.type == analysis::SymbolType::Variable && cand.fileUri == ctx.request.uri)
        {
            return cand;
        }
    }
    for (const auto& cand : candidates)
    {
        if (cand.type == analysis::SymbolType::Variable)
        {
            return cand;
        }
    }
    auto exactCandidates = ctx.request.symbolTable.FindSymbolsPtr(def.name);
    if (exactCandidates)
    {
        for (const auto& cand : *exactCandidates)
        {
            if (cand.type == analysis::SymbolType::Variable && cand.fileUri == ctx.request.uri)
            {
                return cand;
            }
        }
    }
    return std::nullopt;
}

const analysis::Scope* FindDefinitionScope(const HoverQueryContext& ctx, const analysis::LocalDefinition& def)
{
    analysis::ScopeTraversalGuard cycleGuard;
    for (const analysis::Scope* s = ctx.scope; s != nullptr; s = s->parent)
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
    return analysis::FindScopeDeclaringDefinition(ctx.rootScope.get(), def);
}

bool IsDirectInitDeclarator(TSNode node)
{
    TSNode declarator = node;
    if (!ts_node_is_null(declarator) && std::string_view(ts_node_type(declarator)) != "variable_declarator")
    {
        declarator = ts_node_parent(node);
    }
    if (ts_node_is_null(declarator) || std::string_view(ts_node_type(declarator)) != "variable_declarator")
    {
        return false;
    }
    TSNode args = parser::GetChildByField(declarator, parser::fields::Arguments);
    return !ts_node_is_null(args);
}

std::optional<analysis::Symbol> ResolveConstructorForDeclarator(TSNode declarator, std::string_view typeName,
                                                                const HoverQueryContext& ctx)
{
    TSNode argList = analysis::ResolveArgumentListNode(declarator);
    if (ts_node_is_null(argList))
    {
        return std::nullopt;
    }

    std::string baseName = analysis::CleanBaseType(typeName);
    if (baseName.empty())
    {
        return std::nullopt;
    }

    auto candidates =
        analysis::CollectConstructorCandidates(baseName, declarator, ctx.request.sourceCode, ctx.request.symbolTable);
    if (candidates.empty())
    {
        return std::nullopt;
    }

    if (candidates.size() == 1)
    {
        return candidates[0];
    }

    auto argTypes = analysis::ExtractCallArgumentTypes(
        argList, {ctx.scope, ctx.request.symbolTable, ctx.request.sourceCode, ctx.request.uri});
    auto match = analysis::ResolveBestOverload(candidates, argTypes, ctx.request.symbolTable);
    if (match.bestCandidate != nullptr)
    {
        return *match.bestCandidate;
    }

    const analysis::Symbol* fallback =
        analysis::FindBestFallbackOverload(candidates, argTypes, ctx.request.symbolTable);
    if (fallback != nullptr)
    {
        return *fallback;
    }

    return candidates[0];
}

void TryAppendConstructorSignature(TSNode node, std::string_view typeName, const HoverQueryContext& ctx,
                                   std::string& md)
{
    TSNode declarator = node;
    if (!ts_node_is_null(declarator) && std::string_view(ts_node_type(declarator)) != "variable_declarator")
    {
        declarator = ts_node_parent(node);
    }
    if (!ts_node_is_null(declarator) && std::string_view(ts_node_type(declarator)) == "variable_declarator")
    {
        if (auto ctorSym = ResolveConstructorForDeclarator(declarator, typeName, ctx))
        {
            std::string ctorSig = analysis::FormatFunctionDeclaration(*ctorSym);
            if (!ctorSig.empty())
            {
                md += "\n";
                md += ctorSig;
            }
        }
    }
}

void FormatVariableHover(const analysis::LocalDefinition& def, std::string_view typeName, const HoverQueryContext& ctx,
                         std::string& md)
{
    const analysis::Scope* declaringScope = FindDefinitionScope(ctx, def);

    bool isInsideFunction = false;
    analysis::ScopeTraversalGuard cycleGuard;
    for (const analysis::Scope* s = declaringScope; s != nullptr; s = s->parent)
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
        auto globalSym = FindMatchingGlobalSymbol(def, ctx);
        if (globalSym.has_value())
        {
            md += FormatVariableSignature(*globalSym, "(global variable) ");
        }
        else
        {
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
    }
    else
    {
        md += "(local variable) ";
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

    if (IsDirectInitDeclarator(ctx.node))
    {
        TryAppendConstructorSignature(ctx.node, typeName, ctx, md);
    }
}

static void AppendLocalDoc(const analysis::LocalDefinition& def, std::string_view typeName,
                           const HoverQueryContext& ctx, std::string& md)
{
    std::string doc = analysis::ExtractDocComment(ctx.request.sourceCode, def.startLine);
    if (doc.empty() && IsDirectInitDeclarator(ctx.node))
    {
        TSNode declarator = ctx.node;
        if (!ts_node_is_null(declarator) && std::string_view(ts_node_type(declarator)) != "variable_declarator")
        {
            declarator = ts_node_parent(ctx.node);
        }
        if (auto ctorSym = ResolveConstructorForDeclarator(declarator, typeName, ctx))
        {
            doc = DocCommentForSymbol(ctx.request, *ctorSym);
        }
    }
    if (!doc.empty())
    {
        md += "\n\n";
        md += doc;
    }
}

std::optional<lsp::Hover> TryHoverLocalDefinition(const HoverQueryContext& ctx)
{
    if (!ctx.rootScope || !ctx.scope)
    {
        return std::nullopt;
    }

    utils::HighResTimer symTimer;
    lsp::Position queryPos{ctx.queryLine, ctx.request.position.character};
    const analysis::LocalDefinition* def = DefinitionAtPosition(ctx.scope, queryPos);
    if (!def)
    {
        def = analysis::ResolveInScope(ctx.scope, ctx.nodeText);
    }

    if (!def ||
        (def->kind != analysis::LocalDefinitionKind::Parameter && def->kind != analysis::LocalDefinitionKind::Variable))
    {
        ctx.profiler.symMs += symTimer.ElapsedMs();
        return std::nullopt;
    }

    std::string inferredType;
    std::string_view typeName = def->typeName;
    if (typeName.empty())
    {
        inferredType = InferTypeFromAst(ctx.node, ctx.request.sourceCode);
        if (inferredType.empty() && def->kind == analysis::LocalDefinitionKind::Parameter)
        {
            inferredType =
                analysis::InferLambdaParamType(ctx.node, def->name, ctx.request.symbolTable, ctx.request.sourceCode);
        }
        typeName = inferredType;
    }

    if (ctx.profiler.IsActive())
    {
        ctx.profiler.symbolName = def->name;
    }
    ctx.profiler.symMs += symTimer.ElapsedMs();
    utils::HighResTimer fmtTimer;
    std::string md;
    md.reserve(128);
    md += "```angelscript\n";

    if (def->kind == analysis::LocalDefinitionKind::Parameter)
    {
        FormatParameterHover(*def, typeName, ctx, md);
    }
    else
    {
        FormatVariableHover(*def, typeName, ctx, md);
    }
    md += "\n```";

    AppendLocalDoc(*def, typeName, ctx, md);

    ctx.profiler.fmtMs += fmtTimer.ElapsedMs();
    return lsp::Hover{lsp::MarkupContent{lsp::MarkupKindEnum(lsp::MarkupKind::Markdown), std::move(md)}, ctx.range};
}

/**
 * @brief Fallback hover resolution for parameter AST nodes without scope tree entries.
 * @param[in] ctx Hover query context.
 * @return Parameter hover if node is part of a parameter declaration, std::nullopt otherwise.
 */
std::optional<lsp::Hover> TryHoverParameterAst(const HoverQueryContext& ctx)
{
    if (ts_node_is_null(ctx.node) || !ctx.request.tree)
    {
        return std::nullopt;
    }

    TSNode current = ctx.node;
    bool foundParam = false;
    for (int depth = 0; depth < 4 && !ts_node_is_null(current); ++depth, current = ts_node_parent(current))
    {
        if (std::string_view(ts_node_type(current)) == "parameter")
        {
            foundParam = true;
            break;
        }
    }

    if (!foundParam)
    {
        return std::nullopt;
    }

    TSNode rootNode = ts_tree_root_node(ctx.request.tree);
    std::string declText =
        ExtractDeclarationTextAt(rootNode, ctx.request.sourceCode, ts_node_start_point(ctx.node), "parameter");
    if (declText.empty())
    {
        return std::nullopt;
    }

    std::string md = "```angelscript\n(parameter) " + declText + "\n```";
    return lsp::Hover{lsp::MarkupContent{lsp::MarkupKindEnum(lsp::MarkupKind::Markdown), std::move(md)}, ctx.range};
}

static bool IsNamedArgumentLabelAtCursor(TSNode node, TSNode parent)
{
    if (ts_node_is_null(node) || ts_node_is_null(parent) || std::string_view(ts_node_type(parent)) != "argument_list")
    {
        return false;
    }
    TSNode next = ts_node_next_sibling(node);
    while (!ts_node_is_null(next) && std::string_view(ts_node_type(next)) == "comment")
    {
        next = ts_node_next_sibling(next);
    }
    return !ts_node_is_null(next) && std::string_view(ts_node_type(next)) == ":";
}

static std::vector<analysis::Symbol> ResolveNamedArgCalleeCandidates(TSNode callNode, const HoverQueryContext& ctx)
{
    TSNode funcNode = parser::GetChildByField(callNode, parser::fields::Function);
    if (ts_node_is_null(funcNode) && ts_node_child_count(callNode) > 0)
    {
        funcNode = ts_node_child(callNode, 0);
    }
    if (ts_node_is_null(funcNode))
    {
        return {};
    }

    if (std::string_view(ts_node_type(funcNode)) == "member_expression")
    {
        TSNode objectNode = parser::GetChildByField(funcNode, parser::fields::Object);
        TSNode memberNode = parser::GetChildByField(funcNode, parser::fields::Member);
        if (!ts_node_is_null(objectNode) && !ts_node_is_null(memberNode))
        {
            std::string receiverTypeName = ResolveReceiverTypeName(objectNode, ctx);
            std::string memberName = analysis::GetNodeText(memberNode, ctx.request.sourceCode);
            return CollectReceiverMemberSymbols(receiverTypeName, memberName, ctx.request.symbolTable);
        }
    }
    else
    {
        std::string funcName = analysis::GetNodeText(funcNode, ctx.request.sourceCode);
        return ctx.request.symbolTable.FindSymbols(funcName);
    }
    return {};
}

static std::optional<lsp::Hover> FormatNamedArgHover(const std::vector<analysis::Symbol>& candidates,
                                                     const HoverQueryContext& ctx)
{
    for (const auto& sym : candidates)
    {
        if (sym.type != analysis::SymbolType::Function ||
            !std::holds_alternative<analysis::FunctionSignature>(sym.signature))
        {
            continue;
        }
        for (const auto& param : sym.GetFunction().parameters)
        {
            if (param.name != ctx.nodeText)
            {
                continue;
            }
            std::string md = "```angelscript\n(parameter) " + param.typeName + " " + param.name;
            if (!param.defaultValue.empty())
            {
                md += " = " + param.defaultValue;
            }
            else if (param.rawText.find('=') != std::string::npos)
            {
                md += " " + param.rawText.substr(param.rawText.find('='));
            }
            md += "\n```\nParameter of `" + sym.name + "`";
            return lsp::Hover{lsp::MarkupContent{lsp::MarkupKindEnum(lsp::MarkupKind::Markdown), std::move(md)},
                              ctx.range};
        }
    }
    return std::nullopt;
}

/**
 * @brief Attempts to provide hover for a named argument parameter label in a call expression.
 * @param[in] ctx Hover query context.
 * @return Parameter hover if node is a named argument label, std::nullopt otherwise.
 */
std::optional<lsp::Hover> TryHoverNamedArgument(const HoverQueryContext& ctx)
{
    if (!IsNamedArgumentLabelAtCursor(ctx.node, ctx.parent))
    {
        return std::nullopt;
    }

    TSNode callNode = ts_node_parent(ctx.parent);
    if (ts_node_is_null(callNode))
    {
        return std::nullopt;
    }

    auto candidates = ResolveNamedArgCalleeCandidates(callNode, ctx);
    return FormatNamedArgHover(candidates, ctx);
}

void AppendEnclosingClassMethods(TSNode node, std::string_view nodeText, const HoverRequest& request,
                                 std::vector<analysis::Symbol>& symbols)
{
    auto containers = analysis::GetEnclosingContainers(node, request.sourceCode);
    for (const auto& c : containers)
    {
        if (c.kind == analysis::ContainerKind::Class || c.kind == analysis::ContainerKind::Interface)
        {
            auto hierarchy = analysis::GetInheritedTypeHierarchy(c.qualifiedName.empty() ? c.name : c.qualifiedName,
                                                                 request.symbolTable);
            for (const auto& typeName : hierarchy)
            {
                auto found = request.symbolTable.FindSymbolsPtr(typeName + "::" + std::string(nodeText));
                if (found)
                {
                    for (const auto& sym : *found)
                    {
                        if (sym.type == analysis::SymbolType::Function)
                        {
                            bool overriddenLower =
                                std::any_of(symbols.begin(), symbols.end(), [&](const analysis::Symbol& kept)
                                            { return analysis::HasSameParameterList(kept, sym); });
                            if (!overriddenLower)
                            {
                                symbols.push_back(sym);
                            }
                        }
                    }
                }
            }
            break;
        }
    }
}

std::vector<analysis::Symbol> CollectHierarchyHostSymbols(const std::string& hostClass, std::string_view nodeText,
                                                          const analysis::SymbolTable& symbolTable)
{
    std::vector<analysis::Symbol> hostSymbols;
    auto hierarchy = analysis::GetInheritedTypeHierarchy(hostClass, symbolTable);
    for (const auto& typeName : hierarchy)
    {
        auto found = symbolTable.FindSymbolsPtr(typeName + "::" + std::string(nodeText));
        if (found)
        {
            for (const auto& sym : *found)
            {
                if (sym.type == analysis::SymbolType::Function)
                {
                    bool overriddenLower =
                        std::any_of(hostSymbols.begin(), hostSymbols.end(), [&](const analysis::Symbol& kept)
                                    { return analysis::HasSameParameterList(kept, sym); });
                    if (!overriddenLower)
                    {
                        hostSymbols.push_back(sym);
                    }
                }
                else if (sym.type == analysis::SymbolType::Variable || sym.type == analysis::SymbolType::Property)
                {
                    hostSymbols.push_back(sym);
                }
            }
        }
    }
    return hostSymbols;
}

void MergeHostSymbols(std::vector<analysis::Symbol>& symbols, std::vector<analysis::Symbol> hostSymbols,
                      const std::string& fileUri)
{
    std::vector<analysis::Symbol> containerSymbols;
    for (const auto& s : symbols)
    {
        if (!s.containerName.empty() || s.fileUri == fileUri)
        {
            containerSymbols.push_back(s);
        }
    }
    symbols = std::move(containerSymbols);

    for (auto& hs : hostSymbols)
    {
        bool present = std::any_of(symbols.begin(), symbols.end(), [&](const analysis::Symbol& s)
                                   { return s.name == hs.name && analysis::HasSameParameterList(s, hs); });
        if (!present)
        {
            symbols.push_back(std::move(hs));
        }
    }
}

void AppendVirtualHostSymbols(const HoverQueryContext& ctx, std::vector<analysis::Symbol>& symbols,
                              std::string& accessorPropertyType)
{
    if (!ctx.vctx.isVirtual || ctx.vctx.hostClass.empty())
    {
        return;
    }

    std::vector<analysis::Symbol> hostSymbols =
        CollectHierarchyHostSymbols(ctx.vctx.hostClass, ctx.nodeText, ctx.request.symbolTable);

    if (hostSymbols.empty())
    {
        auto hierarchy = analysis::GetInheritedTypeHierarchy(ctx.vctx.hostClass, ctx.request.symbolTable);
        for (const auto& typeName : hierarchy)
        {
            auto accessors = analysis::FindPropertyAccessors(typeName, ctx.nodeText, ctx.request.symbolTable, false);
            if (!accessors.empty())
            {
                hostSymbols = std::move(accessors);
                accessorPropertyType = analysis::PropertyTypeFromAccessors(hostSymbols);
                break;
            }
        }
    }

    if (!hostSymbols.empty())
    {
        MergeHostSymbols(symbols, std::move(hostSymbols), ctx.request.uri);
    }
}

std::optional<lsp::Hover> TryHoverScopeFallback(const HoverQueryContext& ctx)
{
    if (!ctx.scope)
    {
        return std::nullopt;
    }

    const analysis::LocalDefinition* def = analysis::ResolveInScope(ctx.scope, ctx.nodeText);
    if (!def)
    {
        return std::nullopt;
    }

    utils::HighResTimer fmtTimer;
    std::ostringstream oss;
    oss << "```angelscript\n";
    if (def->kind == analysis::LocalDefinitionKind::Field)
    {
        oss << "(field) ";
    }
    if (!def->typeName.empty())
    {
        oss << def->typeName << " ";
    }
    oss << def->name << "\n```";
    std::string doc = analysis::ExtractDocComment(ctx.request.sourceCode, def->startLine);
    if (!doc.empty())
    {
        oss << "\n\n" << doc;
    }
    ctx.profiler.fmtMs += fmtTimer.ElapsedMs();
    return lsp::Hover{lsp::MarkupContent{lsp::MarkupKindEnum(lsp::MarkupKind::Markdown), oss.str()}, ctx.range};
}

std::vector<analysis::Symbol> CollectScopedSymbols(HoverQueryContext& ctx)
{
    std::vector<analysis::Symbol> symbols;
    if (!ts_node_is_null(ctx.parent) && std::string_view(ts_node_type(ctx.parent)) == "scoped_identifier")
    {
        const uint32_t pStart = ts_node_start_byte(ctx.parent);
        const uint32_t nEnd = ts_node_end_byte(ctx.node);
        if (pStart < ctx.request.sourceCode.size() && nEnd <= ctx.request.sourceCode.size() && pStart < nEnd)
        {
            const std::string scopedPrefix = ctx.request.sourceCode.substr(pStart, nEnd - pStart);
            if (ctx.profiler.IsActive())
            {
                ctx.profiler.pathText = scopedPrefix;
            }
            symbols =
                analysis::FindSymbolsInScope(scopedPrefix, ctx.node, ctx.request.sourceCode, ctx.request.symbolTable);
        }
    }
    if (symbols.empty())
    {
        symbols = analysis::FindSymbolsInScope(ctx.nodeText, ctx.node, ctx.request.sourceCode, ctx.request.symbolTable);
    }
    std::erase_if(symbols, [](const analysis::Symbol& s) { return s.type == analysis::SymbolType::CallReference; });
    return symbols;
}

lsp::Hover FormatSymbolsHover(std::vector<analysis::Symbol>& symbols, std::string_view accessorPropertyType,
                              const HoverQueryContext& ctx)
{
    auto best = ResolveCallOverload(ctx.node, symbols, ctx.request, ctx.scope);
    if (best)
    {
        auto it = std::find_if(symbols.begin(), symbols.end(), [&](const analysis::Symbol& s)
                               { return s.name == best->name && analysis::HasSameParameterList(s, *best); });
        if (it != symbols.end())
        {
            std::rotate(symbols.begin(), it, it + 1);
        }
    }

    RemoveDuplicateSymbols(symbols);
    if (!symbols.empty() && ctx.profiler.IsActive())
    {
        ctx.profiler.symbolName = symbols.front().name;
    }

    utils::HighResTimer fmtTimer;
    std::ostringstream oss;
    oss << "```angelscript\n";

    if (!accessorPropertyType.empty())
    {
        oss << "(property) " << accessorPropertyType << " " << ctx.nodeText << "\n";
    }

    constexpr size_t kMaxHoverSymbols = 16;
    const size_t displayCount = std::min(symbols.size(), kMaxHoverSymbols);
    for (size_t i = 0; i < displayCount; ++i)
    {
        if (i > 0)
        {
            oss << "\n";
        }
        oss << FormatDeclarationText(symbols[i], &ctx.request.symbolTable);
    }
    if (symbols.size() > kMaxHoverSymbols)
    {
        oss << "\n// ... and " << (symbols.size() - kMaxHoverSymbols) << " more overloads";
    }
    oss << "\n```";

    std::vector<std::string> docs;
    CollectOverloadDocs(ctx.request, symbols, best, docs);
    for (const auto& d : docs)
    {
        oss << "\n\n" << d;
    }

    ctx.profiler.fmtMs += fmtTimer.ElapsedMs();
    return lsp::Hover{lsp::MarkupContent{lsp::MarkupKindEnum(lsp::MarkupKind::Markdown), oss.str()}, ctx.range};
}

/**
 * @brief Finds the enclosing lambda_expression node for a given node within shallow depth.
 * @param[in] node AST node.
 * @return Enclosing lambda_expression node, or null node if not within a lambda header.
 */
TSNode FindEnclosingLambda(TSNode node)
{
    TSNode cur = node;
    for (int depth = 0; depth < 5 && !ts_node_is_null(cur); ++depth)
    {
        if (std::string_view(ts_node_type(cur)) == "lambda_expression")
        {
            return cur;
        }
        cur = ts_node_parent(cur);
    }
    return TSNode{};
}

/**
 * @brief Formats an untyped or fallback lambda signature from its parameter list node.
 * @param[in] lambdaNode Lambda AST node.
 * @param[in] sourceCode Document source string.
 * @param[out] oss Output stream.
 */
void FormatLambdaParameters(TSNode lambdaNode, std::string_view sourceCode, std::ostringstream& oss)
{
    oss << "(anonymous function) function";
    TSNode paramListNode = ts_node_child_by_field_name(lambdaNode, "parameters", 10);
    if (ts_node_is_null(paramListNode))
    {
        uint32_t count = ts_node_named_child_count(lambdaNode);
        for (uint32_t i = 0; i < count; ++i)
        {
            TSNode child = ts_node_named_child(lambdaNode, i);
            if (std::string_view(ts_node_type(child)) == "parameter_list")
            {
                paramListNode = child;
                break;
            }
        }
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

/**
 * @brief Attempts to construct hover information when hovering on an anonymous function / lambda header.
 * @param[in] ctx Hover query context.
 * @return Optional hover tooltip if hovering on a lambda keyword or header.
 */
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
            oss << "\n\n" << doc;
        }
    }

    ctx.profiler.fmtMs += fmtTimer.ElapsedMs();
    return lsp::Hover{lsp::MarkupContent{lsp::MarkupKindEnum(lsp::MarkupKind::Markdown), oss.str()}, ctx.range};
}

std::optional<lsp::Hover> TryHoverExpressionOrLocal(const HoverQueryContext& ctx)
{
    bool isMemberChild = IsMemberChildOfExpression(ctx.node, ctx.parent);
    if (isMemberChild)
    {
        if (auto memberHover = TryHoverMemberAccess(ctx))
        {
            return memberHover;
        }
    }

    if (auto localHover = TryHoverLocalDefinition(ctx))
    {
        return localHover;
    }

    if (auto paramHover = TryHoverParameterAst(ctx))
    {
        return paramHover;
    }

    if (auto namedArgHover = TryHoverNamedArgument(ctx))
    {
        return namedArgHover;
    }

    if (!isMemberChild)
    {
        if (auto memberHover = TryHoverMemberAccess(ctx))
        {
            return memberHover;
        }
    }
    return std::nullopt;
}

/**
 * @brief Checks if an AST node represents an explicitly qualified member access.
 * @param[in] node AST node to test.
 * @return True if node is the member part of a scoped_identifier or member_expression.
 */
static bool IsQualifiedMemberNode(TSNode node)
{
    TSNode parent = ts_node_parent(node);
    if (ts_node_is_null(parent))
    {
        return false;
    }
    const std::string_view pType = ts_node_type(parent);
    if (pType == "scoped_identifier")
    {
        TSNode firstChild = ts_node_named_child(parent, 0);
        return !ts_node_is_null(firstChild) && !ts_node_eq(firstChild, node) &&
               ts_node_start_byte(firstChild) != ts_node_start_byte(node);
    }
    if (pType == "member_expression")
    {
        TSNode memberField = parser::GetChildByField(parent, parser::fields::Member);
        return ts_node_eq(memberField, node) ||
               (!ts_node_is_null(memberField) && ts_node_start_byte(memberField) == ts_node_start_byte(node));
    }
    return false;
}

enum class ContainerContextPreference
{
    None,
    PreferClass,
    PreferNamespace
};

/**
 * @brief Determines if an AST node is positioned in a type specifier or using namespace context.
 * @param[in] node AST node to inspect.
 * @return Context preference indicating class, namespace, or neutral.
 */
static ContainerContextPreference CheckContextPreference(TSNode node)
{
    TSNode cur = ts_node_parent(node);
    while (!ts_node_is_null(cur))
    {
        const std::string_view t = ts_node_type(cur);
        if (t == "datatype" || t == "template_type_list" || t == "base_class_list" || t == "type")
        {
            return ContainerContextPreference::PreferClass;
        }
        if (t == "using_declaration")
        {
            return ContainerContextPreference::PreferNamespace;
        }
        if (t == "scoped_identifier" || t == "scope")
        {
            cur = ts_node_parent(cur);
            continue;
        }
        break;
    }
    return ContainerContextPreference::None;
}

/**
 * @brief Locates the next member AST node immediately following the given node in a scoped_identifier.
 * @param[in] parent Enclosing scoped_identifier AST node.
 * @param[in] node Container segment AST node.
 * @return Next member TSNode or null node if none found.
 */
static TSNode FindNextScopedMemberNode(TSNode parent, TSNode node)
{
    if (ts_node_is_null(parent) || std::string_view(ts_node_type(parent)) != "scoped_identifier")
    {
        return TSNode{};
    }
    TSTreeCursor cursor = ts_tree_cursor_new(parent);
    bool foundCurrent = false;
    TSNode nextMember{};
    if (ts_tree_cursor_goto_first_child(&cursor))
    {
        do
        {
            TSNode current = ts_tree_cursor_current_node(&cursor);
            if (foundCurrent && ts_node_is_named(current))
            {
                nextMember = current;
                break;
            }
            if (!foundCurrent && ts_node_is_named(current) &&
                (ts_node_eq(current, node) || ts_node_start_byte(current) == ts_node_start_byte(node)))
            {
                foundCurrent = true;
            }
        } while (ts_tree_cursor_goto_next_sibling(&cursor));
    }
    ts_tree_cursor_delete(&cursor);
    return nextMember;
}

/**
 * @brief Tests whether a given member name belongs to a class declaration or its inherited hierarchy.
 * @param[in] classSym Class symbol descriptor.
 * @param[in] memberName Member identifier text.
 * @param[in] table Workspace symbol table.
 * @return True if member is declared or inherited by the class.
 */
static bool MemberBelongsToClass(const analysis::Symbol& classSym, std::string_view memberName,
                                 const analysis::SymbolTable& table)
{
    const std::string qName = classSym.qualifiedName.empty() ? classSym.name : classSym.qualifiedName;
    auto hierarchy = analysis::GetInheritedTypeHierarchy(qName, table);
    for (const auto& cls : hierarchy)
    {
        if (const auto found = table.FindSymbolsPtr(cls + "::" + std::string(memberName)))
        {
            for (const auto& sym : *found)
            {
                if ((sym.fileUri == classSym.fileUri && sym.startLine >= classSym.fullRange.startLine &&
                     sym.startLine <= classSym.fullRange.endLine) ||
                    (cls != classSym.name && cls != qName))
                {
                    return true;
                }
            }
        }
    }
    return false;
}

/**
 * @brief Tests whether a given member name belongs to a namespace declaration.
 * @param[in] nsSym Namespace symbol descriptor.
 * @param[in] memberName Member identifier text.
 * @param[in] table Workspace symbol table.
 * @return True if member is declared inside the namespace.
 */
static bool MemberBelongsToNamespace(const analysis::Symbol& nsSym, std::string_view memberName,
                                     const analysis::SymbolTable& table)
{
    const std::string qName = nsSym.qualifiedName.empty() ? nsSym.name : nsSym.qualifiedName;
    if (const auto found = table.FindSymbolsPtr(qName + "::" + std::string(memberName)))
    {
        for (const auto& sym : *found)
        {
            if (sym.fileUri == nsSym.fileUri && sym.startLine >= nsSym.fullRange.startLine &&
                sym.startLine <= nsSym.fullRange.endLine)
            {
                return true;
            }
        }
    }
    return false;
}

/**
 * @brief Disambiguates between Class and Namespace symbols sharing the same identifier.
 * @param[in] ctx Hover query context.
 * @param[in,out] symbols Candidate symbol collection to filter.
 */
static void DisambiguateContainerSymbols(const HoverQueryContext& ctx, std::vector<analysis::Symbol>& symbols)
{
    if (symbols.size() < 2)
    {
        return;
    }
    auto classIt = std::find_if(symbols.begin(), symbols.end(),
                                [](const analysis::Symbol& s) { return s.type == analysis::SymbolType::Class; });
    auto nsIt = std::find_if(symbols.begin(), symbols.end(),
                             [](const analysis::Symbol& s) { return s.type == analysis::SymbolType::Namespace; });
    if (classIt == symbols.end() || nsIt == symbols.end() || classIt->name != nsIt->name)
    {
        return;
    }

    auto pref = CheckContextPreference(ctx.node);
    if (pref == ContainerContextPreference::PreferClass)
    {
        std::erase_if(symbols, [](const analysis::Symbol& s) { return s.type == analysis::SymbolType::Namespace; });
        return;
    }
    if (pref == ContainerContextPreference::PreferNamespace)
    {
        std::erase_if(symbols, [](const analysis::Symbol& s) { return s.type == analysis::SymbolType::Class; });
        return;
    }

    TSNode memberNode = FindNextScopedMemberNode(ctx.parent, ctx.node);
    if (ts_node_is_null(memberNode))
    {
        return;
    }

    std::string memberName = analysis::GetNodeText(memberNode, ctx.request.sourceCode);
    const bool inClass = MemberBelongsToClass(*classIt, memberName, ctx.request.symbolTable);
    const bool inNs = MemberBelongsToNamespace(*nsIt, memberName, ctx.request.symbolTable);
    if (inClass && !inNs)
    {
        std::erase_if(symbols, [](const analysis::Symbol& s) { return s.type == analysis::SymbolType::Namespace; });
    }
    else if (inNs && !inClass)
    {
        std::erase_if(symbols, [](const analysis::Symbol& s) { return s.type == analysis::SymbolType::Class; });
    }
}

std::optional<lsp::Hover> TryHoverSymbolCandidates(HoverQueryContext& ctx)
{
    utils::HighResTimer symTimer;
    std::vector<analysis::Symbol> symbols = CollectScopedSymbols(ctx);
    if (!IsQualifiedMemberNode(ctx.node))
    {
        AppendEnclosingClassMethods(ctx.node, ctx.nodeText, ctx.request, symbols);
    }
    DisambiguateContainerSymbols(ctx, symbols);

    std::string accessorPropertyType;
    AppendVirtualHostSymbols(ctx, symbols, accessorPropertyType);

    if (symbols.empty() && ctx.rootScope)
    {
        if (auto fallbackHover = TryHoverScopeFallback(ctx))
        {
            ctx.profiler.symMs += symTimer.ElapsedMs();
            return fallbackHover;
        }
    }

    if (symbols.empty() && accessorPropertyType.empty())
    {
        const int accessorMode = ctx.request.config ? ctx.request.config->engine.propertyAccessorMode : 2;
        if (accessorMode >= 2)
        {
            symbols = analysis::FindGlobalPropertyAccessors(ctx.nodeText, ctx.request.symbolTable, accessorMode == 3);
            accessorPropertyType = analysis::PropertyTypeFromAccessors(symbols);
        }
    }

    ctx.profiler.symMs += symTimer.ElapsedMs();
    if (symbols.empty())
    {
        return std::nullopt;
    }

    return FormatSymbolsHover(symbols, accessorPropertyType, ctx);
}
HoverQueryContext BuildHoverQueryContext(const HoverRequest& request, HoverProfiler& profiler, HoverTarget&& target)
{
    analysis::VirtualMixinContext vctx = analysis::ResolveVirtualMixinContext(request.uri, request.symbolTable);
    auto rootScope = request.scopeIndex.GetRoot(analysis::ResolvePhysicalUri(vctx, request.uri));
    uint32_t queryLine = analysis::ResolvePhysicalLine(vctx, request.position.line);
    const analysis::Scope* scope =
        rootScope ? FindInnermostScope(rootScope.get(), queryLine, request.position.character) : nullptr;
    TSNode parent = ts_node_parent(target.node);

    return HoverQueryContext{request,      profiler,        target.node,          std::move(target.text),
                             target.range, std::move(vctx), std::move(rootScope), queryLine,
                             scope,        parent};
}
bool IsPathLikeString(std::string_view str)
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
    return "";
}

std::optional<lsp::Hover> TryHoverStringLiteral(const HoverTarget& target, const HoverRequest& request)
{
    std::string_view txt = target.text;
    std::string_view unquoted = txt;
    if (txt.size() >= 2 && ((txt.front() == '"' && txt.back() == '"') || (txt.front() == '\'' && txt.back() == '\'')))
    {
        unquoted = txt.substr(1, txt.size() - 2);
    }
    else if (txt.starts_with("\"\"\"") && txt.ends_with("\"\"\"") && txt.size() >= 6)
    {
        unquoted = txt.substr(3, txt.size() - 6);
    }

    std::string md;
    if (IsPathLikeString(unquoted))
    {
        std::string resolved = ResolveStringLiteralPath(unquoted, request);
        if (!resolved.empty())
        {
            std::error_code ec;
            auto fsize = std::filesystem::file_size(resolved, ec);
            std::string sizeStr = ec ? "Unknown" : FormatFileSize(fsize);
            std::string normalizedPath = resolved;
            std::replace(normalizedPath.begin(), normalizedPath.end(), '\\', '/');

            md = "*(asset)* `" + std::string(unquoted) +
                 "`\n\n"
                 "- **Status**: Exists\n"
                 "- **Size**: " +
                 sizeStr +
                 "\n"
                 "- **Path**: `" +
                 normalizedPath + "`";
        }
        else
        {
            md = "*(string literal)*: `" + std::string(txt) +
                 "`\n\n"
                 "- **Length**: " +
                 std::to_string(unquoted.size()) +
                 " characters\n"
                 "- **File**: Not found";
        }
    }
    else
    {
        md = "*(string literal)*: `" + std::string(txt) +
             "`\n\n"
             "- **Length**: " +
             std::to_string(unquoted.size()) + " characters";
    }

    return lsp::Hover{lsp::MarkupContent{lsp::MarkupKindEnum(lsp::MarkupKind::Markdown), std::move(md)}, target.range};
}
} // namespace

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

    if (std::string_view(ts_node_type(target->node)) == "string_literal" ||
        (!target->text.empty() && target->text.front() == '"' && target->text.back() == '"'))
    {
        return TryHoverStringLiteral(*target, request);
    }

    profiler.nodeType = ts_node_type(target->node);
    if (profiler.IsActive())
    {
        profiler.pathText = target->text;
        profiler.symbolName = target->text;
    }

    if (auto primHover = TryHoverPrimitiveType(target->text, target->range, profiler))
    {
        return primHover;
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
