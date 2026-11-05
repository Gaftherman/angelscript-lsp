#include "features/call_hierarchy/CallHierarchyHandler.h"

#include "analysis/resolution/SemanticHelpers.h"
#include "features/common/FeatureSharedUtils.h"
#include "utils/Constants.h"
#include "utils/ScopedNameUtils.h"
#include "utils/Utils.h"

#include <algorithm>
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
    item.name = std::string(analysis::LastScopeSegment(sym.name));
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

    const std::string qualified = sym.qualifiedName.empty() ? sym.name : sym.qualifiedName;
    item.detail = qualified;
    item.data = lsp::LSPAny(std::string(qualified));
    return item;
}

/**
 * @brief The qualified name a follow-up request is about, from the item it was handed.
 */
static std::string QualifiedNameOf(const lsp::CallHierarchyItem& item)
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
static std::vector<analysis::Symbol> FindByQualifiedName(const std::string& qualifiedName,
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
static std::vector<analysis::Symbol> FindByBareName(const std::string& bareName, const analysis::SymbolTable& table)
{
    std::vector<analysis::Symbol> found;
    if (bareName.empty())
    {
        return found;
    }

    table.ForEachSymbol(
        [&]([[maybe_unused]] const std::string& qualifiedName, const std::vector<analysis::Symbol>& symbols)
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
 * @brief Finds the function whose body encloses the given document line.
 */
static std::string FindEnclosingFunction(const analysis::SymbolTable& table, std::string_view uri, uint32_t line)
{
    std::string bestFunction;
    uint32_t bestSpan = UINT32_MAX;
    table.ForEachSymbolInFile(
        std::string(uri),
        [&]([[maybe_unused]] const std::string& key, const std::vector<analysis::Symbol>& symbols)
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
                            bestFunction = sym.qualifiedName.empty() ? sym.name : sym.qualifiedName;
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
static std::vector<analysis::DocumentCalls> FindCallsFromCaller(const std::string& caller,
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
            const std::string originCaller = utils::JoinScopedName(sym.containerName, sym.name);
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
 */
static std::vector<std::pair<std::string, std::vector<lsp::Range>>>
GroupCallsByCallee(const std::vector<analysis::DocumentCalls>& calls)
{
    std::vector<std::pair<std::string, std::vector<lsp::Range>>> byCallee;
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

static void AppendOutgoingCallsForCallee(const std::string& callee, const std::vector<lsp::Range>& ranges,
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

    const std::string name = common::FindIdentifierAtPosition(request.sourceCode, request.position);

    std::vector<analysis::Symbol> declarations;
    if (!name.empty())
    {
        declarations = FindByBareName(name, request.symbolTable);
    }

    if (declarations.empty())
    {
        const std::string enclosing = FindEnclosingFunction(request.symbolTable, request.uri, request.position.line);
        if (!enclosing.empty())
        {
            declarations = FindByQualifiedName(enclosing, request.symbolTable);
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
    const std::string target{analysis::LastScopeSegment(request.item.name)};
    if (target.empty())
    {
        return std::nullopt;
    }

    std::vector<std::pair<std::string, std::vector<lsp::Range>>> byCaller;

    for (const auto& document : request.callGraph.FindCallsTo(target))
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
    const std::string caller = QualifiedNameOf(request.item);
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
