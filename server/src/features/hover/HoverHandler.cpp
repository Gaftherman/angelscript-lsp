#include "features/hover/HoverHandler.h"
#include "analysis/documentation/DocComment.h"
#include "analysis/documentation/SignatureFormatter.h"
#include "analysis/resolution/ConversionRankingEngine.h"
#include "analysis/resolution/OverloadResolver.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/rules/RuleIndex.h"
#include "analysis/types/TypeSanitization.h"
#include "analysis/types/VirtualMixinContext.h"
#include "config/EngineRuleConfig.h"
#include "features/hover/HoverDocumentationRenderer.h"
#include "features/hover/HoverSymbolFormatter.h"
#include "features/hover/HoverTypeInspector.h"
#include "i18n/i18n.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarNames.h"
#include "parser/GrammarSymbols.h"
#include "parser/Primitives.h"
#include "utils/Constants.h"
#include "utils/IncludeResolver.h"
#include "utils/LspLogger.h"
#include "utils/MultiFileLogger.h"
#include "utils/ScopedNameUtils.h"
#include "utils/Timer.h"
#include "utils/Utils.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <spdlog/fmt/fmt.h>
#include <sstream>
#include <unordered_set>
#include <vector>

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

/**
 * @brief Locates the local definition whose name spans the specified position.
 * @param[in] scope Starting lexical scope.
 * @param[in] position Query position.
 * @return Matching LocalDefinition pointer, or nullptr if not on declaration name.
 */
static const analysis::LocalDefinition* DefinitionAtPosition(const analysis::Scope* scope,
                                                             const lsp::Position& position)
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

/**
 * @brief Tests whether an AST node is the member field of a member_expression parent.
 * @param[in] node Child AST node.
 * @param[in] parent Enclosing parent node.
 * @return True if node is member child, false otherwise.
 */
static bool IsMemberChildOfExpression(TSNode node, TSNode parent)
{
    const auto& gs = parser::GetGrammarSymbols();
    if (ts_node_is_null(parent) || ts_node_symbol(parent) != gs.symMemberExpression)
    {
        return false;
    }
    TSNode memNode = parser::GetChildByField(parent, gs.fieldMember);
    return !ts_node_is_null(memNode) &&
           (ts_node_eq(memNode, node) || ts_node_start_byte(memNode) == ts_node_start_byte(node));
}

/**
 * @brief Resolves receiver type name of an object expression node.
 * @param[in] objectNode Object expression TSNode.
 * @param[in] ctx Hover query context.
 * @return Qualified receiver type name or empty string.
 */
