#include "features/inlay_hint/InlayHintHandler.h"
#include "analysis/OverloadResolver.h"
#include "analysis/SemanticHelpers.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarNames.h"
#include "utils/LspLogger.h"
#include <algorithm>
#include <cctype>
#include <spdlog/fmt/fmt.h>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace angel_lsp::features
{
namespace
{
/**
 * @brief Extracts text slice of an AST node from the source code.
 * @param[in] node Tree-sitter AST node.
 * @param[in] sourceCode Source text buffer.
 * @return Extracted node text or empty string.
 */
std::string GetNodeText(TSNode node, std::string_view sourceCode)
{
    return parser::GetNodeText(node, sourceCode);
}

/**
 * @brief Trims surrounding whitespace, parentheses, and semicolons from node text.
 * @param[in] text Raw text to trim.
 * @return Trimmed string.
 */
std::string TrimNodeText(std::string text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '('))
    {
        text.erase(text.begin());
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == ')' || text.back() == ';'))
    {
        text.pop_back();
    }
    return text;
}

/**
 * @brief Checks if a position is within the requested range (or if range is unbounded).
 * @param[in] pos Target position to test.
 * @param[in] range Active query range.
 * @return True if position is within range.
 */
bool IsPositionInRange(const lsp::Position& pos, const lsp::Range& range)
{
    if (range.start.line == 0 && range.start.character == 0 && range.end.line == 0 && range.end.character == 0)
    {
        return true;
    }
    if (pos.line < range.start.line || pos.line > range.end.line)
    {
        return false;
    }
    if (pos.line == range.start.line && pos.character < range.start.character)
    {
        return false;
    }
    if (pos.line == range.end.line && pos.character > range.end.character)
    {
        return false;
    }
    return true;
}

/**
 * @brief Checks if an AST node's line range overlaps with the requested range.
 * @param[in] node AST node to check.
 * @param[in] range Active query range.
 * @return True if node overlaps with range.
 */
bool IsNodeOverlappingRange(TSNode node, const lsp::Range& range)
{
    if (range.start.line == 0 && range.start.character == 0 && range.end.line == 0 && range.end.character == 0)
    {
        return true;
    }
    TSPoint startPoint = ts_node_start_point(node);
    TSPoint endPoint = ts_node_end_point(node);
    if (endPoint.row < range.start.line || startPoint.row > range.end.line)
    {
        return false;
    }
    return true;
}

/**
 * @brief Retrieves parameter information vector pointer from a Function or Funcdef symbol if available.
 * @param[in] sym Symbol to inspect.
 * @return Pointer to vector of parameter information, or nullptr if not callable.
 */
const std::vector<analysis::ParameterInformation>* GetParametersIfCallable(const analysis::Symbol& sym)
{
    if (sym.type == analysis::SymbolType::Function &&
        std::holds_alternative<analysis::FunctionSignature>(sym.signature))
    {
        return &sym.GetFunction().parameters;
    }
    if (sym.type == analysis::SymbolType::Funcdef && std::holds_alternative<analysis::FuncdefSignature>(sym.signature))
    {
        return &sym.GetFuncdef().parameters;
    }
    return nullptr;
}

/**
 * @brief Checks if a parameter represents a variadic (varargs) argument.
 * @param[in] param Parameter information.
 * @return True if parameter is varargs (...).
 */
bool IsVarargParameter(const analysis::ParameterInformation& param)
{
    return param.name == "..." || param.typeName == "..." || param.rawText.find("...") != std::string::npos;
}

/**
 * @brief Counts the minimum number of arguments required by a callable's parameters.
 * @param[in] parameters Vector of parameter information to evaluate.
 * @return Count of non-default, non-vararg parameters.
 */
size_t CountRequiredParameters(const std::vector<analysis::ParameterInformation>& parameters)
{
    size_t minRequired = 0;
    for (const auto& p : parameters)
    {
        if (p.defaultValue.empty() && !IsVarargParameter(p))
        {
            minRequired++;
        }
    }
    return minRequired;
}

/**
 * @brief Locates the function expression child node for a call_expression.
 * @param[in] callNode AST call_expression node.
 * @return Function node, or null node if not found.
 */
TSNode FindCalleeFunctionNode(TSNode callNode)
{
    TSNode funcNode = parser::GetChildByField(callNode, parser::fields::Function);
    if (ts_node_is_null(funcNode) && ts_node_child_count(callNode) > 0)
    {
        funcNode = ts_node_child(callNode, 0);
    }
    return funcNode;
}

/**
 * @brief Collects candidate method symbols for a member call expression across type hierarchy.
 * @param[in] funcNode AST member_expression node.
 * @param[in] request Inlay hint request context.
 * @return Vector of candidate symbols.
 */
std::vector<analysis::Symbol> CollectMemberCalleeCandidates(TSNode funcNode, const InlayHintRequest& request)
{
    std::vector<analysis::Symbol> candidateSymbols;
    TSNode objNode = parser::GetChildByField(funcNode, parser::fields::Object);
    TSNode memNode = parser::GetChildByField(funcNode, parser::fields::Member);
    if (ts_node_is_null(objNode) || ts_node_is_null(memNode))
    {
        return candidateSymbols;
    }

    std::string objText = GetNodeText(objNode, request.sourceCode);
    std::string memText = GetNodeText(memNode, request.sourceCode);
    auto rootScope = request.scopeIndex.GetRoot(request.uri);
    TSPoint objPoint = ts_node_start_point(objNode);
    const analysis::Scope* scope =
        rootScope ? FindInnermostScope(rootScope.get(), objPoint.row, objPoint.column) : nullptr;
    std::string receiverTypeName =
        analysis::ResolveReceiverType(objNode, request.sourceCode, request.symbolTable, {scope, "", request.uri});

    if (receiverTypeName.empty())
    {
        auto directCandidates = request.symbolTable.FindSymbols(objText + "::" + memText);
        for (const auto& sym : directCandidates)
        {
            candidateSymbols.push_back(sym);
        }
        return candidateSymbols;
    }

    auto hierarchy = analysis::GetInheritedTypeHierarchy(receiverTypeName, request.symbolTable);
    for (const auto& typeName : hierarchy)
    {
        std::string qualifiedName = typeName + "::" + memText;
        auto found = request.symbolTable.FindSymbols(qualifiedName);
        for (const auto& sym : found)
        {
            if (sym.type == analysis::SymbolType::Function)
            {
                bool overriddenLower =
                    std::any_of(candidateSymbols.begin(), candidateSymbols.end(), [&](const analysis::Symbol& kept)
                                { return analysis::HasSameParameterList(kept, sym); });
                if (!overriddenLower)
                {
                    candidateSymbols.push_back(sym);
                }
            }
            else
            {
                candidateSymbols.push_back(sym);
            }
        }
    }
    return candidateSymbols;
}

/**
 * @brief Collects candidate member methods from enclosing class hierarchy for an unqualified call.
 * @param[in] calleeName Callee identifier text.
 * @param[in] callNode AST call_expression node.
 * @param[in] request Inlay hint request context.
 * @param[in,out] candidateSymbols Symbol collection receiving candidates.
 */
void CollectEnclosingClassCallees(const std::string& calleeName, TSNode callNode, const InlayHintRequest& request,
                                  std::vector<analysis::Symbol>& candidateSymbols)
{
    auto containers = analysis::GetEnclosingContainers(callNode, request.sourceCode);
    for (const auto& c : containers)
    {
        if (c.kind == analysis::ContainerKind::Class || c.kind == analysis::ContainerKind::Interface)
        {
            auto hierarchy = analysis::GetInheritedTypeHierarchy(c.qualifiedName.empty() ? c.name : c.qualifiedName,
                                                                 request.symbolTable);
            for (const auto& typeName : hierarchy)
            {
                std::string qualifiedName = typeName + "::" + calleeName;
                auto found = request.symbolTable.FindSymbols(qualifiedName);
                for (const auto& sym : found)
                {
                    if (sym.type == analysis::SymbolType::Function)
                    {
                        bool overriddenLower = std::any_of(candidateSymbols.begin(), candidateSymbols.end(),
                                                           [&](const analysis::Symbol& kept)
                                                           { return analysis::HasSameParameterList(kept, sym); });
                        if (!overriddenLower)
                        {
                            candidateSymbols.push_back(sym);
                        }
                    }
                }
            }
            break;
        }
    }
}

