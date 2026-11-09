#include "analysis/checkers/ControlFlowChecker.h"

#include "analysis/checkers/TypeConversionChecker.h"
#include "analysis/diagnostics/DiagnosticCodes.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/rules/RuleIndex.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarNames.h"
#include "parser/Primitives.h"
#include "utils/ScopedNameUtils.h"
#include "utils/Utils.h"

#include <ankerl/unordered_dense.h>
#include <array>
#include <charconv>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace angel_lsp::analysis
{
static TSSymbol NodeSymbol(TSNode node)
{
    return ts_node_is_null(node) ? 0 : ts_node_symbol(node);
}

/**
 * @brief `if (c);` and `else;` - a bare semicolon where the branch body belongs.
 *        AngelScript is stricter here than C++ and stricter than its own loops, which is the whole
 */
static void CheckEmptyBranch(TSNode node, DiagnosticContext& ctx)
{
    if (ts_node_has_error(node))
    {
        return;
    }

    const auto& gs = parser::GetGrammarSymbols();
    const TSNode consequence = parser::GetChildByField(node, gs.fieldConsequence);
    if (!ts_node_is_null(consequence) && ts_node_symbol(consequence) == gs.tokSemicolon)
    {
        const TSPoint start = ts_node_start_point(consequence);
        const TSPoint end = ts_node_end_point(consequence);
        ctx.EmitAtRange({start.row, start.column, end.row, end.column}, "as-err-if-empty-statement");
    }

    const TSNode alternative = parser::GetChildByField(node, gs.fieldAlternative);
    if (!ts_node_is_null(alternative) && ts_node_symbol(alternative) == gs.tokSemicolon)
    {
        const TSPoint start = ts_node_start_point(alternative);
        const TSPoint end = ts_node_end_point(alternative);
        ctx.EmitAtRange({start.row, start.column, end.row, end.column}, "as-err-else-empty-statement");
    }
}

// Definite return

/**
 * @brief True for a return type that null cannot convert to.
 *        The primitives, by name. Deliberately not "anything that is not a handle": a value type
 */
static bool IsNonNullablePrimitiveName(std::string_view typeName)
{
    return parser::primitives::IsNonNullable(typeName);
}

// Switch clauses

/**
 * @brief True for a case value shape AngelScript cannot use as a label.
 */
static bool IsUnusableCaseValue(TSNode expression, std::string_view sourceCode)
{
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol sym = ts_node_symbol(expression);

    if (sym == gs.symStringLiteral)
    {
        return true;
    }

    if (sym == gs.symNumberLiteral)
    {
        // A case label has to be an integral constant, so a fractional literal is out.
        // Identifiers are left alone: an enum member and a `const int` both arrive as one
        // and both are legal.
        const std::string_view text = parser::NodeText(expression, sourceCode);
        return text.find('.') != std::string_view::npos;
    }

    return false;
}

static std::optional<long long> ParseDecimalInteger(std::string_view text)
{
    if (text.starts_with('+'))
    {
        text.remove_prefix(1);
        if (text.starts_with('-'))
        {
            return std::nullopt;
        }
    }
    if (text.empty())
    {
        return std::nullopt;
    }

    long long result = 0;
    const auto end = text.data() + text.size();
    const auto parsed = std::from_chars(text.data(), end, result);
    if (parsed.ec != std::errc{} || parsed.ptr != end)
    {
        return std::nullopt;
    }
    return result;
}

static std::optional<long long> ExtractMemberValue(const EnumSignature& enumSig, std::string_view label, bool& matched)
{
    for (const auto& member : enumSig.members)
    {
        if (member.name == label)
        {
            matched = true;
            // No value written means the compiler counts it up from the previous one.
            // Following that count is possible and is not done here: the value then
            // depends on every member before it, and a wrong number would mean claiming
            // a duplicate that is not one.
            if (!member.value.empty())
            {
                return ParseDecimalInteger(member.value);
            }
            break;
        }
    }
    return std::nullopt;
}

static std::pair<std::string_view, std::string_view> SplitEnumeratorQualifier(std::string_view label)
{
    const auto [qualifier, leaf] = utils::SplitScopedName(label);
    const std::string_view enumName = utils::LastScopeSegment(qualifier);
    return {enumName, leaf};
}

/**
 * @brief The number a case label stands for, when that can be known for certain.
 *        `enum E { A = 1, B = 1 }` gives two names to one number, and a switch dispatches on the
 * @brief Resolve an enumerator's integer value from the cached RuleIndex.
 *        Switch-case duplicate detection needs this so that two cases written with different
 */
static std::optional<long long> EnumeratorValue(std::string_view label, const rules::RuleIndex& ruleIndex)
{
    auto [enumName, memberLabel] = SplitEnumeratorQualifier(label);
    if (memberLabel.empty())
    {
        return std::nullopt;
    }

    const auto it = ruleIndex.enumSymbolsByMemberName.find(memberLabel);
    if (it == ruleIndex.enumSymbolsByMemberName.end())
    {
        return std::nullopt;
    }

    std::optional<long long> found;
    size_t declaringEnums = 0;

    for (const auto& sym : it->second)
    {
        if (sym.type != SymbolType::Enum || !std::holds_alternative<EnumSignature>(sym.signature))
        {
            continue;
        }

        // Written with a qualifier: only the enum it names may answer.
        if (!enumName.empty() && sym.name != enumName)
        {
            continue;
        }

        bool matched = false;
        auto val = ExtractMemberValue(sym.GetEnum(), memberLabel, matched);
        if (matched)
        {
            ++declaringEnums;
            if (val)
            {
                found = val;
            }
        }
    }

    // Written without a qualifier and declared by more than one enum: which one this label
    // means is the compiler's business and not knowable from here.
    if (declaringEnums != 1)
    {
        return std::nullopt;
    }

    return found;
}

static bool CheckLocalCaseConstantness(TSNode value, std::string_view idText, DiagnosticContext& ctx)
{
    if (!ctx.request.scopeRoot)
    {
        return false;
    }

    const TSPoint at = ts_node_start_point(value);
    const Scope* scope = FindInnermostScope(ctx.request.scopeRoot.get(), at.row, at.column);
    if (!scope)
    {
        return false;
    }

    const Scope* owner = nullptr;
    const LocalDefinition* def = ResolveInScopeAt(scope, idText, ts_node_start_byte(value), &owner);

    bool insideFunction = false;
    for (const Scope* current = owner; current != nullptr; current = current->parent)
    {
        if (current->isFunctionScope)
        {
            insideFunction = true;
            break;
        }
    }

    if (def && insideFunction && def->kind == LocalDefinitionKind::Variable)
    {
        ctx.EmitAtNodeRange(ToOptionalSourceRange(value), "as-err-case-not-constant");
        return true;
    }

    return false;
}

static void CheckCaseConstantness(TSNode value, std::string_view sourceCode, DiagnosticContext& ctx)
{
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol sym = ts_node_symbol(value);
    if (sym == gs.symIdentifier || sym == gs.symScopedIdentifier)
    {
        const std::string_view idText = utils::TrimView(parser::NodeText(value, sourceCode));

        bool reported = false;
        auto syms =
            FindSymbolsInScope(std::string(idText), value, sourceCode, ctx.request.symbolTable, ctx.request.nodeIndex);
        for (const auto& s : syms)
        {
            if (s.type == SymbolType::Variable && !s.GetVariable().modifiers.isConst)
            {
                ctx.EmitAtNodeRange(ToOptionalSourceRange(value), "as-err-case-not-constant");
                reported = true;
                break;
            }
        }

        if (!reported)
        {
            CheckLocalCaseConstantness(value, idText, ctx);
        }
    }
    else if (sym == gs.symCallExpression)
    {
        ctx.EmitAtNodeRange(ToOptionalSourceRange(value), "as-err-case-not-constant");
    }
}

static std::string ComputeCaseKey(std::string_view text, const rules::RuleIndex& ruleIndex)
{
    if (const auto resolved = EnumeratorValue(text, ruleIndex))
    {
        return "#" + std::to_string(*resolved);
    }
    if (const auto parsed = ParseDecimalInteger(text))
    {
        return "#" + std::to_string(*parsed);
    }
    return std::string(text);
}

static void CheckSwitchLabels(std::span<const TSNode> labels, std::string_view sourceCode, DiagnosticContext& ctx)
{
    ankerl::unordered_dense::set<std::string> seenValues;
    for (const TSNode value : labels)
    {
        if (IsUnusableCaseValue(value, sourceCode))
        {
            ctx.EmitAtNodeRange(ToOptionalSourceRange(value), "as-err-invalid-case-type");
        }

        CheckCaseConstantness(value, sourceCode, ctx);
        const auto text = utils::TrimView(parser::NodeText(value, sourceCode));
        if (!text.empty() && !seenValues.insert(ComputeCaseKey(text, ctx.request.GetRuleIndex())).second)
        {
            ctx.EmitAtNodeRange(ToOptionalSourceRange(value), "as-err-duplicate-case-value", text);
        }
    }
}

static void ReportFlowFindings(std::span<const IndexedFlowFinding> findings, DiagnosticContext& ctx)
{
    for (const auto& finding : findings)
    {
        switch (finding.kind)
        {
        case IndexedFlowFindingKind::Unreachable:
            ctx.EmitAtRange(finding.range, "as-warn-unreachable-code", DiagnosticSeverity::Warning);
            break;
        case IndexedFlowFindingKind::VariableInCase:
            ctx.EmitAtRange(finding.range, diagnostics::codes::VariableInSwitchCase);
            break;
        case IndexedFlowFindingKind::DefaultNotLast:
            ctx.EmitAtRange(finding.range, "as-err-default-must-be-last");
            break;
        }
    }
}

/**
 * @brief Structure holding state and parameters for ControlFlowVisitor.
 */
class ControlFlowVisitor
{
  public:
    ControlFlowVisitor(std::string_view sourceCode, DiagnosticContext& ctx, const NodeIndex& index)
        : m_sourceCode(sourceCode), m_ctx(ctx), m_index(index)
    {
    }

    void Run()
    {
        const auto& grammar = parser::GetGrammarSymbols();
        ReportFlowFindings(m_index.FlowFindings(), m_ctx);
        const std::array nodes{grammar.symFuncDeclaration, grammar.symLambdaExpression, grammar.symIfStatement,
                               grammar.symSwitchStatement, grammar.symWhileStatement,   grammar.symDoWhileStatement,
                               grammar.symForStatement,    grammar.symForeachStatement, grammar.symReturnStatement,
                               grammar.symBreakStatement,  grammar.symContinueStatement};
        std::vector<Frame> frames;
        m_index.ForEachNodeOrdered(nodes, [&](TSNode node) { Visit(node, frames); });
    }

  private:
    std::string_view m_sourceCode;
    DiagnosticContext& m_ctx;
    const NodeIndex& m_index;

    /**
     * @brief Check return statement.
     * @param[in] node AST node under evaluation.
     * @param[in] state Parser or lexer scan state.
     */
    void CheckReturnStatement(TSNode node, const ControlFlowState& state)
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

    void CheckFunctionDeclaration(TSNode node, ControlFlowState& state)
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
            const bool hasSyntaxError = ts_node_has_error(body) ||
                                        (m_ctx.request.nodeIndex && m_ctx.request.nodeIndex->NodeHasSyntaxError(body));
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

    bool CheckLoopControl(TSNode node, TSSymbol sym, const ControlFlowState& state)
    {
        const auto& gs = parser::GetGrammarSymbols();
        if (sym == gs.symBreakStatement)
        {
            if (state.loopDepth == 0 && state.switchDepth == 0)
            {
                m_ctx.EmitAtNodeRange(ToOptionalSourceRange(node), "as-err-break-outside-loop");
            }
            return true;
        }
        if (sym == gs.symContinueStatement)
        {
            if (state.loopDepth == 0)
            {
                m_ctx.EmitAtNodeRange(ToOptionalSourceRange(node), "as-err-continue-outside-loop");
            }
            return true;
        }
        return false;
    }

    void UpdateBranchState(TSNode node, TSSymbol sym, ControlFlowState& state)
    {
        const auto& gs = parser::GetGrammarSymbols();
        if (sym == gs.symWhileStatement || sym == gs.symForStatement || sym == gs.symForeachStatement ||
            sym == gs.symDoWhileStatement)
        {
            ++state.loopDepth;
        }
        else if (sym == gs.symSwitchStatement)
        {
            CheckSwitchLabels(m_index.SwitchLabels(ts_node_start_byte(node)), m_sourceCode, m_ctx);
            ++state.switchDepth;
        }
        else if (sym == gs.symIfStatement)
        {
            CheckEmptyBranch(node, m_ctx);
        }
    }

    using Frame = std::pair<uint32_t, ControlFlowState>;

    void Visit(TSNode node, std::vector<Frame>& frames)
    {
        const uint32_t start = ts_node_start_byte(node);
        while (!frames.empty() && frames.back().first <= start)
        {
            frames.pop_back();
        }

        const ControlFlowState empty;
        const auto& enclosing = frames.empty() ? empty : frames.back().second;
        const auto& grammar = parser::GetGrammarSymbols();
        const TSSymbol symbol = ts_node_symbol(node);
        if (CheckLoopControl(node, symbol, enclosing))
        {
            return;
        }

        if (symbol == grammar.symReturnStatement)
        {
            CheckReturnStatement(node, enclosing);
        }
        else if (symbol == grammar.symFuncDeclaration || symbol == grammar.symLambdaExpression)
        {
            ControlFlowState state;
            CheckFunctionDeclaration(node, state);
            frames.emplace_back(ts_node_end_byte(node), std::move(state));
        }
        else
        {
            auto state = enclosing;
            UpdateBranchState(node, symbol, state);
            if (symbol != grammar.symIfStatement)
            {
                frames.emplace_back(ts_node_end_byte(node), std::move(state));
            }
        }
    }
};

void CheckControlFlow(const ControlFlowCheckRequest& request, DiagnosticContext& ctx)
{
    if (ts_node_is_null(request.root) || request.sourceCode.empty())
    {
        return;
    }

    if (ctx.request.nodeIndex)
    {
        ControlFlowVisitor(request.sourceCode, ctx, *ctx.request.nodeIndex).Run();
    }
    else
    {
        ControlFlowVisitor(request.sourceCode, ctx, NodeIndex(request.root)).Run();
    }
}
} // namespace angel_lsp::analysis
