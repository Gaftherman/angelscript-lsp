#include "features/signature_help/SignatureHelpHandler.h"
#include "analysis/SemanticHelpers.h"
#include "parser/GrammarNames.h"
#include "utils/Utils.h"
#include <algorithm>
#include <sstream>
#include <unordered_set>
#include <vector>

namespace angel_lsp::features
{
namespace
{

/**
 * @brief Appends parameter declarations into comma-separated text in result.
 * @param[in,out] result Destination string.
 * @param[in] parameters List of parameter symbols.
 */
void AppendParameters(std::string& result, const std::vector<analysis::ParameterInformation>& parameters)
{
    for (size_t i = 0; i < parameters.size(); ++i)
    {
        if (i > 0)
        {
            result += ", ";
        }
        const auto& param = parameters[i];
        if (!param.typeName.empty())
        {
            result += param.typeName;
        }
        if (!param.name.empty())
        {
            if (!result.empty() && result.back() != ' ')
            {
                result += " ";
            }
            result += param.name;
        }
        if (!param.defaultValue.empty())
        {
            result += " = ";
            result += param.defaultValue;
        }
    }
}

/**
 * @brief Formats human-readable signature label for a function or funcdef symbol.
 * @param[in] sym Symbol representing a function or funcdef.
 * @return Formatted signature string.
 */
std::string FormatSignatureLabel(const analysis::Symbol& sym)
{
    if (sym.type != analysis::SymbolType::Function && sym.type != analysis::SymbolType::Funcdef)
    {
        return "";
    }

    std::string result;
    result.reserve(64);
    std::string_view returnType;
    const std::vector<analysis::ParameterInformation>* parameters = nullptr;

    if (sym.type == analysis::SymbolType::Funcdef)
    {
        result += "funcdef ";
        const auto& sig = sym.GetFuncdef();
        returnType = sig.returnType;
        parameters = &sig.parameters;
    }
    else
    {
        const auto& sig = sym.GetFunction();
        returnType = sig.returnType;
        parameters = &sig.parameters;
    }

    if (returnType.empty())
    {
        result += "void ";
    }
    else
    {
        result += returnType;
        result += " ";
    }

    if (!sym.containerName.empty())
    {
        result += sym.containerName;
        result += "::";
    }
    result += sym.name;
    result += "(";

    if (parameters)
    {
        AppendParameters(result, *parameters);
    }

    result += ")";
    return result;
}

/**
 * @brief Enclosing call expression and argument list nodes.
 */
struct CallNodes
{
    TSNode callNode{};
    TSNode argListNode{};
};

/**
 * @brief Traverses ancestors of a node to identify enclosing call_expression and argument_list.
 * @param[in] node Starting AST descendant.
 * @return Identified CallNodes.
 */
CallNodes FindCallNodes(TSNode node)
{
    CallNodes result;
    for (TSNode cur = node; !ts_node_is_null(cur); cur = ts_node_parent(cur))
    {
        std::string_view type = ts_node_type(cur);
        if (type == "argument_list")
        {
            result.argListNode = cur;
            TSNode parent = ts_node_parent(cur);
            if (!ts_node_is_null(parent) && std::string_view(ts_node_type(parent)) == "call_expression")
            {
                result.callNode = parent;
                break;
            }
        }
        else if (type == "call_expression")
        {
            result.callNode = cur;
            break;
        }
    }

    if (!ts_node_is_null(result.callNode) && ts_node_is_null(result.argListNode))
    {
        result.argListNode = analysis::ResolveArgumentListNode(result.callNode);
    }
    return result;
}

/**
 * @brief Determines active parameter index for the call at the given request position.
 * @param[in] request Signature help request.
 * @param[in] argListNode Argument list AST node.
 * @return Zero-based parameter index.
 */
uint32_t DetermineActiveParameter(const SignatureHelpRequest& request, TSNode argListNode)
{
    const size_t cursorByte = utils::PositionToOffset(
        request.sourceCode, request.position.line, request.position.character, utils::PositionEncoding::Utf16);
    return analysis::CalculateActiveCallParameter(argListNode, cursorByte, request.sourceCode);
}

/**
 * @brief Extracts the function/callee AST node from a call_expression node.
 * @param[in] callNode Call expression AST node.
 * @return Function AST node or null node if not found.
 */
TSNode ExtractFunctionNode(TSNode callNode)
{
    TSNode funcNode = parser::GetChildByField(callNode, parser::fields::Function);
    if (ts_node_is_null(funcNode))
    {
        uint32_t childCount = ts_node_child_count(callNode);
        if (childCount > 0)
        {
            funcNode = ts_node_child(callNode, 0);
        }
    }
    return funcNode;
}

/**
 * @brief Resolves candidate method symbols for a member expression callee.
 * @param[in] request Signature help request.
 * @param[in] funcNode Member expression AST node.
 * @return Matching symbol candidates from the receiver hierarchy.
 */
std::vector<const analysis::Symbol*> ResolveMemberCandidates(const SignatureHelpRequest& request, TSNode funcNode)
{
    TSNode objNode = parser::GetChildByField(funcNode, parser::fields::Object);
    TSNode memNode = parser::GetChildByField(funcNode, parser::fields::Member);
    if (ts_node_is_null(objNode) || ts_node_is_null(memNode))
    {
        return {};
    }

    const uint32_t memStart = ts_node_start_byte(memNode);
    const uint32_t memEnd = ts_node_end_byte(memNode);
    if (memStart >= request.sourceCode.size() || memEnd > request.sourceCode.size() || memStart >= memEnd)
    {
        return {};
    }
    const std::string_view memText = std::string_view(request.sourceCode).substr(memStart, memEnd - memStart);

    auto rootScope = request.scopeIndex.GetRoot(request.uri);
    const analysis::Scope* scope =
        rootScope ? FindInnermostScope(rootScope.get(), request.position.line, request.position.character) : nullptr;
    std::string receiverTypeName =
        analysis::ResolveReceiverType(objNode, request.sourceCode, request.symbolTable, {scope, "", request.uri});

    if (receiverTypeName.empty())
    {
        return {};
    }

    auto hierarchy = analysis::GetInheritedTypeHierarchy(receiverTypeName, request.symbolTable);
    for (const auto& typeName : hierarchy)
    {
        std::string qualifiedName = typeName + "::" + std::string(memText);
        auto found = request.symbolTable.FindSymbolsPtr(qualifiedName);
        if (found && !found->empty())
        {
            std::vector<const analysis::Symbol*> ptrs;
            ptrs.reserve(found->size());
            for (const auto& sym : *found)
            {
                ptrs.push_back(&sym);
            }
            return ptrs;
        }
    }
    return {};
}

/**
 * @brief Builds LSP parameter information objects for a symbol's parameter list.
 * @param[in] parameters List of symbol parameters.
 * @return Vector of LSP ParameterInformation.
 */
std::vector<lsp::ParameterInformation> BuildParameterList(const std::vector<analysis::ParameterInformation>& parameters)
{
    std::vector<lsp::ParameterInformation> params;
    params.reserve(parameters.size());

    for (const auto& param : parameters)
    {
        lsp::ParameterInformation pInfo;
        std::string pLabel;
        pLabel.reserve(param.typeName.size() + param.name.size() + param.defaultValue.size() + 4);
        if (!param.typeName.empty())
        {
            pLabel += param.typeName;
        }
        if (!param.name.empty())
        {
            if (!pLabel.empty())
            {
                pLabel += " ";
            }
            pLabel += param.name;
        }
        if (!param.defaultValue.empty())
        {
            pLabel += " = ";
            pLabel += param.defaultValue;
        }
        pInfo.label = std::move(pLabel);
        params.push_back(std::move(pInfo));
    }
    return params;
}

/**
 * @brief Appends candidate signature to LSP signature list if valid.
 * @tparam T Candidate type (Symbol or const Symbol*).
 * @param[in,out] signatures Destination signature list.
 * @param[in] candidate Candidate symbol reference or pointer.
 */
template <typename T>
void AppendCandidateSignature(std::vector<lsp::SignatureInformation>& signatures, const T& candidate)
{
    const analysis::Symbol* symPtr = nullptr;
    if constexpr (std::is_pointer_v<T>)
    {
        symPtr = candidate;
    }
    else
    {
        symPtr = &candidate;
    }

    if (!symPtr || (symPtr->type != analysis::SymbolType::Function && symPtr->type != analysis::SymbolType::Funcdef))
    {
        return;
    }

    lsp::SignatureInformation sigInfo;
    sigInfo.label = FormatSignatureLabel(*symPtr);
    const std::vector<analysis::ParameterInformation>* parameters = (symPtr->type == analysis::SymbolType::Function)
                                                                        ? &symPtr->GetFunction().parameters
                                                                        : &symPtr->GetFuncdef().parameters;

    if (parameters && !parameters->empty())
    {
        sigInfo.parameters = BuildParameterList(*parameters);
    }

    signatures.push_back(std::move(sigInfo));
}

/**
 * @brief Constructs LSP signature information objects for candidate symbols.
 * @tparam Range Range of Symbol or Symbol pointers.
 * @param[in] candidateSymbols Candidate function or funcdef symbols.
 * @return List of LSP signature entries.
 */
template <typename Range> std::vector<lsp::SignatureInformation> BuildSignatureList(const Range& candidateSymbols)
{
    std::vector<lsp::SignatureInformation> signatures;
    signatures.reserve(std::size(candidateSymbols));

    for (const auto& item : candidateSymbols)
    {
        AppendCandidateSignature(signatures, item);
    }
    return signatures;
}

/**
 * @brief Selects active signature matching the active parameter index.
 * @param[in] signatures Available signature list.
 * @param[in] activeParameter Active parameter index.
 * @return Zero-based index of the matching signature.
 */
uint32_t SelectActiveSignature(const std::vector<lsp::SignatureInformation>& signatures, uint32_t activeParameter)
{
    for (size_t i = 0; i < signatures.size(); ++i)
    {
        if (signatures[i].parameters.has_value() && activeParameter < signatures[i].parameters->size())
        {
            return static_cast<uint32_t>(i);
        }
    }
    return 0;
}

} // namespace

std::optional<lsp::SignatureHelp> GetSignatureHelp(const SignatureHelpRequest& request)
{
    if (!request.tree || request.sourceCode.empty())
    {
        return std::nullopt;
    }

    TSNode rootNode = ts_tree_root_node(request.tree);
    TSPoint point = {request.position.line, request.position.character};
    TSNode node = ts_node_descendant_for_point_range(rootNode, point, point);

    if (ts_node_is_null(node))
    {
        return std::nullopt;
    }

    CallNodes callNodes = FindCallNodes(node);
    if (ts_node_is_null(callNodes.callNode))
    {
        return std::nullopt;
    }

    uint32_t activeParameter = DetermineActiveParameter(request, callNodes.argListNode);

    TSNode funcNode = ExtractFunctionNode(callNodes.callNode);
    if (ts_node_is_null(funcNode))
    {
        return std::nullopt;
    }

    std::vector<lsp::SignatureInformation> signatures;
    std::string_view funcType = ts_node_type(funcNode);
    if (funcType == "member_expression")
    {
        auto candidateSymbols = ResolveMemberCandidates(request, funcNode);
        signatures = BuildSignatureList(candidateSymbols);
    }
    else
    {
        uint32_t fStart = ts_node_start_byte(funcNode);
        uint32_t fEnd = ts_node_end_byte(funcNode);
        if (fStart < request.sourceCode.size() && fEnd <= request.sourceCode.size() && fStart < fEnd)
        {
            std::string_view calleeName = std::string_view(request.sourceCode).substr(fStart, fEnd - fStart);
            auto found = request.symbolTable.FindSymbolsPtr(calleeName);
            if (found && !found->empty())
            {
                signatures = BuildSignatureList(*found);
            }
        }
    }

    if (signatures.empty())
    {
        return std::nullopt;
    }

    uint32_t activeSignature = SelectActiveSignature(signatures, activeParameter);

    lsp::SignatureHelp result;
    result.signatures = std::move(signatures);
    result.activeSignature = activeSignature;
    result.activeParameter = activeParameter;

    return result;
}

} // namespace angel_lsp::features