/**
 * @brief Collects candidate function symbols for a free or unqualified call expression.
 * @param[in] funcNode AST callee function node.
 * @param[in] callNode AST call_expression node.
 * @param[in] request Inlay hint request context.
 * @return Vector of candidate symbols.
 */
/**
 * @brief Resolves scoped callee candidates by walking enclosing namespaces and inherited hierarchies.
 * @param[in] calleeName Callee identifier string.
 * @param[in] callNode AST call_expression node.
 * @param[in] request Inlay hint request context.
 * @return Vector of resolved candidate symbols.
 */
std::vector<analysis::Symbol> CollectScopedCalleeCandidates(const std::string& calleeName, TSNode callNode,
                                                            const InlayHintRequest& request)
{
    std::vector<analysis::Symbol> candidates;
    if (calleeName.find("::") == std::string::npos)
    {
        return candidates;
    }
    size_t lastColon = calleeName.rfind("::");
    std::string targetContainer = calleeName.substr(0, lastColon);
    std::string methodName = calleeName.substr(lastColon + 2);

    auto hierarchy = analysis::GetInheritedTypeHierarchy(targetContainer, request.symbolTable);
    for (const auto& typeName : hierarchy)
    {
        auto syms = request.symbolTable.FindSymbols(typeName + "::" + methodName);
        if (!syms.empty())
        {
            candidates.insert(candidates.end(), syms.begin(), syms.end());
            return candidates;
        }
    }

    auto containers = analysis::GetEnclosingContainers(callNode, request.sourceCode);
    for (auto it = containers.rbegin(); it != containers.rend(); ++it)
    {
        if (it->kind == analysis::ContainerKind::Namespace)
        {
            std::string nsPrefix = it->qualifiedName.empty() ? it->name : it->qualifiedName;
            auto scopedSyms = request.symbolTable.FindSymbols(nsPrefix + "::" + calleeName);
            if (!scopedSyms.empty())
            {
                return scopedSyms;
            }
            auto scopedHier =
                analysis::GetInheritedTypeHierarchy(nsPrefix + "::" + targetContainer, request.symbolTable);
            for (const auto& typeName : scopedHier)
            {
                auto syms = request.symbolTable.FindSymbols(typeName + "::" + methodName);
                if (!syms.empty())
                {
                    candidates.insert(candidates.end(), syms.begin(), syms.end());
                    return candidates;
                }
            }
        }
    }
    return candidates;
}

std::vector<analysis::Symbol> CollectFreeCalleeCandidates(TSNode funcNode, TSNode callNode,
                                                          const InlayHintRequest& request)
{
    std::string calleeName = GetNodeText(funcNode, request.sourceCode);
    if (calleeName.starts_with("::"))
    {
        calleeName = calleeName.substr(2);
    }
    auto candidateSymbols = analysis::FindSymbolsInScope(calleeName, callNode, request.sourceCode, request.symbolTable);

    CollectEnclosingClassCallees(calleeName, callNode, request, candidateSymbols);

    if (candidateSymbols.empty())
    {
        candidateSymbols = request.symbolTable.FindSymbols(calleeName);
    }

    if (candidateSymbols.empty())
    {
        candidateSymbols = CollectScopedCalleeCandidates(calleeName, callNode, request);
    }

    for (const auto& sym : candidateSymbols)
    {
        if (sym.type == analysis::SymbolType::Class)
        {
            std::string ctorName = sym.name + "::" + sym.name;
            auto ctorSyms = request.symbolTable.FindSymbols(ctorName);
            if (!ctorSyms.empty())
            {
                return ctorSyms;
            }
        }
    }
    return candidateSymbols;
}

/**
 * @brief Selects the candidate matching the given argument count and default parameter bounds.
 * @param[in] candidateSymbols Available candidate symbols.
 * @param[in] numArgs Number of arguments passed.
 * @return Pointer to best matching candidate symbol, or nullptr.
 */
const analysis::Symbol* SelectCandidateByArgCount(const std::vector<analysis::Symbol>& candidateSymbols, size_t numArgs)
{
    const analysis::Symbol* bestSym = nullptr;
    for (const auto& sym : candidateSymbols)
    {
        const auto* params = GetParametersIfCallable(sym);
        if (!params)
        {
            continue;
        }
        size_t minRequiredArgs = CountRequiredParameters(*params);
        if (numArgs >= minRequiredArgs && numArgs <= params->size())
        {
            if (!bestSym || params->size() == numArgs ||
                (GetParametersIfCallable(*bestSym) && GetParametersIfCallable(*bestSym)->size() < numArgs))
            {
                bestSym = &sym;
                if (params->size() == numArgs)
                {
                    break;
                }
            }
        }
    }
    return bestSym;
}

/**
 * @brief Selects the candidate with the highest parameter count as a fallback.
 * @param[in] candidateSymbols Available candidate symbols.
 * @return Pointer to fallback symbol, or nullptr.
 */
const analysis::Symbol* SelectFallbackCandidate(const std::vector<analysis::Symbol>& candidateSymbols)
{
    const analysis::Symbol* bestSym = nullptr;
    size_t maxParams = 0;
    for (const auto& sym : candidateSymbols)
    {
        const auto* params = GetParametersIfCallable(sym);
        size_t pCount = params ? params->size() : 0;
        if (!bestSym || pCount > maxParams)
        {
            bestSym = &sym;
            maxParams = pCount;
        }
    }
    return bestSym;
}

/**
 * @brief Bundles callee document URI, callee name, and extracted parameter information.
 */
struct CalleeResolutionResult
{
    std::string fileUri;
    std::string calleeName;
    std::vector<analysis::ParameterInformation> parameters;
};

/**
 * @brief Extracts callee result with URI, name, and parameters from a resolved callable symbol.
 * @param[in] sym Resolved symbol.
 * @return Callee resolution result.
 */
CalleeResolutionResult ExtractCalleeFromSymbol(const analysis::Symbol* sym)
{
    if (!sym)
    {
        return {};
    }
    const auto* params = GetParametersIfCallable(*sym);
    return {
        sym->fileUri,
        sym->name,
        params ? *params : std::vector<analysis::ParameterInformation>{}
    };
}

/**
 * @brief Resolves parameters for a function, method, or constructor called by a call_expression node.
 * @param[in] callNode AST call_expression node.
 * @param[in] request Inlay hint request context.
 * @param[in] numArgs Number of arguments passed in the call.
 * @return Callee resolution result matching the call.
 */
CalleeResolutionResult ResolveCalleeParameters(TSNode callNode, const InlayHintRequest& request,
                                              size_t numArgs)
{
    TSNode funcNode = FindCalleeFunctionNode(callNode);
    if (ts_node_is_null(funcNode))
    {
        return {};
    }

    std::string_view funcType = ts_node_type(funcNode);
    std::vector<analysis::Symbol> candidateSymbols = (funcType == "member_expression")
                                                         ? CollectMemberCalleeCandidates(funcNode, request)
                                                         : CollectFreeCalleeCandidates(funcNode, callNode, request);

    const analysis::Symbol* bestSym = nullptr;
    if (candidateSymbols.size() > 1)
    {
        auto rootScope = request.scopeIndex.GetRoot(request.uri);
        const analysis::Scope* scope = nullptr;
        if (rootScope)
        {
            TSPoint pt = ts_node_start_point(callNode);
            scope = FindInnermostScope(rootScope.get(), pt.row, pt.column);
        }
        auto argTypes =
            analysis::ExtractCallArgumentTypes(callNode, {scope, request.symbolTable, request.sourceCode, request.uri});
        auto match = analysis::ResolveBestOverload(candidateSymbols, argTypes, request.symbolTable);
        if (match.bestCandidate != nullptr)
        {
            bestSym = match.bestCandidate;
        }
    }

    if (!bestSym)
    {
        bestSym = SelectCandidateByArgCount(candidateSymbols, numArgs);
    }
    if (!bestSym && !candidateSymbols.empty())
    {
        bestSym = SelectFallbackCandidate(candidateSymbols);
    }

    return ExtractCalleeFromSymbol(bestSym);
}