static std::string ResolveReceiverTypeName(TSNode objectNode, const HoverQueryContext& ctx)
{
    const config::EngineRuleConfig* engineRules = ctx.request.config ? &ctx.request.config->engineRules : nullptr;
    std::string receiverTypeName =
        analysis::ResolveReceiverType(objectNode, ctx.request.sourceCode, ctx.request.symbolTable,
                                      {ctx.scope, ctx.vctx.hostClass, ctx.request.uri,
                                       ctx.request.symbolTable.GetDefaultArrayType(), nullptr, engineRules});
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

/**
 * @brief Collects all member symbols matching name across receiver type hierarchy.
 * @param[in] receiverTypeName Base container type name.
 * @param[in] memberName Member identifier text.
 * @param[in] symbolTable Workspace symbol table.
 * @return Member symbols list with deduplicated overloads.
 */
static std::vector<analysis::Symbol> CollectReceiverMemberSymbols(const std::string& receiverTypeName,
                                                                  std::string_view memberName,
                                                                  const analysis::SymbolTable& symbolTable)
{
    std::vector<analysis::Symbol> memberSymbols;
    auto hierarchy = analysis::GetInheritedTypeHierarchy(receiverTypeName, symbolTable);
    for (const auto& typeName : hierarchy)
    {
        std::string qualifiedMember = utils::JoinScopedName(typeName, memberName);
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

/**
 * @brief Attempts hover resolution for a member access expression.
 * @param[in] ctx Hover query context.
 * @return Formatted member hover or std::nullopt.
 */
static std::optional<lsp::Hover> TryHoverMemberAccess(const HoverQueryContext& ctx)
{
    utils::HighResTimer symTimer;
    if (!IsMemberChildOfExpression(ctx.node, ctx.parent))
    {
        ctx.profiler.symMs += symTimer.ElapsedMs();
        return std::nullopt;
    }

    TSNode objectNode = parser::GetChildByField(ctx.parent, parser::GetGrammarSymbols().fieldObject);
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

/**
 * @brief Attempts hover resolution for in-scope local definitions.
 * @param[in] ctx Hover query context.
 * @return Formatted hover response or std::nullopt.
 */
static std::optional<lsp::Hover> TryHoverLocalDefinition(const HoverQueryContext& ctx)
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
        uint32_t cursorByte = ts_node_start_byte(ctx.node);
        def = analysis::ResolveInScopeAt(ctx.scope, ctx.nodeText, cursorByte);
    }

    if (!def ||
        (def->kind != analysis::LocalDefinitionKind::Parameter && def->kind != analysis::LocalDefinitionKind::Variable))
    {
        ctx.profiler.symMs += symTimer.ElapsedMs();
        return std::nullopt;
    }

    const std::string deducedType = DeduceLocalDefinitionHoverType(*def, ctx);
    const std::string_view typeName = deducedType;

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
static std::optional<lsp::Hover> TryHoverParameterAst(const HoverQueryContext& ctx)
{
    if (ts_node_is_null(ctx.node) || !ctx.request.tree)
    {
        return std::nullopt;
    }

    TSNode paramNode = parser::FindAncestorWithSymbol(ctx.node, parser::GetGrammarSymbols().symParameter);
    if (ts_node_is_null(paramNode))
    {
        return std::nullopt;
    }

    TSNode rootNode = ts_tree_root_node(ctx.request.tree);
    std::string declText = ExtractDeclarationTextAt(rootNode, ctx.request.sourceCode, ts_node_start_point(ctx.node),
                                                    parser::GetGrammarSymbols().symParameter);
    if (declText.empty())
    {
        return std::nullopt;
    }

    std::string md = "```angelscript\n(parameter) " + declText + "\n```";
    return lsp::Hover{lsp::MarkupContent{lsp::MarkupKindEnum(lsp::MarkupKind::Markdown), std::move(md)}, ctx.range};
}

static bool IsNamedArgumentLabelAtCursor(TSNode node, TSNode parent)
{
    if (ts_node_is_null(node) || ts_node_is_null(parent) ||
        ts_node_symbol(parent) != parser::GetGrammarSymbols().symArgumentList)
    {
        return false;
    }
    TSNode next = ts_node_next_sibling(node);
    while (!ts_node_is_null(next) && ts_node_symbol(next) == parser::GetGrammarSymbols().symComment)
    {
        next = ts_node_next_sibling(next);
    }
    return !ts_node_is_null(next) && ts_node_symbol(next) == parser::GetGrammarSymbols().tokColon;
}

static std::vector<analysis::Symbol> ResolveNamedArgCalleeCandidates(TSNode callNode, const HoverQueryContext& ctx)
{
    TSNode funcNode = parser::GetChildByField(callNode, parser::GetGrammarSymbols().fieldFunction);
    if (ts_node_is_null(funcNode) && ts_node_child_count(callNode) > 0)
    {
        funcNode = ts_node_child(callNode, 0);
    }
    if (ts_node_is_null(funcNode))
    {
        return {};
    }

    if (ts_node_symbol(funcNode) == parser::GetGrammarSymbols().symMemberExpression)
    {
        TSNode objectNode = parser::GetChildByField(funcNode, parser::GetGrammarSymbols().fieldObject);
        TSNode memberNode = parser::GetChildByField(funcNode, parser::GetGrammarSymbols().fieldMember);
        if (!ts_node_is_null(objectNode) && !ts_node_is_null(memberNode))
        {
            std::string receiverTypeName = ResolveReceiverTypeName(objectNode, ctx);
            std::string memberName = parser::GetNodeText(memberNode, ctx.request.sourceCode);
            return CollectReceiverMemberSymbols(receiverTypeName, memberName, ctx.request.symbolTable);
        }
    }
    else
    {
        std::string funcName = parser::GetNodeText(funcNode, ctx.request.sourceCode);
        if (const auto found = ctx.request.symbolTable.FindSymbolsPtr(funcName))
        {
            return *found;
        }
    }
    return {};
}

static std::optional<lsp::Hover> TryHoverNamedArgument(const HoverQueryContext& ctx)
{
    if (!IsNamedArgumentLabelAtCursor(ctx.node, ctx.parent))
    {
        return std::nullopt;
    }

    TSNode callNode = parser::GetParent(ctx.parent);
    if (ts_node_is_null(callNode))
    {
        return std::nullopt;
    }

    auto candidates = ResolveNamedArgCalleeCandidates(callNode, ctx);
    return FormatNamedArgHover(candidates, ctx);
}

static std::optional<lsp::Hover> TryHoverExpressionOrLocal(const HoverQueryContext& ctx)
{
    if (auto autoHover = TryHoverAutoType(ctx))
    {
        return autoHover;
    }

    if (IsMemberChildOfExpression(ctx.node, ctx.parent))
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

    return std::nullopt;
}

static bool IsQualifiedMemberNode(TSNode node, std::string_view sourceCode = {})
{
    TSNode parent = parser::GetParent(node);
    if (ts_node_is_null(parent))
    {
        return false;
    }
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol pType = ts_node_symbol(parent);
    if (pType == gs.symScopedIdentifier)
    {
        TSNode firstChild = ts_node_named_child(parent, 0);
        return !ts_node_is_null(firstChild) && !ts_node_eq(firstChild, node) &&
               ts_node_start_byte(firstChild) != ts_node_start_byte(node);
    }
    if (pType == gs.symMemberExpression)
    {
        TSNode memberField = parser::GetChildByField(parent, gs.fieldMember);
        return ts_node_eq(memberField, node) ||
               (!ts_node_is_null(memberField) && ts_node_start_byte(memberField) == ts_node_start_byte(node));
    }
    if (pType == gs.symDatatype && !sourceCode.empty())
    {
        return !analysis::ExtractDatatypeScopePrefix(parent, sourceCode).empty();
    }
    return false;
}

static void AppendEnclosingClassMethods(TSNode node, std::string_view nodeText, const HoverRequest& request,
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
                auto found = request.symbolTable.FindMemberSymbolPtr(typeName, nodeText);
                if (found)
                {
                    for (const auto& sym : *found)
                    {
                        if (sym.type == analysis::SymbolType::Function &&
                            sym.containerKind != analysis::ContainerKind::Namespace)
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

static std::vector<analysis::Symbol> CollectHierarchyHostSymbols(const std::string& hostClass,
                                                                 std::string_view nodeText,
                                                                 const analysis::SymbolTable& symbolTable)
{
    std::vector<analysis::Symbol> hostSymbols;
    auto hierarchy = analysis::GetInheritedTypeHierarchy(hostClass, symbolTable);
    for (const auto& typeName : hierarchy)
    {
        auto found = symbolTable.FindMemberSymbolPtr(typeName, nodeText);
        if (found)
        {
            for (const auto& sym : *found)
            {
                if (sym.containerKind == analysis::ContainerKind::Namespace)
                {
                    continue;
                }
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

static void MergeHostSymbols(std::vector<analysis::Symbol>& symbols, std::vector<analysis::Symbol> hostSymbols,
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

static void AppendVirtualHostSymbols(const HoverQueryContext& ctx, std::vector<analysis::Symbol>& symbols,
                                     std::string& accessorPropertyType)
{
    std::string hostClass;
    if (ctx.vctx.isVirtual && !ctx.vctx.hostClass.empty())
    {
        hostClass = ctx.vctx.hostClass;
    }
    else
    {
        hostClass =
            analysis::InferHostClassFromEnclosingMixin(ctx.node, ctx.request.sourceCode, ctx.request.symbolTable);
    }
    if (hostClass.empty())
    {
        return;
    }

    std::vector<analysis::Symbol> hostSymbols =
        CollectHierarchyHostSymbols(hostClass, ctx.nodeText, ctx.request.symbolTable);

    if (hostSymbols.empty())
    {
        auto hierarchy = analysis::GetInheritedTypeHierarchy(hostClass, ctx.request.symbolTable);
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

static std::optional<lsp::Hover> TryHoverScopeFallback(const HoverQueryContext& ctx)
{
    if (!ctx.scope)
    {
        return std::nullopt;
    }

    uint32_t cursorByte = ts_node_start_byte(ctx.node);
    const analysis::LocalDefinition* def = analysis::ResolveInScopeAt(ctx.scope, ctx.nodeText, cursorByte);
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
        oss << "\n\n---\n\n" << doc;
    }
    ctx.profiler.fmtMs += fmtTimer.ElapsedMs();
    return lsp::Hover{lsp::MarkupContent{lsp::MarkupKindEnum(lsp::MarkupKind::Markdown), oss.str()}, ctx.range};
}

static void FilterEnumMemberSymbols(std::vector<analysis::Symbol>& symbols, const HoverQueryContext& ctx)
{
    const auto& gs = parser::GetGrammarSymbols();
    if (ts_node_is_null(ctx.parent) || ts_node_symbol(ctx.parent) != gs.symEnumMember)
    {
        return;
    }
    TSNode enumDecl = parser::GetParent(ctx.parent);
    if (ts_node_is_null(enumDecl) || ts_node_symbol(enumDecl) != gs.symEnumDeclaration)
    {
        return;
    }
    TSNode nameNode = parser::GetChildByField(enumDecl, gs.fieldName);
    if (ts_node_is_null(nameNode))
    {
        return;
    }
    std::string enumName = parser::GetNodeText(nameNode, ctx.request.sourceCode);
    if (enumName.empty())
    {
        return;
    }
    std::erase_if(symbols,
                  [&](const analysis::Symbol& s)
                  {
                      if (s.containerName == enumName || utils::EndsWithScopedMember(s.containerName, enumName))
                      {
                          return false;
                      }
                      if (std::holds_alternative<analysis::VariableSignature>(s.signature))
                      {
                          const auto& vs = s.GetVariable();
                          if (vs.isEnumConstant &&
                              (vs.typeName == enumName || utils::EndsWithScopedMember(vs.typeName, enumName)))
                          {
                              return false;
                          }
                      }
                      return true;
                  });
}

static void ResolveScopedMemberFallback(std::string_view scopedPrefix, const HoverQueryContext& ctx,
                                        std::vector<analysis::Symbol>& symbols)
{
    if (!symbols.empty() || !utils::ContainsScopeSeparator(scopedPrefix))
    {
        return;
    }
    const std::string_view qualifierView = utils::ParentScopeView(scopedPrefix);
    if (qualifierView.empty())
    {
        return;
    }
    const std::string qualifier(qualifierView);
    auto qualSyms = analysis::FindSymbolsInScope(qualifier, ctx.node, ctx.request.sourceCode, ctx.request.symbolTable);
    for (const auto& qs : qualSyms)
    {
        const std::string qName = qs.qualifiedName.empty() ? qs.name : qs.qualifiedName;
        if (const auto found = ctx.request.symbolTable.FindMemberSymbolPtr(qName, ctx.nodeText))
        {
            symbols.insert(symbols.end(), found->begin(), found->end());
            return;
        }
    }
}

static std::vector<analysis::Symbol> CollectScopedSymbols(HoverQueryContext& ctx)
{
    std::vector<analysis::Symbol> symbols;
    bool isExplicitlyScoped = false;
    if (!ts_node_is_null(ctx.parent))
    {
        const auto& gs = parser::GetGrammarSymbols();
        const TSSymbol pSym = ts_node_symbol(ctx.parent);
        if (pSym == gs.symScopedIdentifier)
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
                symbols = analysis::FindSymbolsInScope(scopedPrefix, ctx.node, ctx.request.sourceCode,
                                                       ctx.request.symbolTable);
                ResolveScopedMemberFallback(scopedPrefix, ctx, symbols);
                isExplicitlyScoped = utils::ContainsScopeSeparator(scopedPrefix);
            }
        }
        else if (pSym == gs.symDatatype)
        {
            const std::string prefix = analysis::ExtractDatatypeScopePrefix(ctx.parent, ctx.request.sourceCode);
            if (!prefix.empty())
            {
                if (ctx.profiler.IsActive())
                {
                    ctx.profiler.pathText = prefix + ctx.nodeText;
                }
                symbols = analysis::FindSymbolsInScope(prefix + ctx.nodeText, ctx.node, ctx.request.sourceCode,
                                                       ctx.request.symbolTable);
                isExplicitlyScoped = true;
            }
        }
    }
    if (symbols.empty() && !isExplicitlyScoped)
    {
        symbols = analysis::FindSymbolsInScope(ctx.nodeText, ctx.node, ctx.request.sourceCode, ctx.request.symbolTable);
    }
    FilterEnumMemberSymbols(symbols, ctx);
    std::erase_if(symbols, [](const analysis::Symbol& s) { return s.type == analysis::SymbolType::CallReference; });
    return symbols;
}

static void PopulateGlobalPropertyAccessors(std::vector<analysis::Symbol>& symbols, std::string& accessorPropertyType,
                                            const HoverQueryContext& ctx)
{
    const int accessorMode = ctx.request.config ? ctx.request.config->engine.propertyAccessorMode : 2;
    if (accessorMode >= 2)
    {
        symbols = analysis::FindGlobalPropertyAccessors(ctx.nodeText, ctx.request.symbolTable, accessorMode == 3);
        accessorPropertyType = analysis::PropertyTypeFromAccessors(symbols);
    }
}

static std::optional<lsp::Hover> TryHoverDelegatePropertyFallback(const HoverQueryContext& ctx)
{
    const auto* engineRules = ctx.request.config ? &ctx.request.config->engineRules : nullptr;
    if (!analysis::IsBaseClassDelegateProperty(ctx.nodeText, ctx.request.symbolTable, engineRules))
    {
        return std::nullopt;
    }
    std::string baseClass =
        analysis::ResolveDelegateTargetBaseClass({ctx.node, ctx.request.sourceCode, ctx.request.symbolTable,
                                                  ctx.vctx.isVirtual ? ctx.vctx.hostClass : "", engineRules});
    if (!baseClass.empty())
    {
        std::string text = "```angelscript\n" + baseClass + "@ " + ctx.nodeText + "\n```";
        return lsp::Hover{lsp::MarkupContent{lsp::MarkupKindEnum(lsp::MarkupKind::Markdown), std::move(text)},
                          ctx.range};
    }
    return std::nullopt;
}

static std::optional<lsp::Hover> TryHoverSymbolCandidates(HoverQueryContext& ctx)
{
    utils::HighResTimer symTimer;
    const auto gsq = parser::GetGlobalScopeQualifier(ctx.node, ctx.request.sourceCode);
    std::vector<analysis::Symbol> symbols = CollectScopedSymbols(ctx);
    if (!gsq.isGlobal && !IsQualifiedMemberNode(ctx.node, ctx.request.sourceCode))
    {
        AppendEnclosingClassMethods(ctx.node, ctx.nodeText, ctx.request, symbols);
    }
    DisambiguateContainerSymbols(ctx, symbols);
    DisambiguateTypeContextSymbols(ctx, symbols);
    AppendConstructorSymbolsIfCall(ctx, symbols);

    std::string accessorPropertyType;
    if (!gsq.isGlobal)
    {
        AppendVirtualHostSymbols(ctx, symbols, accessorPropertyType);
    }

    FilterDirectRootGlobalSymbols(symbols, gsq);

    if (symbols.empty() && ctx.rootScope && !gsq.isGlobal)
    {
        if (auto fallbackHover = TryHoverScopeFallback(ctx))
        {
            ctx.profiler.symMs += symTimer.ElapsedMs();
            return fallbackHover;
        }
    }

    if (symbols.empty() && accessorPropertyType.empty() && !gsq.isGlobal)
    {
        PopulateGlobalPropertyAccessors(symbols, accessorPropertyType, ctx);
    }

    if (symbols.empty() && !gsq.isGlobal)
    {
        if (auto specialHover = TryHoverDelegatePropertyFallback(ctx))
        {
            return specialHover;
        }
    }

    ctx.profiler.symMs += symTimer.ElapsedMs();
    if (symbols.empty())
    {
        return std::nullopt;
    }

    return FormatSymbolsHover(symbols, accessorPropertyType, ctx);
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
