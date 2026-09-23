#include "features/definition/DefinitionHandler.h"
#include "analysis/OverloadResolver.h"
#include "analysis/SemanticHelpers.h"
#include "parser/GrammarNames.h"
#include "utils/Utils.h"
#include <algorithm>
#include <cmath>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace angel_lsp::features
{
namespace
{

/**
 * @brief Checks if a tree-sitter node type matches a definition target.
 * @param[in] nodeType Tree-sitter node type string.
 * @return True if node is an identifier, primitive type, or scoped identifier.
 */
inline bool IsTargetNodeType(std::string_view nodeType)
{
    return nodeType == "identifier" || nodeType == "primitive_type" || nodeType == "scoped_identifier";
}

/**
 * @brief Attempts to resolve an identifier node preceding the cursor by one character.
 * @param[in] rootNode Root AST node.
 * @param[in] pos Cursor position.
 * @return AST node if preceding character lies on an identifier, or null node.
 */
TSNode ResolvePrecedingNodeIfIdentifier(TSNode rootNode, lsp::Position pos)
{
    if (pos.character == 0)
    {
        return TSNode{};
    }
    TSPoint prevPoint = {pos.line, pos.character - 1};
    TSNode prevNode = ts_node_descendant_for_point_range(rootNode, prevPoint, prevPoint);
    if (!ts_node_is_null(prevNode) && IsTargetNodeType(ts_node_type(prevNode)))
    {
        return prevNode;
    }
    return TSNode{};
}

/**
 * @brief Safely extracts source code substring corresponding to an AST node.
 * @param[in] node AST node.
 * @param[in] sourceCode Document source string.
 * @return Extracted substring, or empty string on range error.
 */
std::string ExtractValidNodeText(TSNode node, const std::string& sourceCode)
{
    uint32_t startByte = ts_node_start_byte(node);
    uint32_t endByte = ts_node_end_byte(node);
    if (startByte < sourceCode.size() && endByte <= sourceCode.size() && startByte < endByte)
    {
        return sourceCode.substr(startByte, endByte - startByte);
    }
    return "";
}

/**
 * @brief Resolves identifier text and AST node at cursor position.
 * @param[in] request Definition request context.
 * @param[out] outNode Resolved AST node.
 * @return Identifier string at cursor, or empty string if not on an identifier.
 */
std::string GetNodeTextAt(const DefinitionRequest& request, TSNode& outNode)
{
    if (!request.tree || request.sourceCode.empty())
    {
        return "";
    }

    TSNode rootNode = ts_tree_root_node(request.tree);
    TSPoint point = {request.position.line, request.position.character};
    TSNode node = ts_node_descendant_for_point_range(rootNode, point, point);

    if (ts_node_is_null(node))
    {
        return "";
    }

    if (!IsTargetNodeType(ts_node_type(node)))
    {
        TSNode prevNode = ResolvePrecedingNodeIfIdentifier(rootNode, request.position);
        if (!ts_node_is_null(prevNode))
        {
            node = prevNode;
        }
    }

    if (!IsTargetNodeType(ts_node_type(node)))
    {
        return "";
    }

    outNode = node;
    return ExtractValidNodeText(node, request.sourceCode);
}


/**
 * @brief Locates the enclosing call expression node if target node represents the callee.
 * @param[in] node Target identifier node.
 * @return Enclosing call_expression node, or null node.
 */
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
 * @brief Finds the argument_list node within a call_expression.
 * @param[in] callNode Call expression node.
 * @return argument_list node, or null node.
 */
TSNode FindCallArgumentListNode(TSNode callNode)
{
    TSNode argListNode = parser::GetChildByField(callNode, parser::fields::Arguments);
    if (!ts_node_is_null(argListNode))
    {
        return argListNode;
    }
    for (uint32_t i = 0; i < ts_node_child_count(callNode); ++i)
    {
        TSNode ch = ts_node_child(callNode, i);
        if (std::string_view(ts_node_type(ch)) == "argument_list")
        {
            return ch;
        }
    }
    return TSNode{};
}

/**
 * @brief Resolves argument type names for all arguments in a call expression.
 * @param[in] callNode Enclosing call expression node.
 * @param[in] scope Lexical scope for expression resolution.
 * @param[in] request Definition request context.
 * @return Vector of deduced argument type strings.
 */
std::vector<std::string> ExtractCallArgumentTypes(TSNode callNode, const analysis::Scope* scope,
                                                  const DefinitionRequest& request)
{
    TSNode argListNode = FindCallArgumentListNode(callNode);
    if (ts_node_is_null(argListNode))
    {
        return {};
    }
    std::vector<std::string> argTypes;
    uint32_t count = ts_node_child_count(argListNode);
    for (uint32_t i = 0; i < count; ++i)
    {
        TSNode ch = ts_node_child(argListNode, i);
        std::string_view ct = ts_node_type(ch);
        if (ct == "(" || ct == ")" || ct == "," || ct == "comment" || ct == ":")
        {
            continue;
        }
        const char* fieldName = ts_node_field_name_for_child(argListNode, i);
        if (fieldName && std::string_view(fieldName) == "arg_name")
        {
            continue;
        }
        argTypes.push_back(
            analysis::ResolveExpressionType(ch, {scope, request.symbolTable, request.sourceCode, request.uri}));
    }
    return argTypes;
}

/**
 * @brief Checks whether a function signature's parameter bounds accommodate the call arity.
 * @param[in] sig Function signature information.
 * @param[in] argCount Call site argument count.
 * @return True if arity is within min/max bounds.
 */
bool MatchesCallArity(const analysis::FunctionSignature& sig, uint32_t argCount)
{
    uint32_t requiredParams = 0;
    uint32_t maxParams = 0;
    bool isVariadic = false;
    for (const auto& param : sig.parameters)
    {
        if (param.rawText.find("...") != std::string::npos)
        {
            isVariadic = true;
            continue;
        }
        ++maxParams;
        if (param.defaultValue.empty())
        {
            ++requiredParams;
        }
    }
    return (argCount >= requiredParams) && (isVariadic || argCount <= maxParams);
}

/**
 * @brief Checks if any candidate function matches the specified call argument count.
 * @param[in] candidates Candidate symbols to inspect.
 * @param[in] argCount Call site argument count.
 * @return True if at least one candidate matches arity bounds.
 */
bool HasArityMatch(const std::vector<analysis::Symbol>& candidates, uint32_t argCount)
{
    for (const auto& sym : candidates)
    {
        if (std::holds_alternative<analysis::FunctionSignature>(sym.signature))
        {
            if (MatchesCallArity(sym.GetFunction(), argCount))
            {
                return true;
            }
        }
    }
    return false;
}

/**
 * @brief Appends found symbols to candidates list if not already present.
 * @param[in] foundSymbols Newly located symbols.
 * @param[in,out] funcCandidates Destination candidate list.
 */
void AppendUniqueFunctionCandidates(const std::vector<analysis::Symbol>& foundSymbols,
                                    std::vector<analysis::Symbol>& funcCandidates)
{
    for (const auto& sym : foundSymbols)
    {
        if (sym.type == analysis::SymbolType::Function &&
            std::holds_alternative<analysis::FunctionSignature>(sym.signature))
        {
            if (std::none_of(funcCandidates.begin(), funcCandidates.end(),
                             [&](const analysis::Symbol& existing)
                             {
                                 return existing.qualifiedName == sym.qualifiedName &&
                                        analysis::HasSameParameterList(existing, sym);
                             }))
            {
                funcCandidates.push_back(sym);
            }
        }
    }
}

/**
 * @brief Collects inherited and mixin overloads from enclosing classes when arity match fails.
 * @param[in] node AST node at cursor.
 * @param[in] request Definition request context.
 * @param[in,out] funcCandidates Function candidate list to populate.
 */
void CollectClassHierarchyOverloads(TSNode node, const DefinitionRequest& request,
                                    std::vector<analysis::Symbol>& funcCandidates)
{
    std::string targetMethodName = ts_node_is_null(node) ? "" : analysis::GetNodeText(node, request.sourceCode);
    auto containers = analysis::GetEnclosingContainers(node, request.sourceCode);
    for (const auto& c : containers)
    {
        if (c.kind != analysis::ContainerKind::Class)
        {
            continue;
        }
        std::string classKey = c.qualifiedName.empty() ? c.name : c.qualifiedName;
        auto hierarchy = analysis::GetInheritedTypeHierarchy(classKey, request.symbolTable);
        for (const auto& cls : hierarchy)
        {
            if (auto found = request.symbolTable.FindSymbolsPtr(cls + "::" + targetMethodName))
            {
                AppendUniqueFunctionCandidates(*found, funcCandidates);
            }
        }
        if (auto hostSyms = request.symbolTable.FindSymbolsPtr(classKey))
        {
            for (const auto& hs : *hostSyms)
            {
                if (hs.type == analysis::SymbolType::Class &&
                    std::holds_alternative<analysis::ClassSignature>(hs.signature))
                {
                    for (const auto& mixinName : hs.GetClass().includedMixins)
                    {
                        if (auto mixinMethods = request.symbolTable.FindSymbolsPtr(mixinName + "::" + targetMethodName))
                        {
                            AppendUniqueFunctionCandidates(*mixinMethods, funcCandidates);
                        }
                    }
                }
            }
        }
        break;
    }
}

/**
 * @brief Scores a function candidate against call site arguments for fallback matching.
 * @param[in] sig Function signature.
 * @param[in] argTypes Deduced call site argument types.
 * @param[in] symbolTable Symbol table for type scoring.
 * @return Heuristic match score.
 */
int ScoreCandidateFallback(const analysis::FunctionSignature& sig, const std::vector<std::string>& argTypes,
                           const analysis::SymbolTable& symbolTable)
{
    const uint32_t argCount = static_cast<uint32_t>(argTypes.size());
    int score = 0;
    if (MatchesCallArity(sig, argCount))
    {
        score += 100;
        if (argCount == sig.parameters.size())
        {
            score += 50;
        }
    }
    else
    {
        int diff = std::abs(static_cast<int>(argCount) - static_cast<int>(sig.parameters.size()));
        score -= diff * 20;
    }

    for (size_t i = 0; i < argTypes.size() && i < sig.parameters.size(); ++i)
    {
        if (!argTypes[i].empty())
        {
            int pScore = analysis::ScoreArgumentMatch(argTypes[i], sig.parameters[i], symbolTable);
            if (pScore < 999)
            {
                score += 10;
            }
        }
    }
    return score;
}

/**
 * @brief Selects the best fallback function candidate based on arity and argument scores.
 * @param[in] funcCandidates List of function candidates.
 * @param[in] argTypes Call site argument types.
 * @param[in] symbolTable Symbol table for type evaluation.
 * @return Pointer to best matching symbol, or nullptr if none score positively.
 */
const analysis::Symbol* FindBestFallbackCandidate(const std::vector<analysis::Symbol>& funcCandidates,
                                                  const std::vector<std::string>& argTypes,
                                                  const analysis::SymbolTable& symbolTable)
{
    const analysis::Symbol* bestFallback = nullptr;
    int bestFallbackScore = -10000;
    for (const auto& sym : funcCandidates)
    {
        if (!std::holds_alternative<analysis::FunctionSignature>(sym.signature))
        {
            continue;
        }
        int score = ScoreCandidateFallback(sym.GetFunction(), argTypes, symbolTable);
        if (score > bestFallbackScore)
        {
            bestFallbackScore = score;
            bestFallback = &sym;
        }
    }
    return (bestFallback != nullptr && bestFallbackScore > 0) ? bestFallback : nullptr;
}

/**
 * @brief Filters candidate symbols for a call site down to the best matching overload.
 * @param[in] node AST node at cursor.
 * @param[in] candidates Initial candidate symbols.
 * @param[in] request Definition request context.
 * @param[in] scope Lexical scope at cursor.
 * @return Filtered vector containing the resolved overload, or original candidates.
 */
std::vector<analysis::Symbol> FilterOverloadsForCall(TSNode node, const std::vector<analysis::Symbol>& candidates,
                                                     const DefinitionRequest& request, const analysis::Scope* scope)
{
    if (candidates.empty())
    {
        return candidates;
    }
    bool hasFunction = std::any_of(candidates.begin(), candidates.end(),
                                   [](const auto& s) { return s.type == analysis::SymbolType::Function; });
    if (!hasFunction)
    {
        return candidates;
    }

    TSNode callNode = FindEnclosingCallNode(node);
    if (ts_node_is_null(callNode))
    {
        return candidates;
    }

    std::vector<std::string> argTypes = ExtractCallArgumentTypes(callNode, scope, request);
    const uint32_t argCount = static_cast<uint32_t>(argTypes.size());

    std::vector<analysis::Symbol> funcCandidates;
    for (const auto& sym : candidates)
    {
        if (sym.type == analysis::SymbolType::Function)
        {
            funcCandidates.push_back(sym);
        }
    }

    bool hasArityMatch = HasArityMatch(funcCandidates, argCount);
    if (!hasArityMatch)
    {
        CollectClassHierarchyOverloads(node, request, funcCandidates);
    }

    if (funcCandidates.size() <= 1 && hasArityMatch)
    {
        return candidates;
    }

    auto match = analysis::ResolveBestOverload(funcCandidates, argTypes, request.symbolTable);
    if (match.bestCandidate != nullptr)
    {
        return {*match.bestCandidate};
    }

    if (const analysis::Symbol* fallback = FindBestFallbackCandidate(funcCandidates, argTypes, request.symbolTable))
    {
        return {*fallback};
    }

    for (const auto& sym : funcCandidates)
    {
        if (std::holds_alternative<analysis::FunctionSignature>(sym.signature) &&
            sym.GetFunction().parameters.size() == argCount)
        {
            return {sym};
        }
    }

    return candidates;
}

/**
 * @brief Converts a Symbol into an LSP Location, routing to virtual mixin documents if enabled.
 * @param[in] sym Resolved symbol.
 * @param[in] request Definition context containing the symbol table.
 * @return Formatted LSP Location with appropriate URI and mapped line range.
 */
lsp::Location MakeLocation(const analysis::Symbol& sym, const DefinitionRequest& request)
{
    if (request.uri.starts_with("angelscript-virtual:") || request.uri.starts_with("angelscript-virtual://"))
    {
        std::string mixinPart = analysis::SymbolTable::ExtractVirtualMixinName(request.uri);

        const analysis::Symbol* mSym = nullptr;
        auto cand = request.symbolTable.FindSymbols(mixinPart);
        for (const auto& c : cand)
        {
            if (c.type == analysis::SymbolType::Class)
            {
                mSym = &c;
                break;
            }
        }
        if (!mSym)
        {
            std::string shortName = mixinPart;
            auto lastScope = shortName.rfind("::");
            if (lastScope != std::string::npos)
            {
                shortName = shortName.substr(lastScope + 2);
            }
            auto sCand = request.symbolTable.FindTypeSymbolsByShortName(shortName);
            for (const auto& c : sCand)
            {
                if (c.type == analysis::SymbolType::Class)
                {
                    mSym = &c;
                    break;
                }
            }
        }

        if (mSym && sym.fileUri == mSym->fileUri)
        {
            uint32_t mappedStartLine = analysis::SymbolTable::PhysicalToVirtualLine(sym.startLine, mSym->startLine);
            uint32_t lineDiff = sym.endLine >= sym.startLine ? (sym.endLine - sym.startLine) : 0;
            uint32_t mappedEndLine = mappedStartLine + lineDiff;
            return lsp::Location{lsp::DocumentUri::parse(request.uri),
                                 lsp::Range{lsp::Position{mappedStartLine, sym.startCharacter},
                                            lsp::Position{mappedEndLine, sym.endCharacter}}};
        }
    }

    const std::string& targetUri = sym.fileUri.empty() ? request.uri : sym.fileUri;
    return lsp::Location{
        lsp::DocumentUri::parse(targetUri),
        lsp::Range{lsp::Position{sym.startLine, sym.startCharacter}, lsp::Position{sym.endLine, sym.endCharacter}}};
}

/**
 * @brief Extracts the full text view of a specific line from source code.
 * @param[in] sourceCode Entire document string.
 * @param[in] targetLine 0-indexed line number.
 * @return View of line characters, or empty view if out of bounds.
 */
std::string_view ExtractLineAt(const std::string& sourceCode, uint32_t targetLine)
{
    size_t start = 0;
    for (uint32_t current = 0; current < targetLine; ++current)
    {
        const size_t nextBreak = sourceCode.find('\n', start);
        if (nextBreak == std::string::npos)
        {
            return "";
        }
        start = nextBreak + 1;
    }
    const size_t end = sourceCode.find('\n', start);
    const size_t lineEnd = (end == std::string::npos) ? sourceCode.size() : end;
    return std::string_view(sourceCode.data() + start, lineEnd - start);
}

/**
 * @brief Parses include target path from a line if cursor is on an `#include` directive.
 * @param[in] line Source line text.
 * @param[in] character 0-indexed cursor character column.
 * @return Extracted raw path, or empty string if not on include directive.
 */
std::string ParseIncludePathFromLine(std::string_view line, size_t character)
{
    const size_t hash = line.find_first_not_of(" \t");
    if (hash == std::string_view::npos || line[hash] != '#' || line.compare(hash + 1, 7, "include") != 0)
    {
        return "";
    }
    size_t openDelim = line.find_first_of("\"<", hash + 8);
    if (openDelim == std::string_view::npos)
    {
        return "";
    }
    char closeChar = line[openDelim] == '<' ? '>' : '"';
    size_t closeDelim = line.find(closeChar, openDelim + 1);
    if (closeDelim == std::string_view::npos || character < hash || character > closeDelim)
    {
        return "";
    }
    return std::string(line.substr(openDelim + 1, closeDelim - openDelim - 1));
}

/**
 * @brief Attempts to resolve definition when cursor is within an `#include` directive.
 * @param[in] request Definition request context.
 * @return Location vector pointing to the resolved include file, or nullopt.
 */
std::optional<std::vector<lsp::Location>> TryResolveIncludeDirective(const DefinitionRequest& request)
{
    if (!request.resolveInclude)
    {
        return std::nullopt;
    }

    std::string_view line = ExtractLineAt(request.sourceCode, request.position.line);
    if (line.empty())
    {
        return std::nullopt;
    }

    std::string rawPath = ParseIncludePathFromLine(line, static_cast<size_t>(request.position.character));
    if (rawPath.empty())
    {
        return std::nullopt;
    }

    std::string resolved = request.resolveInclude(rawPath);
    if (resolved.empty())
    {
        return std::nullopt;
    }

    lsp::DocumentUri targetUri =
        resolved.rfind("file://", 0) == 0 ? lsp::DocumentUri::parse(resolved) : lsp::Uri::fileUriFromPath(resolved);
    return std::vector<lsp::Location>{lsp::Location{targetUri, lsp::Range{lsp::Position{0, 0}, lsp::Position{0, 0}}}};
}

/**
 * @brief Context for virtual mixin documents.
 */
struct VirtualMixinContext
{
    bool isVirtual = false;
    std::string virtualHostClass;
    std::string virtualMixinName;
    std::optional<analysis::Symbol> virtualMixinSym;
};

/**
 * @brief Resolves virtual mixin context metadata from request URI.
 * @param[in] request Definition request context.
 * @return Populated VirtualMixinContext.
 */
VirtualMixinContext ResolveVirtualMixinContext(const DefinitionRequest& request)
{
    VirtualMixinContext vCtx;
    vCtx.isVirtual =
        request.uri.starts_with("angelscript-virtual:") || request.uri.starts_with("angelscript-virtual://");
    if (!vCtx.isVirtual)
    {
        return vCtx;
    }
    vCtx.virtualHostClass = analysis::SymbolTable::ExtractVirtualHostClass(request.uri);
    vCtx.virtualMixinName = analysis::SymbolTable::ExtractVirtualMixinName(request.uri);

    auto candidates = request.symbolTable.FindSymbolsPtr(vCtx.virtualMixinName);
    if (candidates)
    {
        for (const auto& cand : *candidates)
        {
            if (cand.type == analysis::SymbolType::Class)
            {
                vCtx.virtualMixinSym = cand;
                break;
            }
        }
    }
    if (!vCtx.virtualMixinSym.has_value())
    {
        std::string shortName = vCtx.virtualMixinName;
        auto lastScope = shortName.rfind("::");
        if (lastScope != std::string::npos)
        {
            shortName = shortName.substr(lastScope + 2);
        }
        auto shortCandidates = request.symbolTable.FindTypeSymbolsByShortName(shortName);
        for (const auto& cand : shortCandidates)
        {
            if (cand.type == analysis::SymbolType::Class)
            {
                vCtx.virtualMixinSym = cand;
                break;
            }
        }
    }
    return vCtx;
}

/**
 * @brief Context bundling definition lookup parameters and virtual document state.
 */
struct DefinitionContext
{
    const DefinitionRequest& request;
    VirtualMixinContext vCtx;
    std::shared_ptr<const analysis::Scope> rootScope;
    uint32_t queryLine = 0;
};

/**
 * @brief Constructs a DefinitionContext from a DefinitionRequest.
 * @param[in] request Definition request.
 * @return Initialized DefinitionContext.
 */
DefinitionContext MakeDefinitionContext(const DefinitionRequest& request)
{
    VirtualMixinContext vCtx = ResolveVirtualMixinContext(request);
    auto rootScope = (vCtx.isVirtual && vCtx.virtualMixinSym.has_value())
                         ? request.scopeIndex.GetRoot(vCtx.virtualMixinSym->fileUri)
                         : request.scopeIndex.GetRoot(request.uri);
    uint32_t queryLine =
        (vCtx.isVirtual && vCtx.virtualMixinSym.has_value())
            ? analysis::SymbolTable::VirtualToPhysicalLine(request.position.line, vCtx.virtualMixinSym->startLine)
            : request.position.line;
    return DefinitionContext{request, std::move(vCtx), std::move(rootScope), queryLine};
}

/**
 * @brief Checks if cursor node is the member child of an enclosing member_expression.
 * @param[in] node Cursor node.
 * @return True if node is the member field of a member_expression.
 */
bool IsMemberChildOfExpression(TSNode node)
{
    TSNode parent = ts_node_parent(node);
    if (!ts_node_is_null(parent) && std::string_view(ts_node_type(parent)) == "member_expression")
    {
        TSNode memNode = parser::GetChildByField(parent, parser::fields::Member);
        if (!ts_node_is_null(memNode) &&
            (ts_node_eq(memNode, node) || ts_node_start_byte(memNode) == ts_node_start_byte(node)))
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief Resolves member symbols in the receiver's type hierarchy.
 * @param[in] receiverTypeName Clean receiver class name.
 * @param[in] nodeText Member name identifier.
 * @param[in] symbolTable Global symbol table.
 * @return Vector of matched member symbols.
 */
std::vector<analysis::Symbol> FindMemberSymbolsInHierarchy(const std::string& receiverTypeName,
                                                           const std::string& nodeText,
                                                           const analysis::SymbolTable& symbolTable)
{
    auto hierarchy = analysis::GetInheritedTypeHierarchy(receiverTypeName, symbolTable);
    std::vector<analysis::Symbol> enumFallback;
    for (const auto& typeName : hierarchy)
    {
        std::string qualifiedMember = typeName + "::" + nodeText;
        auto found = symbolTable.FindSymbols(qualifiedMember);
        if (!found.empty())
        {
            std::stable_partition(found.begin(), found.end(), [](const analysis::Symbol& s) {
                if (s.type == analysis::SymbolType::Variable &&
                    std::holds_alternative<analysis::VariableSignature>(s.signature))
                {
                    return !s.GetVariable().isEnumConstant;
                }
                return true;
            });
            const bool hasInstance = std::any_of(found.begin(), found.end(), [](const analysis::Symbol& s) {
                return !(s.type == analysis::SymbolType::Variable &&
                         std::holds_alternative<analysis::VariableSignature>(s.signature) &&
                         s.GetVariable().isEnumConstant);
            });
            if (hasInstance)
            {
                return found;
            }
            if (enumFallback.empty())
            {
                enumFallback = std::move(found);
            }
        }
    }
    for (const auto& typeName : hierarchy)
    {
        auto accessors = analysis::FindPropertyAccessors(typeName, nodeText, symbolTable, false);
        if (!accessors.empty())
        {
            return accessors;
        }
    }
    if (!enumFallback.empty())
    {
        return enumFallback;
    }
    auto directMatches = symbolTable.FindSymbols(receiverTypeName + "::" + nodeText);
    if (!directMatches.empty())
    {
        return directMatches;
    }
    return {};
}

/**
 * @brief Converts matching symbols into LSP Location structures.
 * @param[in] symbols Vector of symbols to convert.
 * @param[in] request Definition request context.
 * @return Formatted LSP Location vector.
 */
std::vector<lsp::Location> ConvertSymbolsToLocations(const std::vector<analysis::Symbol>& symbols,
                                                     const DefinitionRequest& request)
{
    std::vector<lsp::Location> locations;
    for (const auto& sym : symbols)
    {
        if (sym.type != analysis::SymbolType::CallReference)
        {
            auto loc = MakeLocation(sym, request);
            bool duplicate = false;
            for (const auto& existing : locations)
            {
                if (existing.uri.toString() == loc.uri.toString() &&
                    existing.range.start.line == loc.range.start.line &&
                    existing.range.start.character == loc.range.start.character &&
                    existing.range.end.line == loc.range.end.line &&
                    existing.range.end.character == loc.range.end.character)
                {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate)
            {
                locations.push_back(std::move(loc));
            }
        }
    }
    return locations;
}

/**
 * @brief Normalizes receiver type name to a fully qualified symbol table name if available.
 * @param[in] receiverTypeName Initial receiver type name.
 * @param[in] symbolTable Symbol table for type lookup.
 * @return Resolved type name or original input string.
 */
std::string NormalizeReceiverTypeName(std::string receiverTypeName, const analysis::SymbolTable& symbolTable)
{
    if (receiverTypeName.find("::") == std::string::npos && !symbolTable.HasSymbol(receiverTypeName))
    {
        auto shortMatches = symbolTable.FindTypeSymbolsByShortName(receiverTypeName);
        for (const auto& sym : shortMatches)
        {
            if (sym.type == analysis::SymbolType::Class || sym.type == analysis::SymbolType::Interface)
            {
                return sym.qualifiedName.empty() ? sym.name : sym.qualifiedName;
            }
        }
    }
    return receiverTypeName;
}

/**
 * @brief Attempts to resolve member expression definition on receiver object.
 * @param[in] node Cursor node.
 * @param[in] nodeText Member name.
 * @param[in] ctx Definition context.
 * @return Resolved member locations, or nullopt.
 */
std::optional<std::vector<lsp::Location>> TryResolveMemberDefinition(TSNode node, const std::string& nodeText,
                                                                     const DefinitionContext& ctx)
{
    TSNode parent = ts_node_parent(node);
    if (ts_node_is_null(parent) || std::string_view(ts_node_type(parent)) != "member_expression")
    {
        return std::nullopt;
    }

    TSNode objectNode = parser::GetChildByField(parent, parser::fields::Object);
    if (ts_node_is_null(objectNode))
    {
        return std::nullopt;
    }

    const analysis::Scope* scope =
        ctx.rootScope ? FindInnermostScope(ctx.rootScope.get(), ctx.queryLine, ctx.request.position.character)
                      : nullptr;
    std::string receiverTypeName =
        analysis::ResolveReceiverType(objectNode, ctx.request.sourceCode, ctx.request.symbolTable,
                                      {scope, ctx.vCtx.virtualHostClass, ctx.request.uri});

    if (receiverTypeName.empty())
    {
        return std::nullopt;
    }

    receiverTypeName = NormalizeReceiverTypeName(std::move(receiverTypeName), ctx.request.symbolTable);
    auto memberSymbols = FindMemberSymbolsInHierarchy(receiverTypeName, nodeText, ctx.request.symbolTable);
    memberSymbols = FilterOverloadsForCall(node, memberSymbols, ctx.request, scope);

    auto memLocations = ConvertSymbolsToLocations(memberSymbols, ctx.request);
    if (!memLocations.empty())
    {
        return memLocations;
    }
    return std::nullopt;
}

/**
 * @brief Constructs an LSP Location for a local definition, taking virtual line offsets into account.
 * @param[in] def Local definition information.
 * @param[in] ctx Definition context.
 * @return Formatted LSP Location.
 */
lsp::Location MakeLocalDefinitionLocation(const analysis::LocalDefinition& def, const DefinitionContext& ctx)
{
    uint32_t sLine = (def.fullEndLine > 0 || def.fullEndCharacter > 0) ? def.fullStartLine : def.startLine;
    uint32_t sChar = (def.fullEndLine > 0 || def.fullEndCharacter > 0) ? def.fullStartCharacter : def.startCharacter;
    uint32_t eLine = (def.fullEndLine > 0 || def.fullEndCharacter > 0) ? def.fullEndLine : def.endLine;
    uint32_t eChar = (def.fullEndLine > 0 || def.fullEndCharacter > 0) ? def.fullEndCharacter : def.endCharacter;

    if (ctx.vCtx.isVirtual && ctx.vCtx.virtualMixinSym.has_value())
    {
        sLine = analysis::SymbolTable::PhysicalToVirtualLine(sLine, ctx.vCtx.virtualMixinSym->startLine);
        eLine = analysis::SymbolTable::PhysicalToVirtualLine(eLine, ctx.vCtx.virtualMixinSym->startLine);
    }

    return lsp::Location{lsp::DocumentUri::parse(ctx.request.uri),
                         lsp::Range{lsp::Position{sLine, sChar}, lsp::Position{eLine, eChar}}};
}

/**
 * @brief Checks whether a local definition resides inside an enclosing function scope.
 * @param[in] rootScope Root scope of the document.
 * @param[in] def Local definition to check.
 * @return True if definition is declared within a function body.
 */
bool IsDefinitionInsideFunction(const analysis::Scope* rootScope, const analysis::LocalDefinition& def)
{
    const analysis::Scope* declaringScope = analysis::FindScopeDeclaringDefinition(rootScope, def);
    size_t depth = 0;
    ankerl::unordered_dense::set<const analysis::Scope*> visited;
    for (const analysis::Scope* s = declaringScope;
         s != nullptr && visited.insert(s).second && ++depth <= analysis::kMaxScopeDepth;
         s = s->parent)
    {
        if (s->isFunctionScope)
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief Attempts to resolve definition of a local variable or parameter declared in a function.
 * @param[in] nodeText Symbol name.
 * @param[in] ctx Definition context.
 * @return Resolved local definition location, or nullopt.
 */
std::optional<std::vector<lsp::Location>> TryResolveLocalDefinition(const std::string& nodeText,
                                                                    const DefinitionContext& ctx)
{
    if (!ctx.rootScope)
    {
        return std::nullopt;
    }
    const analysis::Scope* scope =
        FindInnermostScope(ctx.rootScope.get(), ctx.queryLine, ctx.request.position.character);
    if (!scope)
    {
        return std::nullopt;
    }
    const analysis::LocalDefinition* def = analysis::ResolveInScope(scope, nodeText);
    if (def &&
        (def->kind == analysis::LocalDefinitionKind::Parameter || def->kind == analysis::LocalDefinitionKind::Variable))
    {
        if (IsDefinitionInsideFunction(ctx.rootScope.get(), *def))
        {
            return std::vector<lsp::Location>{MakeLocalDefinitionLocation(*def, ctx)};
        }
    }
    return std::nullopt;
}

/**
 * @brief Attempts to resolve definition in contextual member or local scopes.
 * @param[in] node Cursor node.
 * @param[in] nodeText Symbol text.
 * @param[in] ctx Definition context.
 * @return Resolved location vector, or nullopt.
 */
std::optional<std::vector<lsp::Location>> TryResolveContextualDefinition(TSNode node, const std::string& nodeText,
                                                                         const DefinitionContext& ctx)
{
    bool isMemberChild = IsMemberChildOfExpression(node);
    if (isMemberChild)
    {
        if (auto memberLocs = TryResolveMemberDefinition(node, nodeText, ctx))
        {
            return memberLocs;
        }
    }
    if (auto localLoc = TryResolveLocalDefinition(nodeText, ctx))
    {
        return localLoc;
    }
    if (!isMemberChild)
    {
        if (auto memberLocs = TryResolveMemberDefinition(node, nodeText, ctx))
        {
            return memberLocs;
        }
    }
    return std::nullopt;
}

/**
 * @brief Finds symbols in scope or global property accessors for an identifier node.
 * @param[in] node Cursor node.
 * @param[in] nodeText Symbol text.
 * @param[in] request Definition request context.
 * @return Vector of matched symbols.
 */
std::vector<analysis::Symbol> FindSymbolsForNode(TSNode node, const std::string& nodeText,
                                                 const DefinitionRequest& request)
{
    std::vector<analysis::Symbol> symbols;
    TSNode parent = ts_node_parent(node);
    if (!ts_node_is_null(parent) && std::string_view(ts_node_type(parent)) == "scoped_identifier")
    {
        uint32_t pStart = ts_node_start_byte(parent);
        uint32_t pEnd = ts_node_end_byte(node);
        if (pStart < request.sourceCode.size() && pEnd <= request.sourceCode.size() && pStart < pEnd)
        {
            std::string scopedText = request.sourceCode.substr(pStart, pEnd - pStart);
            symbols = analysis::FindSymbolsInScope(scopedText, node, request.sourceCode, request.symbolTable);
        }
    }
    if (symbols.empty())
    {
        symbols = analysis::FindSymbolsInScope(nodeText, node, request.sourceCode, request.symbolTable);
    }
    if (symbols.empty())
    {
        symbols = analysis::FindGlobalPropertyAccessors(nodeText, request.symbolTable, false);
    }
    return symbols;
}

/**
 * @brief Finds symbols or accessors from the host class and its type hierarchy.
 * @param[in] hostClass Qualified host class name.
 * @param[in] nodeText Target symbol identifier.
 * @param[in] symbolTable Global symbol table.
 * @return Matched host class symbols.
 */
std::vector<analysis::Symbol> FindHostClassSymbols(const std::string& hostClass, const std::string& nodeText,
                                                   const analysis::SymbolTable& symbolTable)
{
    auto hier = analysis::GetInheritedTypeHierarchy(hostClass, symbolTable);
    for (const auto& cls : hier)
    {
        auto found = symbolTable.FindSymbols(cls + "::" + nodeText);
        if (!found.empty())
        {
            return found;
        }
    }
    for (const auto& cls : hier)
    {
        auto accessors = analysis::FindPropertyAccessors(cls, nodeText, symbolTable, false);
        if (!accessors.empty())
        {
            return accessors;
        }
    }
    return {};
}

/**
 * @brief Appends host class hierarchy symbols when resolving within a virtual mixin document.
 * @param[in,out] symbols Symbol candidate list.
 * @param[in] nodeText Symbol identifier text.
 * @param[in] ctx Definition context.
 */
void AppendHostClassHierarchySymbols(std::vector<analysis::Symbol>& symbols, const std::string& nodeText,
                                     const DefinitionContext& ctx)
{
    if (!ctx.vCtx.isVirtual || ctx.vCtx.virtualHostClass.empty())
    {
        return;
    }
    auto hostSymbols = FindHostClassSymbols(ctx.vCtx.virtualHostClass, nodeText, ctx.request.symbolTable);
    if (hostSymbols.empty())
    {
        return;
    }
    std::vector<analysis::Symbol> containerSymbols;
    for (const auto& s : symbols)
    {
        if (!s.containerName.empty() || s.fileUri == ctx.request.uri)
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

/**
 * @brief Resolves candidate symbols and filters overloads for the cursor call site.
 * @param[in] node AST node at cursor.
 * @param[in] nodeText Symbol name.
 * @param[in] ctx Definition context.
 * @return Filtered vector of candidate symbols.
 */
std::vector<analysis::Symbol> ResolveCandidateSymbols(TSNode node, const std::string& nodeText,
                                                      const DefinitionContext& ctx)
{
    std::vector<analysis::Symbol> symbols = FindSymbolsForNode(node, nodeText, ctx.request);
    AppendHostClassHierarchySymbols(symbols, nodeText, ctx);
    const analysis::Scope* scope =
        ctx.rootScope ? FindInnermostScope(ctx.rootScope.get(), ctx.queryLine, ctx.request.position.character)
                      : nullptr;
    return FilterOverloadsForCall(node, symbols, ctx.request, scope);
}

/**
 * @brief Fallback lookup for definitions in local scope outside of functions (e.g. fields).
 * @param[in] nodeText Symbol name.
 * @param[in] ctx Definition context.
 * @return Resolved definition locations, or nullopt.
 */
std::optional<std::vector<lsp::Location>> TryResolveLocalDefinitionFallback(const std::string& nodeText,
                                                                            const DefinitionContext& ctx)
{
    if (!ctx.rootScope)
    {
        return std::nullopt;
    }
    const analysis::Scope* scope =
        FindInnermostScope(ctx.rootScope.get(), ctx.queryLine, ctx.request.position.character);
    if (!scope)
    {
        return std::nullopt;
    }
    if (const analysis::LocalDefinition* def = analysis::ResolveInScope(scope, nodeText))
    {
        return std::vector<lsp::Location>{MakeLocalDefinitionLocation(*def, ctx)};
    }
    return std::nullopt;
}

/**
 * @brief Resolves definition for the `this` keyword to the enclosing class or interface.
 * @param[in] node AST node at cursor.
 * @param[in] ctx Definition context.
 * @return Locations pointing to the enclosing class or interface, or nullopt.
 */
std::optional<std::vector<lsp::Location>> TryResolveThisKeyword(TSNode node, const DefinitionContext& ctx)
{
    std::string targetClass;
    if (ctx.vCtx.isVirtual && !ctx.vCtx.virtualHostClass.empty())
    {
        targetClass = ctx.vCtx.virtualHostClass;
    }
    else
    {
        auto containers = analysis::GetEnclosingContainers(node, ctx.request.sourceCode);
        for (const auto& c : containers)
        {
            if (c.kind == analysis::ContainerKind::Class || c.kind == analysis::ContainerKind::Interface)
            {
                targetClass = c.qualifiedName.empty() ? c.name : c.qualifiedName;
                break;
            }
        }
    }
    if (targetClass.empty())
    {
        return std::nullopt;
    }
    std::vector<lsp::Location> locations;
    auto cands = ctx.request.symbolTable.FindSymbols(targetClass);
    for (const auto& c : cands)
    {
        if (c.type == analysis::SymbolType::Class || c.type == analysis::SymbolType::Interface)
        {
            locations.push_back(MakeLocation(c, ctx.request));
        }
    }
    if (!locations.empty())
    {
        return locations;
    }
    return std::nullopt;
}

/**
 * @brief Checks if a tree-sitter node type corresponds to an entity declaration.
 * @param[in] nodeType Tree-sitter node type string.
 * @return True if node is a recognized declaration node.
 */
bool IsDeclarationNode(std::string_view nodeType)
{
    static const std::unordered_set<std::string_view> declTypes = {
        parser::nodes::FuncDeclaration,      parser::nodes::VariableDeclarator,   parser::nodes::Parameter,
        parser::nodes::ClassDeclaration,     parser::nodes::InterfaceDeclaration, parser::nodes::InterfaceMethod,
        parser::nodes::EnumDeclaration,      parser::nodes::EnumMember,           parser::nodes::FuncdefDeclaration,
        parser::nodes::NamespaceDeclaration, parser::nodes::ForeachVariable,      parser::nodes::VirtualProperty,
        parser::nodes::MixinDeclaration};
    return declTypes.contains(nodeType);
}

/**
 * @brief Fallback when cursor is on a declaration: returns its own range so editor highlights it.
 * @param[in] node AST node at cursor.
 * @param[in] ctx Definition context.
 * @return Location for declaration, or nullopt.
 */
std::optional<std::vector<lsp::Location>> TryResolveDeclarationFallback(TSNode node, const DefinitionContext& ctx)
{
    if (ts_node_is_null(node))
    {
        return std::nullopt;
    }
    TSNode p = ts_node_parent(node);
    if (ts_node_is_null(p))
    {
        return std::nullopt;
    }
    if (std::string_view(ts_node_type(p)) == parser::nodes::ScopedIdentifier)
    {
        TSNode gp = ts_node_parent(p);
        if (!ts_node_is_null(gp))
        {
            p = gp;
        }
    }
    if (!IsDeclarationNode(ts_node_type(p)))
    {
        return std::nullopt;
    }
    TSPoint s = ts_node_start_point(node);
    TSPoint e = ts_node_end_point(node);
    uint32_t sLine = s.row;
    uint32_t sChar = s.column;
    uint32_t eLine = e.row;
    uint32_t eChar = e.column;
    if (ctx.vCtx.isVirtual && ctx.vCtx.virtualMixinSym.has_value())
    {
        sLine = analysis::SymbolTable::PhysicalToVirtualLine(sLine, ctx.vCtx.virtualMixinSym->startLine);
        eLine = analysis::SymbolTable::PhysicalToVirtualLine(eLine, ctx.vCtx.virtualMixinSym->startLine);
    }
    return std::vector<lsp::Location>{
        lsp::Location{lsp::DocumentUri::parse(ctx.request.uri),
                      lsp::Range{lsp::Position{sLine, sChar}, lsp::Position{eLine, eChar}}}};
}

/**
 * @brief Resolves type name from local scope definition.
 * @param[in] nodeText Symbol name under cursor.
 * @param[in] request Definition request context.
 * @return Clean base type name, or empty string if not found.
 */
std::string ResolveLocalScopeTypeName(const std::string& nodeText, const DefinitionRequest& request)
{
    auto rootScope = request.scopeIndex.GetRoot(request.uri);
    if (!rootScope)
    {
        return "";
    }
    const analysis::Scope* scope =
        FindInnermostScope(rootScope.get(), request.position.line, request.position.character);
    if (!scope)
    {
        return "";
    }
    const analysis::LocalDefinition* def = analysis::ResolveInScope(scope, nodeText);
    if (def && !def->typeName.empty())
    {
        return analysis::CleanBaseType(def->typeName);
    }
    return "";
}

/**
 * @brief Resolves type name from global symbol table entries matching symbol name.
 * @param[in] node AST node at cursor.
 * @param[in] nodeText Symbol name under cursor.
 * @param[in] request Definition request context.
 * @return Clean base type name, or empty string if not found.
 */
std::string ResolveGlobalSymbolTypeName(TSNode node, const std::string& nodeText, const DefinitionRequest& request)
{
    auto symbols = request.symbolTable.FindSymbols(nodeText);
    if (symbols.empty() && !ts_node_is_null(node))
    {
        symbols = analysis::FindSymbolsInScope(nodeText, node, request.sourceCode, request.symbolTable);
    }
    for (const auto& sym : symbols)
    {
        if (sym.type == analysis::SymbolType::Variable)
        {
            const auto& var = sym.GetVariable();
            if (!var.typeName.empty())
            {
                return analysis::CleanBaseType(var.typeName);
            }
        }
        else if (sym.type == analysis::SymbolType::Function)
        {
            const auto& fn = sym.GetFunction();
            if (!fn.returnType.empty())
            {
                return analysis::CleanBaseType(fn.returnType);
            }
        }
        else if (sym.type == analysis::SymbolType::Class || sym.type == analysis::SymbolType::Interface ||
                 sym.type == analysis::SymbolType::Enum || sym.type == analysis::SymbolType::Typedef ||
                 sym.type == analysis::SymbolType::Funcdef)
        {
            return sym.name;
        }
    }
    return "";
}

/**
 * @brief Resolves type name from global property accessors matching symbol name.
 * @param[in] node AST node at cursor.
 * @param[in] nodeText Symbol name under cursor.
 * @param[in] request Definition request context.
 * @return Clean base type name, or empty string if not found.
 */
std::string ResolveAccessorTypeName(TSNode node, const std::string& nodeText, const DefinitionRequest& request)
{
    auto accessors = analysis::FindGlobalPropertyAccessors(nodeText, request.symbolTable);
    if (accessors.empty() && !ts_node_is_null(node))
    {
        for (const auto prefix : {"get_", "set_"})
        {
            for (const auto& sym :
                 analysis::FindSymbolsInScope(prefix + nodeText, node, request.sourceCode, request.symbolTable))
            {
                if (sym.type == analysis::SymbolType::Function)
                {
                    accessors.push_back(sym);
                }
            }
        }
    }
    const std::string propType = analysis::PropertyTypeFromAccessors(accessors);
    if (!propType.empty())
    {
        return analysis::CleanBaseType(propType);
    }
    return "";
}

/**
 * @brief Checks if a symbol type represents a user-defined type definition target.
 * @param[in] type Symbol type enum.
 * @return True if Class, Interface, Enum, Typedef, or Funcdef.
 */
inline bool IsTypeDefType(analysis::SymbolType type)
{
    return type == analysis::SymbolType::Class || type == analysis::SymbolType::Interface ||
           type == analysis::SymbolType::Enum || type == analysis::SymbolType::Typedef ||
           type == analysis::SymbolType::Funcdef;
}

/**
 * @brief Collects symbols defining the specified type name.
 * @param[in] node AST node at cursor.
 * @param[in] typeNameToFind Clean target type name.
 * @param[in] request Definition request context.
 * @return Vector of matching type definition symbols.
 */
std::vector<analysis::Symbol> CollectTypeDefinitionSymbols(TSNode node, const std::string& typeNameToFind,
                                                           const DefinitionRequest& request)
{
    std::vector<analysis::Symbol> typeSymbols = request.symbolTable.FindSymbols(typeNameToFind);
    if (typeSymbols.empty() && !ts_node_is_null(node))
    {
        for (const auto& sym :
             analysis::FindSymbolsInScope(typeNameToFind, node, request.sourceCode, request.symbolTable))
        {
            if (IsTypeDefType(sym.type))
            {
                typeSymbols.push_back(sym);
            }
        }
    }
    if (typeSymbols.empty())
    {
        typeSymbols = request.symbolTable.FindTypeSymbolsByShortName(typeNameToFind);
    }
    return typeSymbols;
}

} // namespace

std::optional<std::vector<lsp::Location>> GetDefinition(const DefinitionRequest& request)
{
    if (auto includeLoc = TryResolveIncludeDirective(request))
    {
        return includeLoc;
    }

    TSNode node{};
    std::string nodeText = GetNodeTextAt(request, node);
    if (nodeText.empty() || ts_node_is_null(node))
    {
        return std::nullopt;
    }

    DefinitionContext ctx = MakeDefinitionContext(request);
    if (auto contextualLocs = TryResolveContextualDefinition(node, nodeText, ctx))
    {
        return contextualLocs;
    }

    std::vector<analysis::Symbol> symbols = ResolveCandidateSymbols(node, nodeText, ctx);
    if (symbols.empty())
    {
        if (auto localFallback = TryResolveLocalDefinitionFallback(nodeText, ctx))
        {
            return localFallback;
        }
    }

    if (nodeText == "this")
    {
        if (auto thisLocs = TryResolveThisKeyword(node, ctx))
        {
            return thisLocs;
        }
    }

    auto locations = ConvertSymbolsToLocations(symbols, request);
    if (!locations.empty())
    {
        return locations;
    }

    return TryResolveDeclarationFallback(node, ctx);
}

std::optional<std::vector<lsp::Location>> GetTypeDefinition(const DefinitionRequest& request)
{
    TSNode node{};
    std::string nodeText = GetNodeTextAt(request, node);
    if (nodeText.empty() || ts_node_is_null(node))
    {
        return std::nullopt;
    }

    std::string typeNameToFind = ResolveLocalScopeTypeName(nodeText, request);
    if (typeNameToFind.empty())
    {
        typeNameToFind = ResolveGlobalSymbolTypeName(node, nodeText, request);
    }
    if (typeNameToFind.empty())
    {
        typeNameToFind = ResolveAccessorTypeName(node, nodeText, request);
    }
    if (typeNameToFind.empty())
    {
        typeNameToFind = analysis::CleanBaseType(nodeText);
    }

    if (typeNameToFind.empty() || analysis::IsPrimitiveTypeName(typeNameToFind))
    {
        return std::nullopt;
    }

    auto typeSymbols = CollectTypeDefinitionSymbols(node, typeNameToFind, request);
    std::vector<lsp::Location> locations;
    for (const auto& sym : typeSymbols)
    {
        if (IsTypeDefType(sym.type))
        {
            locations.push_back(MakeLocation(sym, request));
        }
    }

    if (!locations.empty())
    {
        return locations;
    }

    return std::nullopt;
}

} // namespace angel_lsp::features