/**
 * @brief Holds parsed information for an argument AST node in a call or constructor.
 */
struct ArgInfo
{
    TSNode exprNode;
    bool isNamed = false;
    std::string argName;
    lsp::Position hintPosition;
    std::string text;
};

/**
 * @brief Parses an argument_list node into structured ArgInfo items.
 * @param[in] argListNode AST argument_list node.
 * @param[in] sourceCode Source text buffer.
 * @return Vector of parsed ArgInfo items.
 */
std::vector<ArgInfo> ParseArguments(TSNode argListNode, std::string_view sourceCode)
{
    std::vector<ArgInfo> args;
    auto callArgs = analysis::ExtractCallArguments(argListNode, sourceCode);
    args.reserve(callArgs.size());
    for (const auto& a : callArgs)
    {
        ArgInfo arg;
        arg.exprNode = a.exprNode;
        arg.isNamed = !a.name.empty();
        arg.argName = a.name;
        TSPoint startPoint = ts_node_start_point(a.exprNode);
        arg.hintPosition = lsp::Position{startPoint.row, startPoint.column};
        arg.text = GetNodeText(a.exprNode, sourceCode);
        args.push_back(std::move(arg));
    }
    return args;
}

/**
 * @brief Collects candidate constructor symbols for direct-initialization.
 * @param[in] baseName Clean base type name.
 * @param[in] declaratorNode AST variable_declarator node.
 * @param[in] request Inlay hint request context.
 * @return Vector of candidate constructor symbols.
 */
std::vector<analysis::Symbol> CollectConstructorCandidates(const std::string& baseName, TSNode declaratorNode,
                                                           const InlayHintRequest& request)
{
    return analysis::CollectConstructorCandidates(baseName, declaratorNode, request.sourceCode, request.symbolTable);
}

/**
 * @brief Matches the best constructor overload using argument types.
 * @param[in] candidateSymbols Candidate constructor symbols.
 * @param[in] declaratorNode AST variable_declarator node.
 * @param[in] request Inlay hint request context.
 * @param[in] args Parsed argument information.
 * @return Pointer to best matching symbol, or nullptr.
 */
const analysis::Symbol* MatchConstructorOverload(const std::vector<analysis::Symbol>& candidateSymbols,
                                                 TSNode declaratorNode, const InlayHintRequest& request,
                                                 const std::vector<ArgInfo>& args)
{
    if (candidateSymbols.size() <= 1)
    {
        return nullptr;
    }

    auto rootScope = request.scopeIndex.GetRoot(request.uri);
    const analysis::Scope* scope = nullptr;
    if (rootScope)
    {
        TSPoint pt = ts_node_start_point(declaratorNode);
        scope = FindInnermostScope(rootScope.get(), pt.row, pt.column);
    }

    std::vector<std::string> argTypes;
    argTypes.reserve(args.size());
    for (const auto& arg : args)
    {
        std::string aType = analysis::ResolveExpressionType(
            arg.exprNode, {scope, request.symbolTable, request.sourceCode, request.uri});
        argTypes.push_back(std::move(aType));
    }

    auto match = analysis::ResolveBestOverload(candidateSymbols, argTypes, request.symbolTable);
    return match.bestCandidate;
}

/**
 * @brief Resolves constructor parameters for a variable direct-initialization.
 * @param[in] declaredTypeName The type name written in the variable declaration.
 * @param[in] declaratorNode The variable_declarator AST node.
 * @param[in] request Inlay hint context request.
 * @param[in] args Parsed argument information.
 * @return Callee resolution result matching the constructor call.
 */
CalleeResolutionResult ResolveConstructorParameters(const std::string& declaredTypeName,
                                                     TSNode declaratorNode,
                                                     const InlayHintRequest& request,
                                                     const std::vector<ArgInfo>& args)
{
    std::string baseName = analysis::CleanBaseType(declaredTypeName);
    if (baseName.empty())
    {
        return {};
    }

    std::vector<analysis::Symbol> candidateSymbols = CollectConstructorCandidates(baseName, declaratorNode, request);
    if (candidateSymbols.empty())
    {
        return {};
    }

    const analysis::Symbol* bestSym = MatchConstructorOverload(candidateSymbols, declaratorNode, request, args);

    if (!bestSym)
    {
        bestSym = SelectCandidateByArgCount(candidateSymbols, args.size());
    }

    if (!bestSym)
    {
        for (const auto& sym : candidateSymbols)
        {
            if (sym.type == analysis::SymbolType::Function)
            {
                bestSym = &sym;
                break;
            }
        }
    }

    if (bestSym && bestSym->type == analysis::SymbolType::Function)
    {
        return ExtractCalleeFromSymbol(bestSym);
    }

    return {};
}

/**
 * @brief Forward declaration for expression type deduction.
 * @param[in] exprNode Target expression node.
 * @param[in] request Inlay hint request context.
 * @return Deduced type string or empty string.
 */
std::string DeduceExpressionType(TSNode exprNode, const InlayHintRequest& request);

/**
 * @brief Checks if trimmed text represents a numeric literal token.
 * @param[in] text Trimmed text string.
 * @param[in] nodeType AST node type name.
 * @return True if token is numeric.
 */
bool IsNumericLiteralText(std::string_view text, std::string_view nodeType)
{
    if (text.empty())
    {
        return false;
    }
    if (nodeType == "number_literal")
    {
        return true;
    }
    if (isdigit(static_cast<unsigned char>(text[0])))
    {
        return true;
    }
    if (text.size() > 1 && (text[0] == '-' || text[0] == '+') && isdigit(static_cast<unsigned char>(text[1])))
    {
        return true;
    }
    return text.starts_with("0x") || text.starts_with("0X") || text.starts_with("0b") || text.starts_with("0B") ||
           text.starts_with("0o") || text.starts_with("0O");
}

/**
 * @brief Deduces AngelScript type string from a numeric literal text.
 * @param[in] nodeTxt Trimmed literal string.
 * @return Deduced type ("float", "double", or "int").
 */
std::string DeduceNumericLiteralType(std::string_view nodeTxt)
{
    if (nodeTxt.find('.') != std::string_view::npos || nodeTxt.find('e') != std::string_view::npos ||
        nodeTxt.find('E') != std::string_view::npos)
    {
        if (nodeTxt.ends_with('f') || nodeTxt.ends_with('F'))
        {
            return "float";
        }
        return "double";
    }
    if (nodeTxt.ends_with('f') || nodeTxt.ends_with('F'))
    {
        return "float";
    }
    if (nodeTxt.ends_with('d') || nodeTxt.ends_with('D'))
    {
        return "double";
    }
    return "int";
}

/**
 * @brief Deduces literal expression type from syntax node and source code.
 * @param[in] exprNode Literal AST node.
 * @param[in] sourceCode Source text buffer.
 * @return Type string or empty string.
 */
std::string DeduceLiteralType(TSNode exprNode, std::string_view sourceCode, std::string_view stringTypeName = "string")
{
    std::string_view type = ts_node_type(exprNode);
    std::string nodeTxt = TrimNodeText(GetNodeText(exprNode, sourceCode));
    if (IsNumericLiteralText(nodeTxt, type))
    {
        return DeduceNumericLiteralType(nodeTxt);
    }
    if (type == "string_literal" || type == "concatenated_string")
    {
        return stringTypeName.empty() ? "string" : std::string(stringTypeName);
    }
    if (type == "boolean_literal")
    {
        return "bool";
    }
    return "";
}

/**
 * @brief Deduces type from functional cast, C-style cast, or constructor call expressions.
 * @param[in] exprNode AST cast or constructor call node.
 * @param[in] sourceCode Source text buffer.
 * @return Type string or empty string.
 */
