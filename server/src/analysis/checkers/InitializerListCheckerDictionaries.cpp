#include "analysis/checkers/InitializerListCheckerCommon.h"
#include "analysis/index/RuleIndex.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace angel_lsp::analysis
{
/**
 * @brief Inspects a function symbol for associative dictionary signatures (`set`, `get`, `exists`).
 * @param[in] m     Candidate function symbol.
 * @param[in,out] state Dictionary type state sink.
 */
static void UpdateDictValType(const ParameterInformation& param, DictTypeState& state)
{
    const std::string& paramVal = param.baseTypeName.empty() ? param.typeName : param.baseTypeName;
    if (paramVal == "?" || param.typeName.find('?') != std::string::npos)
    {
        state.valType = "?";
    }
    else if (state.valType.empty())
    {
        state.valType = paramVal;
    }
    else if (state.valType != paramVal)
    {
        state.valType = "?";
    }
}

static void InspectDictFunctionSignature(const Symbol& m, DictTypeState& state)
{
    if (m.type != SymbolType::Function || !std::holds_alternative<FunctionSignature>(m.signature))
    {
        return;
    }
    const auto& fn = m.GetFunction();
    const bool isSetOrGet = (m.name == "set" || m.name == "get") && fn.parameters.size() >= 2;
    const bool isExists = (m.name == "exists") && !fn.parameters.empty();
    if (!isSetOrGet && !isExists)
    {
        return;
    }
    state.isAssociative = true;
    if (state.keyType.empty())
    {
        state.keyType =
            fn.parameters[0].baseTypeName.empty() ? fn.parameters[0].typeName : fn.parameters[0].baseTypeName;
    }
    if (isSetOrGet)
    {
        UpdateDictValType(fn.parameters[1], state);
    }
}

/**
 * @brief Inspects member methods and keys for associative container semantics.
 * @param[in] containerKey Container key in rule index.
 * @param[in] argCount     Template argument count.
 * @param[in] ctx          Diagnostic context.
 * @param[in,out] state        Dictionary type state sink.
 */
static void InspectClassDictMembers(const std::string& containerKey, size_t argCount, const DiagnosticContext& ctx,
                                    DictTypeState& state)
{
    const auto& members = ctx.request.GetRuleIndex().Members(containerKey);
    if (members.methodNames.contains("exists"))
    {
        state.isAssociative = true;
    }
    if (argCount >= 2 && (members.methodNames.contains("insert") || members.methodNames.contains("opIndex") ||
                          members.methodNames.contains("find") || members.methodNames.contains("set")))
    {
        state.isAssociative = true;
    }

    for (const auto& key : members.memberKeys)
    {
        if (const auto mSyms = ctx.request.symbolTable.FindSymbolsPtr(key))
        {
            for (const auto& m : *mSyms)
            {
                InspectDictFunctionSignature(m, state);
            }
        }
    }
}

/**
 * @brief Inspects a single class symbol for dictionary semantics.
 * @param[in] sym      Candidate class symbol.
 * @param[in] spelling Parsed template spelling.
 * @param[in] ctx      Diagnostic context.
 * @param[in,out] state    Dictionary type state sink.
 */
static void InspectClassDict(const Symbol& sym, const TemplateSpelling& spelling, const DiagnosticContext& ctx,
                             DictTypeState& state)
{
    if (sym.type != SymbolType::Class)
    {
        return;
    }
    const auto containers = CandidateContainerKeys(sym, spelling.name);
    for (const auto& containerKey : containers)
    {
        InspectClassDictMembers(containerKey, spelling.arguments.size(), ctx, state);
    }
}

/**
 * @brief Inspects whether a type represents an associative container and resolves its key and value types.
 * @param[in] spelling Parsed template spelling of the container.
 * @param[in] ctx      Diagnostic context.
 * @return Resolved dictionary info.
 */
DictInfo InspectDictionary(const TemplateSpelling& spelling, const DiagnosticContext& ctx)
{
    if (spelling.name.empty())
    {
        return {false, "", ""};
    }

    DictTypeState state;
    if (spelling.arguments.size() >= 2)
    {
        state.keyType = spelling.arguments[0];
        state.valType = spelling.arguments[1];
    }
    else if (spelling.arguments.size() == 1)
    {
        state.valType = spelling.arguments[0];
    }

    const auto syms = ctx.request.symbolTable.FindSymbolsPtr(spelling.name);
    const auto typeSymbols = syms ? *syms : ctx.request.symbolTable.FindTypeSymbolsByShortName(spelling.name);

    for (const auto& sym : typeSymbols)
    {
        InspectClassDict(sym, spelling, ctx, state);
    }

    if (!state.isAssociative)
    {
        return {false, "", ""};
    }

    if (state.keyType.empty())
    {
        state.keyType = ctx.request.symbolTable.GetStringType();
    }
    if (state.valType.empty())
    {
        state.valType = "?";
    }

    return {true, StripDecorations(state.keyType), StripDecorations(state.valType)};
}

/**
 * @brief Validates a single dictionary key-value pair in an initializer list.
 * @param[in] child    Pair initializer list node.
 * @param[in] dictInfo Dictionary key and value type information.
 * @param[in] valCtx   Validation context.
 */
static void ValidateDictionaryPair(TSNode child, const DictInfo& dictInfo, const ListValidationContext& valCtx)
{
    const auto& gs = parser::GetGrammarSymbols();
    TSNode keyNode{};
    TSNode valNode{};
    uint32_t pairCount = 0;

    parser::ForEachChildNode(child,
                             [&](TSNode elem)
                             {
                                 if (!ts_node_is_named(elem) || ts_node_symbol(elem) == gs.symComment)
                                 {
                                     return;
                                 }
                                 if (pairCount == 0)
                                 {
                                     keyNode = elem;
                                 }
                                 else if (pairCount == 1)
                                 {
                                     valNode = elem;
                                 }
                                 ++pairCount;
                             });

    if (!ts_node_is_null(keyNode))
    {
        if (ts_node_symbol(keyNode) == gs.symInitializerList)
        {
            valCtx.ctx.EmitAtNodeRange(ToOptionalSourceRange(keyNode), "as-err-initializer-list-not-supported",
                                       dictInfo.keyType);
        }
        else
        {
            CheckElementValue(keyNode, dictInfo.keyType, valCtx.ctx, valCtx.elements);
        }
    }
    if (!ts_node_is_null(valNode))
    {
        if (ts_node_symbol(valNode) == gs.symInitializerList)
        {
            valCtx.ctx.EmitAtNodeRange(ToOptionalSourceRange(valNode), "as-err-initializer-list-not-supported",
                                       dictInfo.valType);
        }
        else
        {
            CheckElementValue(valNode, dictInfo.valType, valCtx.ctx, valCtx.elements);
        }
    }
}

/**
 * @brief Validates an initializer list against expected dictionary key-value semantics.
 * @param[in] listNode Initializer list node.
 * @param[in] dictInfo Dictionary key and value type information.
 * @param[in] valCtx   Validation context.
 */
void ValidateDictionaryList(TSNode listNode, const DictInfo& dictInfo, const ListValidationContext& valCtx)
{
    const auto& gs = parser::GetGrammarSymbols();
    parser::ForEachChildNode(
        listNode,
        [&](TSNode child)
        {
            if (!ts_node_is_named(child) || ts_node_symbol(child) == gs.symComment)
            {
                return;
            }
            if (ts_node_symbol(child) != gs.symInitializerList)
            {
                valCtx.ctx.EmitAtNodeRange(ToOptionalSourceRange(child), "as-err-initializer-list-expected", "");
                return;
            }

            const uint32_t valuesCount = ListValueCount(child);
            if (valuesCount < 2)
            {
                valCtx.ctx.EmitAtNodeRange(ToOptionalSourceRange(child), "as-err-initializer-list-too-few", "");
            }
            else if (valuesCount > 2)
            {
                valCtx.ctx.EmitAtNodeRange(ToOptionalSourceRange(child), "as-err-initializer-list-too-many", "");
            }

            ValidateDictionaryPair(child, dictInfo, valCtx);
        });
}
} // namespace angel_lsp::analysis
