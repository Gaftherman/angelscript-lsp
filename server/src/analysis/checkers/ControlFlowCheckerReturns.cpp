#include "analysis/checkers/ControlFlowCheckerCommon.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarNames.h"
#include "parser/Primitives.h"
#include "utils/Utils.h"

#include <string>
#include <string_view>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{

static TSSymbol NodeSymbol(TSNode node)
{
    return ts_node_is_null(node) ? 0 : ts_node_symbol(node);
}

/**
 * @brief True for a return type that null cannot convert to.
 *        The primitives, by name. Deliberately not "anything that is not a handle": a value type
 */
static bool IsNonNullablePrimitiveName(std::string_view typeName)
{
    return parser::primitives::IsNonNullable(typeName);
}

void ControlFlowVisitor::CheckReturnStatement(TSNode node, const ControlFlowState& state)
{
    // A bare `return;` in a function that owes a value. Measured: the compiler answers
    // "Must return a value" and rejects the file, and nothing here saw it -
    // as-err-not-all-paths-return asks only whether a return is *reached*, so
    // `float FS(float f) { return; }` passed every check this analyzer had.
    if (!state.requiredReturn.empty() && state.requiredReturn != "void" && ts_node_named_child_count(node) == 0)
    {
        m_ctx.EmitAtNodeRange(ToOptionalSourceRange(node), "as-err-return-value-required", state.functionName);
    }

    // The constructor half of the same rule. Reported here rather than in
    // TypeConversionChecker because that one asks the return type node what is required,
    // and a constructor has none - so the one function in the language that can only
    // return void was the one nothing checked.
    if (state.implicitVoid && ts_node_named_child_count(node) > 0)
    {
        m_ctx.EmitAtNodeRange(ToOptionalSourceRange(node), "as-err-void-return-value");
    }

    // `int f() { return null; }` - "No conversion from '<null handle>' to 'int' available."
    // as-err-null-non-handle says exactly this about a variable and had nothing to say
    // about a return. Restricted to the primitives, and to a return type carrying no `@`,
    // so a class or a handle - where null may well be legal, and where the host may have
    // registered a conversion this analyzer cannot see - is left alone.
    if (!state.requiredReturn.empty() && !IsHandleTypeView(state.requiredReturn) &&
        IsNonNullablePrimitiveName(state.requiredReturn) && ts_node_named_child_count(node) == 1 &&
        NodeSymbol(ts_node_named_child(node, 0)) == parser::GetGrammarSymbols().symNullLiteral)
    {
        m_ctx.EmitAtNodeRange(ToOptionalSourceRange(node), "as-err-null-non-handle", state.requiredReturn);
    }
}

void ControlFlowVisitor::CheckFunctionDeclaration(TSNode node, ControlFlowState& state)
{
    // A nested function opens its own flow: a loop enclosing the declaration does not
    // make a `break` inside the nested body legal.
    state = ControlFlowState{};

    TSNode body = parser::GetChildByField(node, parser::GetGrammarSymbols().fieldBody);
    TSNode returnType = parser::GetChildByField(node, parser::GetGrammarSymbols().fieldReturnType);
    TSNode name = parser::GetChildByField(node, parser::GetGrammarSymbols().fieldName);

    // What this body is required to return, and under whose name to say so.
    //
    // A lambda has neither: the grammar gives `lambda_expression` a parameter list and
    // a body and nothing else, so `return_type` is null and this check simply never
    // ran for one. The requirement comes from the funcdef it is being handed to -
    // `funcdef int CB(); CB@ cb = function() { };` is "Not all paths return a value"
    // to the real compiler - and that funcdef is also the only name there is to
    // report, so the message names it and the diagnostic is anchored on the lambda.
    std::string requiredReturn;
    std::string reportedName;
    if (!ts_node_is_null(returnType))
    {
        requiredReturn = utils::TrimView(parser::NodeText(returnType, m_sourceCode));
        reportedName = std::string(parser::NodeText(name, m_sourceCode));
    }
    else if (NodeSymbol(node) == parser::GetGrammarSymbols().symLambdaExpression)
    {
        if (const auto target = FuncdefTargetOfLambda(node, m_ctx.request.symbolTable, m_sourceCode))
        {
            requiredReturn = CleanBaseType(target->GetFuncdef().returnType);
            reportedName = target->name;
        }
    }

    if (!ts_node_is_null(body) && !requiredReturn.empty() && requiredReturn != "void")
    {
        const bool hasSyntaxError =
            ts_node_has_error(body) || (m_ctx.request.nodeIndex && m_ctx.request.nodeIndex->NodeHasSyntaxError(body));
        if (!hasSyntaxError && !m_index.CallableBodyDefinitelyReturns(ts_node_start_byte(body)))
        {
            m_ctx.EmitAtNodeRange(ToOptionalSourceRange(ts_node_is_null(name) ? node : name),
                                  "as-err-not-all-paths-return", reportedName);
        }
    }

    // Carried into the body, so every `return` inside it knows what it owes. A lambda
    // whose funcdef target could not be resolved leaves this empty and the check below
    // does nothing, which is the same silence the rule above keeps.
    state.requiredReturn = requiredReturn;
    state.functionName = reportedName;
    state.implicitVoid =
        ts_node_is_null(returnType) && NodeSymbol(node) != parser::GetGrammarSymbols().symLambdaExpression;
}

} // namespace angel_lsp::analysis