std::string DeduceCastOrConstructType(TSNode exprNode, std::string_view sourceCode)
{
    std::string_view type = ts_node_type(exprNode);
    if (type == "functional_cast_expression" || type == "cast_expression")
    {
        TSNode typeNode = parser::GetChildByField(exprNode, parser::fields::Type);
        if (!ts_node_is_null(typeNode))
        {
            return GetNodeText(typeNode, sourceCode);
        }
    }
    else if (type == "construct_call_expression")
    {
        TSNode typeNode = parser::GetChildByField(exprNode, parser::fields::Type);
        if (!ts_node_is_null(typeNode))
        {
            std::string cType = GetNodeText(typeNode, sourceCode);
            uint32_t count = ts_node_child_count(exprNode);
            for (uint32_t i = 0; i < count; ++i)
            {
                TSNode child = ts_node_child(exprNode, i);
                if (std::string_view(ts_node_type(child)) == "template_type_list")
                {
                    cType += GetNodeText(child, sourceCode);
                    break;
                }
            }
            return cType;
        }
    }
    return "";
}

/**
 * @brief Resolves receiver class type name for a method call expression.
 * @param[in] objNode Object expression AST node.
 * @param[in] exprNode Full call expression AST node.
 * @param[in] request Inlay hint request context.
 * @param[in] rootScope Root scope of document, or nullptr.
 * @return Receiver type name or empty string.
 */
std::string ResolveMemberCallReceiverType(TSNode objNode, TSNode exprNode, const InlayHintRequest& request,
                                          const analysis::Scope* rootScope)
{
    std::string objText = GetNodeText(objNode, request.sourceCode);
    if (objText == "this")
    {
        auto containers = analysis::GetEnclosingContainers(exprNode, request.sourceCode);
        for (const auto& c : containers)
        {
            if (c.kind == analysis::ContainerKind::Class)
            {
                return c.name;
            }
        }
    }
    else if (rootScope)
    {
        TSPoint objPoint = ts_node_start_point(objNode);
        const analysis::Scope* objScope = FindInnermostScope(rootScope, objPoint.row, objPoint.column);
        if (objScope)
        {
            const analysis::LocalDefinition* def = analysis::ResolveInScope(objScope, objText);
            if (def && !def->typeName.empty())
            {
                return analysis::CleanBaseType(def->typeName);
            }
        }
    }
    return objText;
}

/**
 * @brief Deduces return type for a member method call expression.
 * @param[in] funcNode AST member_expression node.
 * @param[in] exprNode Call expression AST node.
 * @param[in] request Inlay hint request context.
 * @param[in] rootScope Root scope of document, or nullptr.
 * @return Return type string or empty string.
 */
std::string DeduceMemberCallReturnType(TSNode funcNode, TSNode exprNode, const InlayHintRequest& request,
                                       const analysis::Scope* rootScope)
{
    TSNode objNode = parser::GetChildByField(funcNode, parser::fields::Object);
    TSNode memNode = parser::GetChildByField(funcNode, parser::fields::Member);
    if (ts_node_is_null(objNode) || ts_node_is_null(memNode))
    {
        return "";
    }
    std::string receiverType = ResolveMemberCallReceiverType(objNode, exprNode, request, rootScope);
    if (receiverType.empty())
    {
        return "";
    }
    std::string memText = GetNodeText(memNode, request.sourceCode);
    auto hierarchy = analysis::GetInheritedTypeHierarchy(receiverType, request.symbolTable);
    if (hierarchy.empty())
    {
        hierarchy.push_back(receiverType);
    }
    for (const auto& typeName : hierarchy)
    {
        std::string qualifiedName = typeName + "::" + memText;
        auto found = request.symbolTable.FindSymbols(qualifiedName);
        for (const auto& sym : found)
        {
            if (sym.type == analysis::SymbolType::Function)
            {
                return sym.GetFunction().returnType;
            }
        }
    }
    return "";
}

/**
 * @brief Deduces return type for a free or unqualified function call expression.
 * @param[in] funcNode Callee function identifier AST node.
 * @param[in] exprNode Call expression AST node.
 * @param[in] request Inlay hint request context.
 * @return Return type string or class name, or empty string.
 */
std::string DeduceFreeCallReturnType(TSNode funcNode, TSNode exprNode, const InlayHintRequest& request)
{
    std::string fName = GetNodeText(funcNode, request.sourceCode);
    auto candidates = analysis::FindSymbolsInScope(fName, exprNode, request.sourceCode, request.symbolTable);
    if (candidates.empty())
    {
        candidates = request.symbolTable.FindSymbols(fName);
    }
    for (const auto& sym : candidates)
    {
        if (sym.type == analysis::SymbolType::Function)
        {
            return sym.GetFunction().returnType;
        }
        if (sym.type == analysis::SymbolType::Funcdef)
        {
            return sym.GetFuncdef().returnType;
        }
        if (sym.type == analysis::SymbolType::Class)
        {
            return sym.name;
        }
    }
    return "";
}

/**
 * @brief Deduces the return type of a call expression.
 * @param[in] exprNode Call expression AST node.
 * @param[in] request Inlay hint request context.
 * @param[in] rootScope Root document scope.
 * @return Deduced return type string or empty string.
 */
std::string DeduceCallExpressionType(TSNode exprNode, const InlayHintRequest& request, const analysis::Scope* rootScope)
{
    TSNode funcNode = parser::GetChildByField(exprNode, parser::fields::Function);
    if (ts_node_is_null(funcNode))
    {
        uint32_t childCount = ts_node_child_count(exprNode);
        if (childCount > 0)
        {
            funcNode = ts_node_child(exprNode, 0);
        }
    }
    if (ts_node_is_null(funcNode))
    {
        return "";
    }
    if (std::string_view(ts_node_type(funcNode)) == "member_expression")
    {
        return DeduceMemberCallReturnType(funcNode, exprNode, request, rootScope);
    }
    return DeduceFreeCallReturnType(funcNode, exprNode, request);
}

/**
 * @brief Deduces the type of a member expression (property or method return type).
 * @param[in] exprNode Member expression AST node.
 * @param[in] request Inlay hint request context.
 * @param[in] rootScope Root document scope.
 * @return Deduced type string or empty string.
 */
std::string DeduceMemberExpressionType(TSNode exprNode, const InlayHintRequest& request,
                                       const analysis::Scope* rootScope)
{
    TSNode objNode = parser::GetChildByField(exprNode, parser::fields::Object);
    TSNode memNode = parser::GetChildByField(exprNode, parser::fields::Member);
    if (ts_node_is_null(objNode) || ts_node_is_null(memNode))
    {
        return "";
    }
    std::string objText = GetNodeText(objNode, request.sourceCode);
    std::string memText = GetNodeText(memNode, request.sourceCode);
    std::string receiverType;
    if (rootScope)
    {
        TSPoint objPoint = ts_node_start_point(objNode);
        const analysis::Scope* objScope = FindInnermostScope(rootScope, objPoint.row, objPoint.column);
        if (objScope)
        {
            const analysis::LocalDefinition* def = analysis::ResolveInScope(objScope, objText);
            if (def && !def->typeName.empty())
            {
                receiverType = analysis::CleanBaseType(def->typeName);
            }
        }
    }
    if (receiverType.empty())
    {
        return "";
    }
    auto hierarchy = analysis::GetInheritedTypeHierarchy(receiverType, request.symbolTable);
    for (const auto& typeName : hierarchy)
    {
        std::string qualifiedName = typeName + "::" + memText;
        auto found = request.symbolTable.FindSymbols(qualifiedName);
        for (const auto& sym : found)
        {
            if (sym.type == analysis::SymbolType::Variable)
            {
                return sym.GetVariable().typeName;
            }
            if (sym.type == analysis::SymbolType::Function)
            {
                return sym.GetFunction().returnType;
            }
        }
    }
    return "";
}

/**
 * @brief Deduces the type of an identifier or scoped identifier from scope or symbol table.
 * @param[in] exprNode Identifier AST node.
 * @param[in] request Inlay hint request context.
 * @param[in] scope Enclosing lexical scope, or nullptr.
 * @return Type string or empty string.
 */
std::string DeduceIdentifierType(TSNode exprNode, const InlayHintRequest& request, const analysis::Scope* scope)
{
    std::string name = GetNodeText(exprNode, request.sourceCode);
    if (scope)
    {
        const analysis::LocalDefinition* def = analysis::ResolveInScope(scope, name);
        if (def && !def->typeName.empty())
        {
            return def->typeName;
        }
    }
    auto symbols = request.symbolTable.FindSymbols(name);
    for (const auto& sym : symbols)
    {
        if (sym.type == analysis::SymbolType::Variable)
        {
            return sym.GetVariable().typeName;
        }
    }
    return "";
}

