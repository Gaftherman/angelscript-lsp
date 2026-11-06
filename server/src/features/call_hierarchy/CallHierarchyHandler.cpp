#include "features/call_hierarchy/CallHierarchyHandler.h"

#include "analysis/resolution/SemanticHelpers.h"
#include "features/common/FeatureSharedUtils.h"
#include "utils/Constants.h"
#include "utils/ScopedNameUtils.h"
#include "utils/Utils.h"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace angel_lsp::features
{

/**
 * @brief Converts an analysis symbol into an LSP CallHierarchyItem.
 * @param[in] sym Analysis symbol representing a function or method.
 * @return Constructed CallHierarchyItem populated with name, range, and URI.
 */
static lsp::CallHierarchyItem ToItem(const analysis::Symbol& sym)
{
    lsp::CallHierarchyItem item;
    item.name.assign(analysis::LastScopeSegment(sym.name));
    item.kind = sym.containerName.empty() ? lsp::SymbolKind::Function : lsp::SymbolKind::Method;
    item.uri = lsp::DocumentUri::parse(sym.fileUri);

    const bool hasFullRange = sym.fullRange.endLine != 0 || sym.fullRange.endCharacter != 0;
    item.range = hasFullRange ? common::ToLspRange(sym.fullRange)
                              : lsp::Range{lsp::Position{sym.startLine, sym.startCharacter},
                                           lsp::Position{sym.endLine, sym.endCharacter}};

    const bool hasSelection = sym.selectionRange.endLine != 0 || sym.selectionRange.endCharacter != 0;
    item.selectionRange = hasSelection ? common::ToLspRange(sym.selectionRange)
                                       : lsp::Range{lsp::Position{sym.startLine, sym.startCharacter},
                                                    lsp::Position{sym.endLine, sym.endCharacter}};

    const std::string_view qualified = sym.qualifiedName.empty() ? sym.name : sym.qualifiedName;
    item.detail = lsp::String(qualified);
    item.data = lsp::LSPAny(lsp::String(qualified));
    return item;
}

/**
 * @brief The qualified name a follow-up request is about, from the item it was handed.
 * @param[in] item Item echoed back by the client; the returned view borrows from it.
 * @return Qualified name stored in the item's data payload, or the item's display name when absent.
 */
static std::string_view QualifiedNameOf(const lsp::CallHierarchyItem& item)
{
    if (item.data.has_value() && item.data->isString())
    {
        return item.data->string();
    }
    return item.name;
}

static bool IsFunctionSymbol(const analysis::Symbol& sym)
{
    return sym.type == analysis::SymbolType::Function &&
           std::holds_alternative<analysis::FunctionSignature>(sym.signature);
}

/**
 * @brief Function declarations whose qualified name is exactly this.
 */
static std::vector<analysis::Symbol> FindByQualifiedName(std::string_view qualifiedName,
                                                         const analysis::SymbolTable& table)
{
    std::vector<analysis::Symbol> found;
    const auto symbols = table.FindSymbolsPtr(qualifiedName);
    if (!symbols)
    {
        return found;
    }
    for (const auto& sym : *symbols)
    {
        if (IsFunctionSymbol(sym))
        {
            found.push_back(sym);
        }
    }
    return found;
}

/**
 * @brief Function declarations whose bare name is this, wherever they were declared.
 */
static std::vector<analysis::Symbol> FindByBareName(std::string_view bareName, const analysis::SymbolTable& table)
{
    std::vector<analysis::Symbol> found;
    if (bareName.empty())
    {
        return found;
    }

    table.ForEachSymbol(
        [&]([[maybe_unused]] std::string_view qualifiedName, const std::vector<analysis::Symbol>& symbols)
        {
            for (const auto& sym : symbols)
            {
                if (IsFunctionSymbol(sym) && analysis::LastScopeSegment(sym.name) == bareName)
                {
                    found.push_back(sym);
                }
            }
        });
    return found;
}

/**
 * @brief Finds the innermost function whose body encloses the given document line.
 * @param[in] table Workspace symbol table.
 * @param[in] uri Document URI to search.
 * @param[in] line 0-based document line.
 * @return Owned copy of the tightest enclosing function declaration, or nullopt when the line is outside any function.
 */
static std::optional<analysis::Symbol> FindEnclosingFunction(const analysis::SymbolTable& table, std::string_view uri,
                                                             uint32_t line)
{
    std::optional<analysis::Symbol> bestFunction;
    uint32_t bestSpan = UINT32_MAX;
    table.ForEachSymbolInFile(
        uri,
        [&]([[maybe_unused]] std::string_view key, const std::vector<analysis::Symbol>& symbols)
        {
            for (const auto& sym : symbols)
            {
                if (sym.type == analysis::SymbolType::Function &&
                    (sym.fileUri == uri || utils::PathToUri(sym.fileUri) == uri))
                {
                    const auto range = (sym.fullRange.endLine != 0 || sym.fullRange.endCharacter != 0)
                                           ? sym.fullRange
                                           : sym.selectionRange;
                    const uint32_t sL =
                        (range.endLine != 0 || range.endCharacter != 0) ? range.startLine : sym.startLine;
                    const uint32_t eL = (range.endLine != 0 || range.endCharacter != 0) ? range.endLine : sym.endLine;
                    if (line >= sL && line <= eL)
                    {
                        const uint32_t span = eL - sL;
                        if (span <= bestSpan)
                        {
                            bestSpan = span;
                            bestFunction = sym;
                        }
                    }
                }
            }
        });
    return bestFunction;
}

/**
 * @brief Locate outgoing calls from a caller, falling back to synthesized host class origin mixin.
 */
static std::vector<analysis::DocumentCalls> FindCallsFromCaller(std::string_view caller,
                                                                const analysis::CallGraphIndex& callGraph,
                                                                const analysis::SymbolTable& symbolTable)
{
    auto calls = callGraph.FindCallsFrom(caller);
    if (!calls.empty())
    {
        return calls;
    }

    const auto symbols = symbolTable.FindSymbolsPtr(caller);
    if (!symbols)
    {
        return calls;
    }

    for (const auto& sym : *symbols)
    {
        if (sym.isSynthesized && !sym.containerName.empty())
        {
            const auto originCaller = utils::JoinScopedName(sym.containerName, sym.name);
            calls = callGraph.FindCallsFrom(originCaller);
            if (!calls.empty())
            {
                break;
            }
        }
    }
    return calls;
}

/**
 * @brief Group call ranges by callee name.
 * @param[in] calls Outgoing call records; the returned callee names are views into them.
 * @return Callee name and call-site ranges, in first-seen order.
 */
static std::vector<std::pair<std::string_view, std::vector<lsp::Range>>>
GroupCallsByCallee(const std::vector<analysis::DocumentCalls>& calls)
{
    std::vector<std::pair<std::string_view, std::vector<lsp::Range>>> byCallee;
    for (const auto& document : calls)
    {
        for (const auto& call : document.calls)
        {
            auto existing = std::find_if(byCallee.begin(), byCallee.end(),
                                         [&call](const auto& entry) { return entry.first == call.callee; });
            if (existing == byCallee.end())
            {
                byCallee.emplace_back(call.callee, std::vector<lsp::Range>{common::ToLspRange(call.range)});
            }
            else
            {
                existing->second.push_back(common::ToLspRange(call.range));
            }
        }
    }
    return byCallee;
}

static bool OutgoingCallExists(const std::vector<lsp::CallHierarchyOutgoingCall>& outgoing, const analysis::Symbol& sym)
{
    for (const auto& out : outgoing)
    {
        if (out.to.uri.toString() == sym.fileUri && out.to.selectionRange.start.line == sym.selectionRange.startLine &&
            out.to.selectionRange.start.character == sym.selectionRange.startCharacter)
        {
            return true;
        }
    }
    return false;
}

static void AppendOutgoingCallsForCallee(std::string_view callee, const std::vector<lsp::Range>& ranges,
                                         const analysis::SymbolTable& symbolTable,
                                         std::vector<lsp::CallHierarchyOutgoingCall>& outgoing)
{
    for (const auto& sym : FindByBareName(callee, symbolTable))
    {
        if (!OutgoingCallExists(outgoing, sym))
        {
            lsp::CallHierarchyOutgoingCall entry;
            entry.to = ToItem(sym);
            entry.fromRanges = ranges;
            outgoing.push_back(std::move(entry));
        }
    }
}

static bool SameDeclarationLocation(const analysis::Symbol& a, const analysis::Symbol& b)
{
    return a.fileUri == b.fileUri && a.startLine == b.startLine && a.startCharacter == b.startCharacter;
}

std::optional<std::vector<lsp::CallHierarchyItem>> PrepareCallHierarchy(const CallHierarchyPrepareRequest& request)
{
    if (request.sourceCode.empty())
    {
        return std::nullopt;
    }

    const std::string_view name = common::FindIdentifierAtPosition(request.sourceCode, request.position);

    std::vector<analysis::Symbol> declarations;
    if (!name.empty())
    {
        declarations = FindByBareName(name, request.symbolTable);
    }

    if (declarations.empty())
    {
        if (const auto enclosing = FindEnclosingFunction(request.symbolTable, request.uri, request.position.line))
        {
            declarations = FindByQualifiedName(
                enclosing->qualifiedName.empty() ? enclosing->name : enclosing->qualifiedName, request.symbolTable);
        }
    }

    if (declarations.empty())
    {
        return std::nullopt;
    }

    std::vector<analysis::Symbol> uniqueDeclarations;
    for (const auto& sym : declarations)
    {
        auto it = std::find_if(uniqueDeclarations.begin(), uniqueDeclarations.end(),
                               [&sym](const analysis::Symbol& u) { return SameDeclarationLocation(u, sym); });
        if (it == uniqueDeclarations.end())
        {
            uniqueDeclarations.push_back(sym);
        }
        else if (it->isSynthesized && !sym.isSynthesized)
        {
            *it = sym;
        }
    }

    std::vector<lsp::CallHierarchyItem> items;
    items.reserve(uniqueDeclarations.size());
    for (const auto& sym : uniqueDeclarations)
    {
        items.push_back(ToItem(sym));
    }
    return items;
}

std::optional<std::vector<lsp::CallHierarchyIncomingCall>> GetIncomingCalls(const CallHierarchyItemRequest& request)
{
    const std::string_view target = analysis::LastScopeSegment(request.item.name);
    if (target.empty())
    {
        return std::nullopt;
    }

    // Caller names are views into callsTo, which outlives byCaller.
    const auto callsTo = request.callGraph.FindCallsTo(target);
    std::vector<std::pair<std::string_view, std::vector<lsp::Range>>> byCaller;

    for (const auto& document : callsTo)
    {
        for (const auto& call : document.calls)
        {
            if (call.caller.empty())
            {
                continue;
            }

            auto existing = std::find_if(byCaller.begin(), byCaller.end(),
                                         [&call](const auto& entry) { return entry.first == call.caller; });
            if (existing == byCaller.end())
            {
                byCaller.emplace_back(call.caller, std::vector<lsp::Range>{common::ToLspRange(call.range)});
            }
            else
            {
                existing->second.push_back(common::ToLspRange(call.range));
            }
        }
    }

    std::vector<lsp::CallHierarchyIncomingCall> incoming;
    for (auto& [caller, ranges] : byCaller)
    {
        for (const auto& sym : FindByQualifiedName(caller, request.symbolTable))
        {
            bool exists = false;
            for (const auto& inc : incoming)
            {
                if (inc.from.uri.toString() == sym.fileUri &&
                    inc.from.selectionRange.start.line == sym.selectionRange.startLine &&
                    inc.from.selectionRange.start.character == sym.selectionRange.startCharacter)
                {
                    exists = true;
                    break;
                }
            }
            if (!exists)
            {
                lsp::CallHierarchyIncomingCall entry;
                entry.from = ToItem(sym);
                entry.fromRanges = ranges;
                incoming.push_back(std::move(entry));
            }
        }
    }

    return incoming.empty() ? std::nullopt : std::optional{incoming};
}

std::optional<std::vector<lsp::CallHierarchyOutgoingCall>> GetOutgoingCalls(const CallHierarchyItemRequest& request)
{
    const std::string_view caller = QualifiedNameOf(request.item);
    if (caller.empty())
    {
        return std::nullopt;
    }

    const auto calls = FindCallsFromCaller(caller, request.callGraph, request.symbolTable);
    const auto byCallee = GroupCallsByCallee(calls);

    std::vector<lsp::CallHierarchyOutgoingCall> outgoing;
    for (const auto& [callee, ranges] : byCallee)
    {
        AppendOutgoingCallsForCallee(callee, ranges, request.symbolTable, outgoing);
    }

    return outgoing.empty() ? std::nullopt : std::optional{outgoing};
}
} // namespace angel_lsp::features
