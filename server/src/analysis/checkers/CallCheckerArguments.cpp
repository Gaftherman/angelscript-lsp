#include "analysis/checkers/CallCheckerCommon.h"
#include "analysis/checkers/InitializerListChecker.h"
#include "analysis/diagnostics/DiagnosticCodes.h"
#include "analysis/index/SemanticNodes.h"
#include "analysis/resolution/AmbiguityDetector.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/types/ExpressionTypeCache.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"
#include "parser/Keywords.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace angel_lsp::analysis
{

uint32_t CountArguments(TSNode argumentList)
{
    const ast::CallExpressionView callView(argumentList, "");
    return callView.ArgumentCount();
}

std::vector<TSNode> GetArgumentNodes(TSNode argumentList)
{
    const ast::CallExpressionView callView(argumentList, "");
    return callView.ArgumentNodes();
}

static std::vector<std::string> GetArgumentNames(TSNode argumentList, std::string_view sourceCode)
{
    const ast::CallExpressionView callView(argumentList, sourceCode);
    return callView.ArgumentNames();
}

bool ValidateArgumentOrdering(TSNode arguments, bool& sawNamedArg, DiagnosticContext& ctx)
{
    sawNamedArg = false;
    auto callArgs = ExtractCallArguments(arguments, "");
    for (const auto& arg : callArgs)
    {
        bool isNamed = !ts_node_is_null(arg.nameNode);
        if (isNamed)
        {
            sawNamedArg = true;
        }
        else if (sawNamedArg)
        {
            if (!ts_node_is_null(arg.exprNode))
            {
                const TSPoint aStart = ts_node_start_point(arg.exprNode);
                const TSPoint aEnd = ts_node_end_point(arg.exprNode);
                ctx.EmitAtRange({aStart.row, aStart.column, aEnd.row, aEnd.column},
                                "as-err-positional-after-named-arg");
            }
            return false;
        }
    }
    return true;
}

static bool IsAssignableLValueSymbol(std::string_view name, const Scope* scope, const SymbolTable& table,
                                     uint32_t refByte = UINT32_MAX)
{
    if (scope)
    {
        const auto* def = ResolveInScopeAt(scope, name, refByte);
        if (def && (def->kind == LocalDefinitionKind::Variable || def->kind == LocalDefinitionKind::Parameter ||
                    def->kind == LocalDefinitionKind::Field))
        {
            return !HasConstModifier(def->typeName);
        }
    }
    auto syms = table.FindSymbolsPtr(name);
    if (syms)
    {
        for (const auto& s : *syms)
        {
            if (s.type == SymbolType::Variable && !s.GetVariable().modifiers.isConst)
            {
                return true;
            }
        }
    }
    return false;
}

static bool CheckArgIsLValue(TSNode argNode, std::string_view sourceCode, const Scope* scope, const SymbolTable& table)
{
    const auto& gs = parser::GetGrammarSymbols();
    TSSymbol aSym = ts_node_symbol(argNode);
    std::string aText(parser::NodeText(argNode, sourceCode));
    TrimString(aText);

    if (aSym == gs.symUnaryExpression && aText.starts_with("@"))
    {
        TSNode operand = parser::GetChildByField(argNode, gs.fieldOperand);
        if (!ts_node_is_null(operand))
        {
            argNode = operand;
            aSym = ts_node_symbol(argNode);
            aText = parser::NodeText(argNode, sourceCode);
            TrimString(aText);
        }
    }

    if (aSym == gs.symThisExpression || aText == parser::keywords::This)
    {
        return true;
    }

    if (aSym == gs.symIdentifier || aSym == gs.symScopedIdentifier)
    {
        return IsAssignableLValueSymbol(aText, scope, table, ts_node_start_byte(argNode));
    }
    return (aSym == gs.symMemberExpression || aSym == gs.symIndexExpression);
}

TSNode UnwrapParenNode(TSNode node)
{
    const auto& gs = parser::GetGrammarSymbols();
    while (!ts_node_is_null(node) && ts_node_symbol(node) == gs.symParenthesizedExpression)
    {
        TSNode child = ts_node_named_child(node, 0);
        if (ts_node_is_null(child))
        {
            break;
        }
        node = child;
    }
    return node;
}

CallResolutionContext ResolveCallArguments(const CallValidationContext& valCtx)
{
    CallResolutionContext result;
    result.argNodes = GetArgumentNodes(valCtx.arguments);
    const size_t argCount = result.argNodes.size();
    result.argTypes.reserve(argCount);
    result.argIsLValue.reserve(argCount);
    result.argNames = GetArgumentNames(valCtx.arguments, valCtx.request.sourceCode);

    for (const auto& argNode : result.argNodes)
    {
        result.argIsLValue.push_back(
            CheckArgIsLValue(argNode, valCtx.request.sourceCode, valCtx.scope, valCtx.ctx.request.symbolTable));

        if (auto dataTypeName = IsBareDataType(argNode, ExpressionTypeContext(valCtx.scope, valCtx.ctx)))
        {
            if (*dataTypeName == "void")
            {
                result.argTypes.push_back("void");
                continue;
            }
            const TSPoint aStart = ts_node_start_point(argNode);
            const TSPoint aEnd = ts_node_end_point(argNode);
            valCtx.ctx.EmitAtRange({aStart.row, aStart.column, aEnd.row, aEnd.column},
                                   diagnostics::codes::ExpressionIsDataType, *dataTypeName);
            result.allArgsResolved = false;
            result.argTypes.push_back("");
            continue;
        }

        std::string argType = ResolveExpressionType(argNode, ExpressionTypeContext(valCtx.scope, valCtx.ctx));
        if (argType.empty() || ExpressionTypeCache::IsPoisonType(argType))
        {
            const auto& gs = parser::GetGrammarSymbols();
            TSNode unwrapped = UnwrapParenNode(argNode);
            if (ts_node_symbol(unwrapped) == gs.symNullLiteral)
            {
                argType = "null";
            }
            else if (ts_node_symbol(unwrapped) == gs.symInitializerList)
            {
                argType = constants::k_initializerListTypeSentinel;
            }
            else
            {
                result.allArgsResolved = false;
            }
        }
        else if (!HasConstModifier(argType) && IsObjectNodeConst(argNode, valCtx))
        {
            argType.insert(0, "const ");
        }
        result.argTypes.push_back(std::move(argType));
    }
    return result;
}

std::vector<const Symbol*> FilterMatchingArityCandidates(const std::vector<Symbol>& candidates, uint32_t argumentCount)
{
    std::vector<const Symbol*> matching;
    matching.reserve(candidates.size());
    for (const auto& sym : candidates)
    {
        if (std::holds_alternative<FunctionSignature>(sym.signature))
        {
            const auto arity = InspectFunctionArity(sym.GetFunction());
            if (IsArityCompatible(arity, argumentCount))
            {
                matching.push_back(&sym);
            }
        }
    }
    return matching;
}

void CheckInitializerListArgs(const CallResolutionContext& args, const FunctionSignature& fn,
                              const CallValidationContext& valCtx)
{
    const auto& gs = parser::GetGrammarSymbols();
    for (size_t i = 0; i < args.argNodes.size(); ++i)
    {
        if (ts_node_symbol(args.argNodes[i]) != gs.symInitializerList)
        {
            continue;
        }

        const std::string_view argName =
            (i < args.argNames.size()) ? std::string_view(args.argNames[i]) : std::string_view{};
        size_t paramIdx = size_t(-1);
        if (!argName.empty())
        {
            for (size_t p = 0; p < fn.parameters.size(); ++p)
            {
                if (fn.parameters[p].name == argName)
                {
                    paramIdx = p;
                    break;
                }
            }
        }
        else if (i < fn.parameters.size())
        {
            paramIdx = i;
        }

        if (paramIdx < fn.parameters.size())
        {
            CheckInitializerListAgainstType(args.argNodes[i], fn.parameters[paramIdx].typeName,
                                            {valCtx.request.sourceCode, valCtx.scope}, valCtx.ctx);
        }
    }
}

void ValidateOutArguments(const Symbol& candidate, const std::vector<TSNode>& argNodes,
                          const CallValidationContext& valCtx)
{
    const auto& fn = candidate.GetFunction();
    for (size_t i = 0; i < argNodes.size() && i < fn.parameters.size(); ++i)
    {
        const auto& param = fn.parameters[i];
        const bool isMutableRef = !param.isConst && (param.isOutRef || param.isInOutRef || param.isStandaloneRef);
        if (!isMutableRef)
        {
            continue;
        }

        std::string_view aText = parser::NodeText(argNodes[i], valCtx.request.sourceCode);
        TrimTypeWhitespace(aText);
        if (aText == "void" && param.isOutRef)
        {
            continue;
        }

        if (!CheckArgIsLValue(argNodes[i], valCtx.request.sourceCode, valCtx.scope, valCtx.ctx.request.symbolTable))
        {
            const TSPoint aStart = ts_node_start_point(argNodes[i]);
            const TSPoint aEnd = ts_node_end_point(argNodes[i]);
            valCtx.ctx.EmitAtRange({aStart.row, aStart.column, aEnd.row, aEnd.column},
                                   "as-err-lvalue-required-for-out-param");
        }
    }
}

} // namespace angel_lsp::analysis