/**
 * @brief Checks if an operator produces a boolean result.
 * @param[in] op Operator string.
 * @return True if operator is comparison or logical.
 */
bool IsBooleanBinaryOperator(std::string_view op)
{
    static const std::unordered_set<std::string_view> kBoolOps = {"==", "!=",  "<",  ">",   "<=", ">=", "&&",
                                                                  "||", "and", "or", "xor", "^^", "is", "!is"};
    return kBoolOps.contains(op);
}

/**
 * @brief Selects the wider numeric/string type from binary operand types.
 * @param[in] leftT Left operand type string.
 * @param[in] rightT Right operand type string.
 * @return Wider type string.
 */
static bool IsOperandMatch(std::string_view leftT, std::string_view rightT, std::string_view target) noexcept
{
    return leftT == target || rightT == target;
}

std::string SelectWiderType(std::string_view leftT, std::string_view rightT, std::string_view stringTypeName)
{
    static constexpr std::array<std::string_view, 2> kFloatingTypes = {"double", "float"};
    for (const auto type : kFloatingTypes)
    {
        if (IsOperandMatch(leftT, rightT, type))
        {
            return std::string(type);
        }
    }

    if (!stringTypeName.empty() && IsOperandMatch(leftT, rightT, stringTypeName))
    {
        return std::string(stringTypeName);
    }

    static constexpr std::array<std::string_view, 2> kWideIntTypes = {"int64", "uint"};
    for (const auto type : kWideIntTypes)
    {
        if (IsOperandMatch(leftT, rightT, type))
        {
            return std::string(type);
        }
    }

    if (!leftT.empty())
    {
        return std::string(leftT);
    }
    return !rightT.empty() ? std::string(rightT) : "int";
}

/**
 * @brief Deduces result type of a binary expression AST node.
 * @param[in] exprNode Binary expression AST node.
 * @param[in] request Inlay hint request context.
 * @return Deduced type string.
 */
std::string DeduceBinaryExpressionType(TSNode exprNode, const InlayHintRequest& request)
{
    TSNode opNode = parser::GetChildByField(exprNode, parser::fields::Operator);
    std::string op = GetNodeText(opNode, request.sourceCode);
    if (IsBooleanBinaryOperator(op))
    {
        return "bool";
    }

    TSNode left = parser::GetChildByField(exprNode, parser::fields::Left);
    TSNode right = parser::GetChildByField(exprNode, parser::fields::Right);
    std::string leftT = DeduceExpressionType(left, request);
    std::string rightT = DeduceExpressionType(right, request);

    const std::string_view strType =
        request.config ? std::string_view(request.config->types.stringTypeName) : "string";
    return SelectWiderType(leftT, rightT, strType);
}

/**
 * @brief Deduces result type of a unary expression AST node.
 * @param[in] exprNode Unary expression AST node.
 * @param[in] request Inlay hint request context.
 * @return Deduced type string.
 */
std::string DeduceUnaryExpressionType(TSNode exprNode, const InlayHintRequest& request)
{
    TSNode opNode = parser::GetChildByField(exprNode, parser::fields::Operator);
    std::string op = GetNodeText(opNode, request.sourceCode);
    TSNode operand = parser::GetChildByField(exprNode, parser::fields::Operand);

    if (op == "!" || op == "not")
    {
        return "bool";
    }
    if (op == "@")
    {
        std::string opT = DeduceExpressionType(operand, request);
        if (!opT.empty() && !opT.ends_with("@"))
        {
            return opT + "@";
        }
        return opT;
    }
    return DeduceExpressionType(operand, request);
}

/**
 * @brief Deduces type for a parenthesized expression AST node.
 * @param[in] exprNode Parenthesized expression node.
 * @param[in] request Inlay hint request context.
 * @return Deduced type string or empty string.
 */
std::string DeduceParenthesizedType(TSNode exprNode, const InlayHintRequest& request)
{
    uint32_t count = ts_node_child_count(exprNode);
    for (uint32_t i = 0; i < count; ++i)
    {
        TSNode child = ts_node_child(exprNode, i);
        std::string_view cType = ts_node_type(child);
        if (cType != "(" && cType != ")")
        {
            return DeduceExpressionType(child, request);
        }
    }
    return "";
}

/**
 * @brief Deduces the type string for an expression AST node.
 * @param[in] exprNode AST expression node.
 * @param[in] request Inlay hint request context.
 * @return Deduced type string or empty string.
 */
/**
 * @brief Deduces compound expression type (call, member, unary, binary, postfix).
 * @param[in] type AST node type name.
 * @param[in] exprNode Target expression node.
 * @param[in] request Inlay hint request context.
 * @param[in] rootScope Root lexical scope.
 * @return Deduced type string or empty string.
 */
std::string DeduceCompoundExpressionType(std::string_view type, TSNode exprNode, const InlayHintRequest& request,
                                         const analysis::Scope* rootScope)
{
    if (type == "call_expression")
    {
        return DeduceCallExpressionType(exprNode, request, rootScope);
    }
    if (type == "member_expression")
    {
        return DeduceMemberExpressionType(exprNode, request, rootScope);
    }
    if (type == "binary_expression")
    {
        return DeduceBinaryExpressionType(exprNode, request);
    }
    if (type == "unary_expression")
    {
        return DeduceUnaryExpressionType(exprNode, request);
    }
    if (type == "postfix_expression")
    {
        TSNode operand = parser::GetChildByField(exprNode, parser::fields::Operand);
        return DeduceExpressionType(operand, request);
    }
    return "";
}

std::string DeduceExpressionType(TSNode exprNode, const InlayHintRequest& request)
{
    if (ts_node_is_null(exprNode))
    {
        return "";
    }

    auto rootScope = request.scopeIndex.GetRoot(request.uri);
    TSPoint point = ts_node_start_point(exprNode);
    const analysis::Scope* scope = rootScope ? FindInnermostScope(rootScope.get(), point.row, point.column) : nullptr;

    const std::string_view strType = (request.config && !request.config->types.stringTypeName.empty())
                                         ? std::string_view(request.config->types.stringTypeName)
                                         : std::string_view("string");
    const std::string_view arrType = (request.config && !request.config->types.arrayTypeName.empty())
                                         ? std::string_view(request.config->types.arrayTypeName)
                                         : std::string_view("array");

    analysis::ExpressionTypeContext exprCtx{scope, request.symbolTable, request.sourceCode, request.uri};
    exprCtx.stringTypeName = strType;
    exprCtx.arrayTypeName = arrType;
    std::string resolved = analysis::ResolveExpressionType(exprNode, exprCtx);
    if (!resolved.empty() && resolved != "auto")
    {
        return resolved;
    }

    std::string_view type = ts_node_type(exprNode);
    if (type == "parenthesized_expression")
    {
        return DeduceParenthesizedType(exprNode, request);
    }
    std::string lit = DeduceLiteralType(exprNode, request.sourceCode, strType);
    if (!lit.empty())
    {
        return lit;
    }
    std::string cast = DeduceCastOrConstructType(exprNode, request.sourceCode);
    if (!cast.empty())
    {
        return cast;
    }
    if (type == "identifier" || type == "scoped_identifier")
    {
        return DeduceIdentifierType(exprNode, request, scope);
    }
    return DeduceCompoundExpressionType(type, exprNode, request, rootScope.get());
}

/**
 * @brief Resolves parameter label name, falling back to declared type for nameless parameters.
 * @param[in] param Parameter information.
 * @return Resolved parameter label, or empty string if parameter cannot be labeled.
 */
std::string ResolveParameterLabel(const analysis::ParameterInformation& param)
{
    if (!param.name.empty() && param.name != "...")
    {
        return param.name;
    }
    if (!param.rawText.empty() && param.rawText.find("...") == std::string::npos)
    {
        std::string fallback = TrimNodeText(param.rawText.substr(0, param.rawText.find('=')));
        if (!fallback.empty() && fallback != "...")
        {
            return fallback;
        }
    }
    return (!param.typeName.empty() && param.typeName != "...") ? param.typeName : "";
}

/**
 * @brief Formats hover tooltip text for a parameter hint.
 * @param[in] param Parameter information.
 * @param[in] label Resolved parameter label.
 * @param[in] calleeName Name of the callee function or method.
 * @return Formatted tooltip string.
 */
std::string FormatParameterTooltip(const analysis::ParameterInformation& param, const std::string& label,
                                   std::string_view calleeName = "")
{
    std::string sig;
    if (!param.name.empty())
    {
        sig = (!param.typeName.empty() ? (param.typeName + " ") : "") + param.name;
    }
    else
    {
        sig = !param.typeName.empty() ? param.typeName : label;
    }
    std::string tooltip = "```angelscript\n" + sig + "\n```";
    if (!calleeName.empty())
    {
        tooltip += "\n*Parameter for `" + std::string(calleeName) + "`*";
    }
    return tooltip;
}

/**
 * @brief Context bundle for parameter inlay hint construction.
 */
struct ParamHintContext
{
    const CalleeResolutionResult& callee;
    const InlayHintRequest& request;
};


/**
 * @brief Resolves target location for an inlay hint parameter definition.
 * @param[in] fallbackUri Fallback document URI.
 * @param[in] calleeUri Declared URI of the callee.
 * @param[in] param Callee parameter information.
 * @return Resolved Location if parameter has valid range, std::nullopt otherwise.
 */
std::optional<lsp::Location> ResolveParameterLocation(const std::string& fallbackUri,
                                                     const std::string& calleeUri,
                                                     const analysis::ParameterInformation& param)
{
    const std::string& targetUri = calleeUri.empty() ? fallbackUri : calleeUri;
    if (targetUri.empty())
    {
        return std::nullopt;
    }
    const bool hasNameLoc = (param.nameStartLine != param.nameEndLine || param.nameStartCharacter != param.nameEndCharacter);
    const bool hasValidLoc = hasNameLoc || (param.startLine != param.endLine || param.startCharacter != param.endCharacter);
    if (!hasValidLoc)
    {
        return std::nullopt;
    }
    const uint32_t sL = hasNameLoc ? param.nameStartLine : param.startLine;
    const uint32_t sC = hasNameLoc ? param.nameStartCharacter : param.startCharacter;
    const uint32_t eL = hasNameLoc ? param.nameEndLine : param.endLine;
    const uint32_t eC = hasNameLoc ? param.nameEndCharacter : param.endCharacter;
    lsp::Range paramRange{lsp::Position{sL, sC}, lsp::Position{eL, eC}};
    return lsp::Location{
        targetUri.rfind("file://", 0) == 0 ? lsp::DocumentUri::parse(targetUri)
                                           : lsp::Uri::fileUriFromPath(targetUri),
        paramRange
    };
}

/**
 * @brief Builds a parameter inlay hint for a single argument.
 * @param[in] ctx Parameter hint context containing call and config details.
 * @param[in] param Callee parameter information.
 * @param[in] arg Argument metadata.
 * @param[in] label Formatted parameter label.
 * @return Populated InlayHint.
 */
lsp::InlayHint BuildParameterHint(const ParamHintContext& ctx,
                                 const analysis::ParameterInformation& param,
                                 const ArgInfo& arg,
                                 std::string label)
{
    lsp::InlayHint hint;
    hint.position = arg.hintPosition;
    if (ctx.request.maxLength > 0 && label.length() > ctx.request.maxLength)
    {
        label = label.substr(0, ctx.request.maxLength) + "...";
    }
    const bool enableTooltip = !ctx.request.config || ctx.request.config->features.inlayHintsEnableTooltip;
    const bool enableLocation = !ctx.request.config || ctx.request.config->features.inlayHintsEnableLocation;

    lsp::InlayHintLabelPart part;
    part.value = label + ":";
    if (enableTooltip)
    {
        part.tooltip = lsp::MarkupContent{
            lsp::MarkupKindEnum(lsp::MarkupKind::Markdown),
            FormatParameterTooltip(param, label, ctx.callee.calleeName)
        };
    }
    if (enableLocation)
    {
        part.location = ResolveParameterLocation(ctx.request.uri, ctx.callee.fileUri, param);
    }
    hint.label = std::vector<lsp::InlayHintLabelPart>{std::move(part)};
    hint.kind = lsp::InlayHintKindEnum(lsp::InlayHintKind::Parameter);
    hint.paddingRight = true;
    hint.paddingLeft = false;
    hint.tooltip = std::nullopt;
    return hint;
}

/**
 * @brief Adds parameter inlay hints for matched parameters and arguments.
 * @param[in] callee Callee resolution result.
 * @param[in] args Parsed call arguments.
 * @param[in] request Inlay hint request context.
 * @param[in,out] hints Hint vector receiving generated parameter hints.
 */
void AddParameterHints(const CalleeResolutionResult& callee, const std::vector<ArgInfo>& args,
                       const InlayHintRequest& request, std::vector<lsp::InlayHint>& hints)
{
    const auto& parameters = callee.parameters;
    const ParamHintContext ctx{callee, request};
    for (size_t i = 0; i < args.size() && i < parameters.size(); ++i)
    {
        if (request.maxParameters > 0 && i >= request.maxParameters)
        {
            break;
        }

        const auto& param = parameters[i];
        const auto& arg = args[i];

        if (arg.isNamed || IsVarargParameter(param))
        {
            continue;
        }

        std::string label = ResolveParameterLabel(param);
        if (label.empty() || (request.suppressWhenArgumentMatchesName && arg.text == label))
        {
            continue;
        }

        if (IsPositionInRange(arg.hintPosition, request.range))
        {
            hints.push_back(BuildParameterHint(ctx, param, arg, std::move(label)));
        }
    }
}

/**
 * @brief Context bundling immutable request and mutable output hints vector.
 */
struct OmittedHintContext
{
    const InlayHintRequest& request;
    std::vector<lsp::InlayHint>& hints;
};

/**
 * @brief Position and bound parameter information for a call argument.
 */
struct CallArgPosition
{
    TSNode startNode;
    TSNode endNode;
    int boundParamIndex = -1;
};

/**
 * @brief Bundles formatted label text and hover tooltip for an omitted parameter hint.
 */
struct OmittedParamHint
{
    std::string label;
    std::string tooltip;
    std::string fileUri;
    const analysis::ParameterInformation* param = nullptr;
};

/**
 * @brief Formats label text for an omitted default argument hint.
 * @param[in] param Parameter information.
 * @param[in] mode Inlay hint display mode.
 * @param[in] maxLength Maximum label length threshold (0 for unlimited).
 * @return Formatted label string.
 */
std::string FormatOmittedDefaultLabel(const analysis::ParameterInformation& param,
                                      config::OmittedDefaultArgumentsMode mode, size_t maxLength)
{
    std::string text = (mode == config::OmittedDefaultArgumentsMode::Declaration)
                           ? (param.typeName + " " + param.name + " = " + param.defaultValue)
                           : (param.name + ": " + param.defaultValue);
    if (maxLength > 0 && text.length() > maxLength)
    {
        text = text.substr(0, maxLength) + "...";
    }
    return text;
}

/**
 * @brief Binds extracted call arguments to parameter indices and collects their AST boundaries.
 * @param[in] callArgs Extracted call argument list.
 * @param[in] parameters Function parameter signatures.
 * @param[out] boundParamIndices Set of parameter indices that have arguments passed.
 * @return Vector of CallArgPosition elements.
 */
std::vector<CallArgPosition> MapArgumentsToParameters(const std::vector<analysis::CallArgumentInfo>& callArgs,
                                                      const std::vector<analysis::ParameterInformation>& parameters,
                                                      std::unordered_set<size_t>& boundParamIndices)
{
    std::vector<CallArgPosition> positions;
    positions.reserve(callArgs.size());
    for (size_t i = 0; i < callArgs.size(); ++i)
    {
        const auto& arg = callArgs[i];
        CallArgPosition pos;
        pos.startNode = !ts_node_is_null(arg.nameNode) ? arg.nameNode : arg.exprNode;
        pos.endNode = arg.exprNode;
        if (!arg.name.empty())
        {
            for (size_t p = 0; p < parameters.size(); ++p)
            {
                if (parameters[p].name == arg.name)
                {
                    pos.boundParamIndex = static_cast<int>(p);
                    boundParamIndices.insert(p);
                    break;
                }
            }
        }
        else if (i < parameters.size())
        {
            pos.boundParamIndex = static_cast<int>(i);
            boundParamIndices.insert(i);
        }
        positions.push_back(pos);
    }
    return positions;
}

/**
 * @brief Constructs an InlayHint item for an omitted default parameter.
 * @param[in] pos Target document position.
 * @param[in] label Formatted hint label.
 * @param[in] hintInfo Omitted parameter metadata including location.
 * @return InlayHint object.
 */
lsp::InlayHint MakeOmittedDefaultHint(const lsp::Position& pos, std::string label,
                                      const OmittedParamHint& hintInfo,
                                      const InlayHintRequest& request)
{
    lsp::InlayHint hint;
    hint.position = pos;
    hint.kind = lsp::InlayHintKindEnum(lsp::InlayHintKind::Parameter);
    hint.paddingLeft = false;
    hint.paddingRight = false;

    const bool enableTooltip = !request.config || request.config->features.inlayHintsEnableTooltip;
    const bool enableLocation = !request.config || request.config->features.inlayHintsEnableLocation;

    lsp::InlayHintLabelPart part;
    part.value = std::move(label);
    if (enableTooltip && !hintInfo.tooltip.empty())
    {
        part.tooltip = lsp::MarkupContent{
            lsp::MarkupKindEnum(lsp::MarkupKind::Markdown),
            hintInfo.tooltip
        };
    }
    const bool hasValidLocation = hintInfo.param && (hintInfo.param->startLine != hintInfo.param->endLine ||
                                                    hintInfo.param->startCharacter != hintInfo.param->endCharacter);
    if (enableLocation && !hintInfo.fileUri.empty() && hasValidLocation)
    {
        lsp::Range paramRange{
            lsp::Position{hintInfo.param->startLine, hintInfo.param->startCharacter},
            lsp::Position{hintInfo.param->endLine, hintInfo.param->endCharacter}
        };
        part.location = lsp::Location{
            hintInfo.fileUri.rfind("file://", 0) == 0 ? lsp::DocumentUri::parse(hintInfo.fileUri)
                                                      : lsp::Uri::fileUriFromPath(hintInfo.fileUri),
            paramRange
        };
    }
    hint.label = std::vector<lsp::InlayHintLabelPart>{std::move(part)};
    hint.tooltip = std::nullopt;
    return hint;
}

/**
 * @brief Emits an omitted parameter hint into an empty argument list.
 * @param[in] argListNode AST argument_list node.
 * @param[in] labelText Formatted parameter label text.
 * @param[in] hintInfo Omitted parameter metadata.
 * @param[in,out] ctx Bundled request and hints context.
 */
void EmitEmptyListOmittedHint(TSNode argListNode, const std::string& labelText,
                              const OmittedParamHint& hintInfo, OmittedHintContext& ctx)
{
    TSPoint pt = ts_node_start_point(argListNode);
    if (ts_node_child_count(argListNode) > 0)
    {
        pt = ts_node_end_point(ts_node_child(argListNode, 0));
    }
    lsp::Position pos{pt.row, pt.column};
    if (IsPositionInRange(pos, ctx.request.range))
    {
        ctx.hints.push_back(MakeOmittedDefaultHint(pos, labelText, hintInfo, ctx.request));
    }
}

/**
 * @brief Emits an omitted parameter hint into a non-empty argument list.
 * @param[in] argPositions Mapped call argument positions.
 * @param[in] paramIndex Index of the omitted parameter.
 * @param[in] hintInfo Formatted parameter label text and hover tooltip.
 * @param[in,out] ctx Bundled request and hints context.
 */
void EmitArgListOmittedHint(const std::vector<CallArgPosition>& argPositions, size_t paramIndex,
                            const OmittedParamHint& hintInfo, OmittedHintContext& ctx)
{
    const CallArgPosition* nextArg = nullptr;
    for (const auto& ap : argPositions)
    {
        if (ap.boundParamIndex > static_cast<int>(paramIndex))
        {
            nextArg = &ap;
            break;
        }
    }

    if (nextArg)
    {
        TSPoint pt = ts_node_start_point(nextArg->startNode);
        lsp::Position pos{pt.row, pt.column};
        if (IsPositionInRange(pos, ctx.request.range))
        {
            ctx.hints.push_back(MakeOmittedDefaultHint(pos, hintInfo.label + ", ", hintInfo, ctx.request));
        }
    }
    else
    {
        TSPoint pt = ts_node_end_point(argPositions.back().endNode);
        lsp::Position pos{pt.row, pt.column};
        if (IsPositionInRange(pos, ctx.request.range))
        {
            ctx.hints.push_back(MakeOmittedDefaultHint(pos, ", " + hintInfo.label, hintInfo, ctx.request));
        }
    }
}

/**
 * @brief Adds inlay hints for omitted optional parameters that have default values.
 * @param[in] callee Callee resolution result.
 * @param[in] argListNode AST argument_list node.
 * @param[in] request Inlay hint request context.
 * @param[in,out] hints Hint vector receiving generated inlay hints.
 */
void AddOmittedDefaultArgumentHints(const CalleeResolutionResult& callee, TSNode argListNode,
                                    const InlayHintRequest& request, std::vector<lsp::InlayHint>& hints)
{
    const auto& parameters = callee.parameters;
    if (request.omittedDefaultArguments == config::OmittedDefaultArgumentsMode::Off || parameters.empty() ||
        ts_node_is_null(argListNode))
    {
        return;
    }

    auto callArgs = analysis::ExtractCallArguments(argListNode, request.sourceCode);
    std::unordered_set<size_t> boundParams;
    auto argPositions = MapArgumentsToParameters(callArgs, parameters, boundParams);

    OmittedHintContext ctx{request, hints};
    bool isFirstInEmptyList = true;
    for (size_t p = 0; p < parameters.size(); ++p)
    {
        if (boundParams.contains(p))
        {
            continue;
        }
        const auto& param = parameters[p];
        if (param.defaultValue.empty() || param.name.empty() || IsVarargParameter(param))
        {
            continue;
        }

        std::string labelText = FormatOmittedDefaultLabel(param, request.omittedDefaultArguments, request.maxLength);
        std::string tooltip = "Default parameter:\n```angelscript\n" + param.typeName + " " + param.name +
                              " = " + param.defaultValue + "\n```";
        std::string targetUri = callee.fileUri.empty() ? request.uri : callee.fileUri;
        OmittedParamHint hintInfo{std::move(labelText), std::move(tooltip), std::move(targetUri), &param};

        if (argPositions.empty())
        {
            std::string fullLabel = isFirstInEmptyList ? hintInfo.label : (", " + hintInfo.label);
            EmitEmptyListOmittedHint(argListNode, fullLabel, hintInfo, ctx);
            isFirstInEmptyList = false;
        }
        else
        {
            EmitArgListOmittedHint(argPositions, p, hintInfo, ctx);
        }
    }
}

/**
 * @brief Processes a call_expression node to generate parameter inlay hints.
 * @param[in] node AST call_expression node.
 * @param[in] request Inlay hint request context.
 * @param[in,out] hints Hint vector receiving generated parameter hints.
 */
void ProcessCallExpression(TSNode node, const InlayHintRequest& request, std::vector<lsp::InlayHint>& hints)
{
    TSNode argListNode = parser::GetChildByField(node, parser::fields::Arguments);
    if (ts_node_is_null(argListNode))
    {
        uint32_t childCount = ts_node_child_count(node);
        for (uint32_t i = 0; i < childCount; ++i)
        {
            TSNode child = ts_node_child(node, i);
            if (std::string_view(ts_node_type(child)) == "argument_list")
            {
                argListNode = child;
                break;
            }
        }
    }

    if (!ts_node_is_null(argListNode))
    {
        auto args = ParseArguments(argListNode, request.sourceCode);
        auto callee = ResolveCalleeParameters(node, request, args.size());
        AddParameterHints(callee, args, request, hints);
        AddOmittedDefaultArgumentHints(callee, argListNode, request, hints);
    }
}

/**
 * @brief Generates type deduction hint for an auto variable declarator node.
 * @param[in] declarator AST variable_declarator node.
 * @param[in] request Inlay hint request context.
 * @param[in,out] hints Hint vector receiving generated type hints.
 * @param[in] isHandle Whether the auto declaration explicitly carries a handle modifier.
 */
void ProcessAutoVariableDeclarator(TSNode declarator, const InlayHintRequest& request,
                                   std::vector<lsp::InlayHint>& hints, bool isHandle)
{
    TSNode nameNode = parser::GetChildByField(declarator, parser::fields::Name);
    if (ts_node_is_null(nameNode))
    {
        return;
    }

    TSNode initExpr = parser::GetChildByField(declarator, parser::fields::Value);
    if (ts_node_is_null(initExpr))
    {
        initExpr = parser::GetChildByField(declarator, parser::fields::Arguments);
    }
    if (ts_node_is_null(initExpr))
    {
        return;
    }

    std::string deduced = DeduceExpressionType(initExpr, request);
    if (deduced.empty() || deduced == "auto" || deduced == "null" || deduced == "void")
    {
        return;
    }

    if (isHandle && !deduced.ends_with('@'))
    {
        deduced += '@';
    }

    TSPoint endPoint = ts_node_end_point(nameNode);
    lsp::Position hintPos{endPoint.row, endPoint.column};
    if (IsPositionInRange(hintPos, request.range))
    {
        lsp::InlayHint hint;
        hint.position = hintPos;
        hint.label = ": " + deduced;
        hint.kind = lsp::InlayHintKindEnum(lsp::InlayHintKind::Type);
        hint.paddingLeft = true;
        hint.paddingRight = false;
        if (!request.config || request.config->features.inlayHintsEnableTooltip)
        {
            hint.tooltip = lsp::MarkupContent{
                lsp::MarkupKindEnum(lsp::MarkupKind::Markdown),
                "Deduced type: " + deduced
            };
        }
        hints.push_back(std::move(hint));
    }
}

/**
 * @brief Locates the argument_list AST node within a variable declarator.
 * @param[in] declarator AST variable_declarator node.
 * @return AST argument_list node or null node.
 */
TSNode FindDeclaratorArgumentList(TSNode declarator)
{
    TSNode argListNode = parser::GetChildByField(declarator, parser::fields::Arguments);
    if (ts_node_is_null(argListNode))
    {
        uint32_t count = ts_node_child_count(declarator);
        for (uint32_t j = 0; j < count; ++j)
        {
            TSNode grandChild = ts_node_child(declarator, j);
            if (std::string_view(ts_node_type(grandChild)) == "argument_list")
            {
                return grandChild;
            }
        }
    }
    return argListNode;
}

/**
 * @brief Generates parameter hints for a constructor direct-initialization declarator.
 * @param[in] typeText Declared type name.
 * @param[in] declarator AST variable_declarator node.
 * @param[in] request Inlay hint request context.
 * @param[in,out] hints Hint vector receiving generated parameter hints.
 */
void ProcessConstructorInitDeclarator(const std::string& typeText, TSNode declarator, const InlayHintRequest& request,
                                      std::vector<lsp::InlayHint>& hints)
{
    TSNode argListNode = FindDeclaratorArgumentList(declarator);
    if (!ts_node_is_null(argListNode))
    {
        auto args = ParseArguments(argListNode, request.sourceCode);
        auto callee = ResolveConstructorParameters(typeText, declarator, request, args);
        AddParameterHints(callee, args, request, hints);
        AddOmittedDefaultArgumentHints(callee, argListNode, request, hints);
    }
}

/**
 * @brief Locates the type AST node within a variable_declaration node.
 * @param[in] varDeclNode AST variable_declaration node.
 * @return AST type node or null node.
 */
TSNode FindVariableTypeNode(TSNode varDeclNode)
{
    TSNode varTypeNode = parser::GetChildByField(varDeclNode, parser::fields::VarType);
    if (ts_node_is_null(varTypeNode))
    {
        uint32_t count = ts_node_child_count(varDeclNode);
        for (uint32_t i = 0; i < count; ++i)
        {
            TSNode child = ts_node_child(varDeclNode, i);
            if (std::string_view(ts_node_type(child)) == "type")
            {
                return child;
            }
        }
    }
    return varTypeNode;
}

/**
 * @brief Processes a variable_declaration AST node for auto type hints or direct-initialization hints.
 * @param[in] node AST variable_declaration node.
 * @param[in] request Inlay hint request context.
 * @param[in,out] hints Hint vector receiving generated inlay hints.
 */
void ProcessVariableDeclaration(TSNode node, const InlayHintRequest& request, std::vector<lsp::InlayHint>& hints)
{
    TSNode varTypeNode = FindVariableTypeNode(node);
    if (ts_node_is_null(varTypeNode))
    {
        return;
    }

    std::string typeText = GetNodeText(varTypeNode, request.sourceCode);
    bool isAuto = (analysis::CleanBaseType(typeText) == "auto");
    bool isHandle = (typeText.find('@') != std::string::npos);
    uint32_t count = ts_node_child_count(node);
    for (uint32_t i = 0; i < count; ++i)
    {
        TSNode child = ts_node_child(node, i);
        if (std::string_view(ts_node_type(child)) == "variable_declarator")
        {
            if (isAuto)
            {
                ProcessAutoVariableDeclarator(child, request, hints, isHandle);
            }
            else if (!typeText.empty())
            {
                ProcessConstructorInitDeclarator(typeText, child, request, hints);
            }
        }
    }
}

/**
 * @brief Iteratively traverses the AST using a flat Tree-Sitter cursor to collect inlay hints.
 * @param[in] rootNode Root AST node of the document.
 * @param[in] request Inlay hint request context.
 * @param[in,out] hints Inlay hints vector receiving collected items.
 */
void CollectInlayHints(TSNode rootNode, const InlayHintRequest& request, std::vector<lsp::InlayHint>& hints)
{
    if (ts_node_is_null(rootNode) || !IsNodeOverlappingRange(rootNode, request.range))
    {
        return;
    }

    TSTreeCursor cursor = ts_tree_cursor_new(rootNode);
    bool descending = true;

    while (true)
    {
        TSNode currentNode = ts_tree_cursor_current_node(&cursor);
        bool overlaps = IsNodeOverlappingRange(currentNode, request.range);

        if (descending && overlaps)
        {
            std::string_view nodeType = ts_node_type(currentNode);
            if (nodeType == "call_expression")
            {
                ProcessCallExpression(currentNode, request, hints);
            }
            else if (nodeType == "variable_declaration")
            {
                ProcessVariableDeclaration(currentNode, request, hints);
            }

            if (ts_tree_cursor_goto_first_child(&cursor))
            {
                continue;
            }
        }

        if (ts_node_eq(currentNode, rootNode))
        {
            break;
        }

        if (ts_tree_cursor_goto_next_sibling(&cursor))
        {
            descending = true;
            continue;
        }

        if (!ts_tree_cursor_goto_parent(&cursor))
        {
            break;
        }
        descending = false;
    }

    ts_tree_cursor_delete(&cursor);
}
} // namespace

std::optional<InlayHintResult> GetInlayHints(const InlayHintRequest& request)
{
    if (!request.tree || request.sourceCode.empty())
    {
        return std::nullopt;
    }

    if (request.logger && request.logger->IsDebugEnabled())
    {
        request.logger->LogDebug(fmt::format("[InlayHint] Computing inlay hints for URI: {}", request.uri));
    }

    TSNode rootNode = ts_tree_root_node(request.tree);
    if (ts_node_is_null(rootNode))
    {
        return std::nullopt;
    }

    std::vector<lsp::InlayHint> hints;
    CollectInlayHints(rootNode, request, hints);

    std::stable_sort(hints.begin(), hints.end(),
                     [](const lsp::InlayHint& a, const lsp::InlayHint& b)
                     {
                         if (a.position.line != b.position.line)
                         {
                             return a.position.line < b.position.line;
                         }
                         return a.position.character < b.position.character;
                     });

    if (request.logger && request.logger->IsTraceEnabled())
    {
        request.logger->LogTrace(fmt::format("[InlayHint] Computed {} hints for URI: {}", hints.size(), request.uri));
    }

    return hints;
}
} // namespace angel_lsp::features
