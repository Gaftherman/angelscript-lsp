#include "analysis/ControlFlowChecker.h"
#include "analysis/ASTUtils.h"
#include "analysis/SemanticHelpers.h"
#include "analysis/TypeConversionChecker.h"
#include "analysis/rules/RuleIndex.h"

#include "parser/GrammarNames.h"
#include "parser/Primitives.h"
#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace angel_lsp::analysis
{
namespace
{
std::string_view NodeType(TSNode node)
{
    return ts_node_is_null(node) ? std::string_view{} : std::string_view(ts_node_type(node));
}

std::string_view NodeText(TSNode node, std::string_view sourceCode)
{
    if (ts_node_is_null(node))
    {
        return {};
    }
    const uint32_t start = ts_node_start_byte(node);
    const uint32_t end = ts_node_end_byte(node);
    if (start >= end || end > sourceCode.size())
    {
        return {};
    }
    return sourceCode.substr(start, end - start);
}

std::string_view Trim(std::string_view text)
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
    {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
    {
        text.remove_suffix(1);
    }
    return text;
}

/**
 * @brief The keyword a switch clause opens with: "case", "default", or empty if malformed.
 *
 * Read as a node type rather than by matching the clause's source text. The grammar makes
 * both keywords their own anonymous token, so the type is exact; the text is not. Anything
 * the parser steps over on the way to the keyword - a fall-through comment written just
 * before `default:`, say - shifts the text, and the old test then answered "case" for a
 * default clause. That silenced the default-must-be-last rule on exactly the switch
 * statements most likely to carry the bug.
 */
std::string_view ClauseKeyword(TSNode clause)
{
    return NodeType(ts_node_child(clause, 0));
}

/** @brief True when the clause is spelled `default:` rather than `case <expr>:`. */
bool IsDefaultClause(TSNode clause)
{
    return ClauseKeyword(clause) == "default";
}

/**
 * @brief Index of the clause's first statement among its named children.
 *
 * A `case` clause's label expression is a named child sitting where a statement would, so
 * counting children directly reads `case 2:` as a clause with one statement when it is
 * really an empty one falling through to the next.
 */
uint32_t FirstStatementIndex(TSNode clause)
{
    return IsDefaultClause(clause) ? 0u : 1u;
}

void EmitAtNode(TSNode node, DiagnosticContext& ctx, std::string_view code,
                DiagnosticSeverity severity = DiagnosticSeverity::Error)
{
    const TSPoint start = ts_node_start_point(node);
    const TSPoint end = ts_node_end_point(node);
    ctx.EmitAtRange({start.row, start.column, end.row, end.column}, code, severity);
}

void EmitAtNode(TSNode node, DiagnosticContext& ctx, std::string_view code, const std::string& arg)
{
    const TSPoint start = ts_node_start_point(node);
    const TSPoint end = ts_node_end_point(node);
    ctx.EmitAtRange({start.row, start.column, end.row, end.column}, code, arg);
}

/**
 * @brief `if (c);` and `else;` - a bare semicolon where the branch body belongs.
 *
 * AngelScript is stricter here than C++ and stricter than its own loops, which is the whole
 * reason this is worth a rule. Measured, six probes:
 *
 *     if (c);          ERROR   If with empty statement
 *     else;            ERROR   Else with empty statement
 *     if (c) {}        accepted - an empty *block* is fine
 *     while (c);       accepted
 *     for (;;);        accepted
 *     do; while (c);   accepted
 *
 * So it is `if` and `else` and nothing else. The mistake it catches is the one that started
 * this: `if (cond); return true; else return false;` reads as a three-branch decision and is
 * really an empty `if`, a `return` that always runs, and an `else` belonging to no `if`.
 *
 * Skipped inside a parse error, where a `;` in this position is as likely to be tree-sitter
 * recovering as it is to be what somebody typed.
 */
void CheckEmptyBranch(TSNode node, DiagnosticContext& ctx)
{
    if (ts_node_has_error(node))
    {
        return;
    }

    const TSNode consequence = parser::GetChildByField(node, parser::fields::Consequence);
    if (!ts_node_is_null(consequence) && std::string_view(ts_node_type(consequence)) == ";")
    {
        const TSPoint start = ts_node_start_point(consequence);
        const TSPoint end = ts_node_end_point(consequence);
        ctx.EmitAtRange({start.row, start.column, end.row, end.column}, "as-err-if-empty-statement");
    }

    const TSNode alternative = parser::GetChildByField(node, parser::fields::Alternative);
    if (!ts_node_is_null(alternative) && std::string_view(ts_node_type(alternative)) == ";")
    {
        const TSPoint start = ts_node_start_point(alternative);
        const TSPoint end = ts_node_end_point(alternative);
        ctx.EmitAtRange({start.row, start.column, end.row, end.column}, "as-err-else-empty-statement");
    }
}

// =====================================================================
// Definite return
// =====================================================================

/**
 * @brief True for a return type that null cannot convert to.
 *
 * The primitives, by name. Deliberately not "anything that is not a handle": a value type
 * the host registered may accept null through a conversion this analyzer never sees, and
 * inventing an error there costs more than missing one.
 */
bool IsNonNullablePrimitiveName(std::string_view typeName)
{
    return parser::primitives::IsNonNullable(typeName);
}

bool BlockOrClauseDefinitelyReturns(TSNode node, std::string_view type, std::string_view sourceCode);
bool IfDefinitelyReturns(TSNode node, std::string_view sourceCode);
bool SwitchDefinitelyReturns(TSNode node, std::string_view sourceCode);
bool TryDefinitelyReturns(TSNode node, std::string_view sourceCode);

/**
 * @brief True when control cannot fall off the end of this statement.
 *
 * Deliberately one-sided. Answering "yes" wrongly hides a real missing return, which costs
 * nothing; answering "no" wrongly reports a function that is perfectly fine, which is the
 * failure that matters. So every construct whose exit conditions this pass cannot settle -
 * a try block, a loop with a computed condition - answers "no" only where "no" is also the
 * honest reading, and the loops whose condition is literally true answer "yes" because they
 * have no normal exit at all.
 */
bool DefinitelyReturns(TSNode node, std::string_view sourceCode)
{
    const std::string_view type = NodeType(node);

    if (type == "return_statement")
    {
        return true;
    }

    if (type == "statement_block" || type == "case_clause")
    {
        return BlockOrClauseDefinitelyReturns(node, type, sourceCode);
    }

    if (type == "if_statement")
    {
        return IfDefinitelyReturns(node, sourceCode);
    }

    if (type == "switch_statement")
    {
        return SwitchDefinitelyReturns(node, sourceCode);
    }

    // No loop counts, whatever its condition says. This used to reason that `while (true)`
    // has no normal exit and so ends the function - true of the program, and not the rule
    // the compiler applies. Measured, all three:
    //
    //     int f() { while (true) { return 1; } }        Not all paths return a value
    //     int f() { for (;;) { return 1; } }            Not all paths return a value
    //     int f() { do { return 1; } while (true); }    Not all paths return a value
    //
    // Its analysis is structural: a loop body may run zero times as far as it is concerned,
    // so a return inside one is not a return on every path. Believing otherwise cost three
    // errors the compiler gives and this analyzer did not.
    if (type == "while_statement" || type == "do_while_statement" || type == "for_statement" ||
        type == "foreach_statement")
    {
        return false;
    }

    if (type == "try_statement")
    {
        return TryDefinitelyReturns(node, sourceCode);
    }

    return false;
}

bool BlockOrClauseDefinitelyReturns(TSNode node, std::string_view type, std::string_view sourceCode)
{
    const uint32_t first = type == "case_clause" ? FirstStatementIndex(node) : 0u;
    const uint32_t count = ts_node_named_child_count(node);
    for (uint32_t i = first; i < count; ++i)
    {
        if (DefinitelyReturns(ts_node_named_child(node, i), sourceCode))
        {
            return true;
        }
    }
    return false;
}

bool IfDefinitelyReturns(TSNode node, std::string_view sourceCode)
{
    TSNode alternative = parser::GetChildByField(node, parser::fields::Alternative);
    if (ts_node_is_null(alternative))
    {
        return false;
    }
    TSNode consequence = parser::GetChildByField(node, parser::fields::Consequence);
    return DefinitelyReturns(consequence, sourceCode) && DefinitelyReturns(alternative, sourceCode);
}

bool SwitchDefinitelyReturns(TSNode node, std::string_view sourceCode)
{
    bool hasDefault = false;
    bool allReturn = true;
    const uint32_t count = ts_node_named_child_count(node);
    for (uint32_t i = 0; i < count; ++i)
    {
        TSNode clause = ts_node_named_child(node, i);
        if (NodeType(clause) != "case_clause")
        {
            continue;
        }
        if (IsDefaultClause(clause))
        {
            hasDefault = true;
        }
        // An empty clause falls through to the next one, which is ordinary and says
        // nothing about whether the switch returns.
        const bool hasStatements = ts_node_named_child_count(clause) > FirstStatementIndex(clause);
        if (hasStatements && !DefinitelyReturns(clause, sourceCode))
        {
            allReturn = false;
        }
    }
    return hasDefault && allReturn;
}

bool TryDefinitelyReturns(TSNode node, std::string_view sourceCode)
{
    // Every block has to return, because either one of them can be the path taken: the
    // try block runs to its end, or an exception hands control to the catch block. The
    // real compiler answers `try { return 1; } catch { }` with "Not all paths return a
    // value" and accepts it once the catch returns too.
    //
    // The grammar gives `try` and `catch` a `statement_block` each and no fields, so the
    // named children are exactly the blocks to check.
    const uint32_t blockCount = ts_node_named_child_count(node);
    if (blockCount == 0)
    {
        return false;
    }
    for (uint32_t i = 0; i < blockCount; ++i)
    {
        if (!DefinitelyReturns(ts_node_named_child(node, i), sourceCode))
        {
            return false;
        }
    }
    return true;
}

// =====================================================================
// Switch clauses
// =====================================================================

/** @brief True for a case value shape AngelScript cannot use as a label. */
bool IsUnusableCaseValue(TSNode expression, std::string_view sourceCode)
{
    const std::string_view type = NodeType(expression);

    if (type == "string_literal")
    {
        return true;
    }

    if (type == "number_literal")
    {
        // A case label has to be an integral constant, so a fractional literal is out.
        // Identifiers are left alone: an enum member and a `const int` both arrive as one
        // and both are legal.
        const std::string_view text = NodeText(expression, sourceCode);
        return text.find('.') != std::string_view::npos;
    }

    return false;
}

std::optional<long long> ParseDecimalInteger(std::string_view text)
{
    if (text.empty())
    {
        return std::nullopt;
    }

    size_t index = 0;
    bool negative = false;
    if (text[index] == '-' || text[index] == '+')
    {
        negative = text[index] == '-';
        ++index;
    }
    if (index >= text.size())
    {
        return std::nullopt;
    }

    long long parsed = 0;
    for (; index < text.size(); ++index)
    {
        if (text[index] < '0' || text[index] > '9')
        {
            return std::nullopt;
        }
        parsed = parsed * 10 + (text[index] - '0');
    }

    return negative ? -parsed : parsed;
}

std::optional<long long> ExtractMemberValue(const EnumSignature& enumSig, std::string_view label, bool& matched)
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

std::pair<std::string_view, std::string_view> SplitEnumeratorQualifier(std::string_view label)
{
    std::string_view enumName;
    if (const size_t sep = label.rfind("::"); sep != std::string_view::npos)
    {
        enumName = label.substr(0, sep);
        label = label.substr(sep + 2);

        if (const size_t outer = enumName.rfind("::"); outer != std::string_view::npos)
        {
            enumName = enumName.substr(outer + 2);
        }
    }
    return {enumName, label};
}

/**
 * @brief The number a case label stands for, when that can be known for certain.
 *
 * `enum E { A = 1, B = 1 }` gives two names to one number, and a switch dispatches on the
 * number - so `case A:` and `case B:` are the same label twice and the compiler says so:
 * @brief Resolve an enumerator's integer value from the cached RuleIndex.
 *
 * Switch-case duplicate detection needs this so that two cases written with different
 * enumerator names that evaluate to the same number are caught, without falsely flagging
 * an enumerator and a literal, or two enumerators whose value we cannot resolve or that are
 * not one, on code that compiles.
 *
 * Accepts both `A` and `E::A`; the qualifier is dropped before the lookup.
 */
std::optional<long long> EnumeratorValue(std::string_view label, const rules::RuleIndex& ruleIndex)
{
    auto [enumName, memberLabel] = SplitEnumeratorQualifier(label);
    if (memberLabel.empty())
    {
        return std::nullopt;
    }

    const auto it = ruleIndex.enumSymbolsByMemberName.find(std::string(memberLabel));
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

bool CheckLocalCaseConstantness(TSNode value, const std::string& idText, DiagnosticContext& ctx)
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
    const LocalDefinition* def = ResolveInScope(scope, idText, &owner);

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
        EmitAtNode(value, ctx, "as-err-case-not-constant");
        return true;
    }

    return false;
}

void CheckCaseConstantness(TSNode value, std::string_view sourceCode, DiagnosticContext& ctx)
{
    const std::string_view nodeType = NodeType(value);
    if (nodeType == "identifier" || nodeType == "scoped_identifier")
    {
        std::string idText(Trim(NodeText(value, sourceCode)));

        bool reported = false;
        auto syms = FindSymbolsInScope(idText, value, sourceCode, ctx.request.symbolTable);
        for (const auto& s : syms)
        {
            if (s.type == SymbolType::Variable && !s.GetVariable().modifiers.isConst)
            {
                EmitAtNode(value, ctx, "as-err-case-not-constant");
                reported = true;
                break;
            }
        }

        if (!reported)
        {
            CheckLocalCaseConstantness(value, idText, ctx);
        }
    }
    else if (nodeType == "call_expression")
    {
        EmitAtNode(value, ctx, "as-err-case-not-constant");
    }
}

std::string ComputeCaseKey(std::string_view text, const rules::RuleIndex& ruleIndex)
{
    if (const auto resolved = EnumeratorValue(text, ruleIndex))
    {
        return "#" + std::to_string(*resolved);
    }
    if (text.find_first_not_of("-+0123456789") == std::string::npos)
    {
        return "#" + std::to_string(std::stoll(std::string(text)));
    }
    return std::string(text);
}

void ProcessCaseClause(TSNode clause, std::string_view sourceCode, std::vector<std::string>& seenValues,
                       DiagnosticContext& ctx)
{
    TSNode value = ts_node_named_child(clause, 0);
    if (ts_node_is_null(value) || ClauseKeyword(clause) != "case")
    {
        return;
    }

    if (IsUnusableCaseValue(value, sourceCode))
    {
        EmitAtNode(value, ctx, "as-err-invalid-case-type");
    }

    CheckCaseConstantness(value, sourceCode, ctx);

    std::string text(Trim(NodeText(value, sourceCode)));
    if (!text.empty())
    {
        std::string key = ComputeCaseKey(text, ctx.request.GetRuleIndex());
        if (std::find(seenValues.begin(), seenValues.end(), key) != seenValues.end())
        {
            EmitAtNode(value, ctx, "as-err-duplicate-case-value", text);
        }
        else
        {
            seenValues.push_back(std::move(key));
        }
    }
}

void CheckSwitch(TSNode node, std::string_view sourceCode, DiagnosticContext& ctx)
{
    std::vector<std::string> seenValues;
    TSNode defaultClause = {};
    bool haveDefault = false;
    bool defaultIsLast = true;

    const uint32_t count = ts_node_named_child_count(node);
    for (uint32_t i = 0; i < count; ++i)
    {
        TSNode clause = ts_node_named_child(node, i);
        if (NodeType(clause) != "case_clause")
        {
            continue;
        }

        if (IsDefaultClause(clause))
        {
            defaultClause = clause;
            haveDefault = true;
            defaultIsLast = true;
            continue;
        }

        if (haveDefault)
        {
            defaultIsLast = false;
        }

        ProcessCaseClause(clause, sourceCode, seenValues, ctx);
    }

    if (haveDefault && !defaultIsLast)
    {
        EmitAtNode(defaultClause, ctx, "as-err-default-must-be-last");
    }
}

// =====================================================================
// Traversal
// =====================================================================

struct FlowState
{
    uint32_t loopDepth = 0;
    uint32_t switchDepth = 0;

    /**
     * @brief What the enclosing function must return, and the name to say it under.
     *
     * Empty means there is nothing to check against - a constructor, or a lambda whose
     * funcdef this analyzer cannot see - and the rule stays silent, which is the policy
     * everywhere else the world is only partly visible.
     */
    std::string requiredReturn;
    std::string functionName;

    /**
     * @brief True for a function the grammar gives no return type: a constructor or a
     *        destructor.
     *
     * Both return void, and `return 42;` in one is "Can't return value when return type is
     * 'void'" - measured. as-err-void-return-value covers the same mistake in a function
     * that spells `void` out, and it reads the return type node, so it never fired here.
     */
    bool implicitVoid = false;
};

/**
 * @brief Reports the first statement a block can never reach.
 *
 * Purely structural, and that is the whole rule: anything written after a `return`, a
 * `break` or a `continue` *in the same block* is dead. Compiled against a real engine,
 * which warns - not errors - at exactly those three, and does not warn after an `if` whose
 * body returns, because the false branch still falls through. So no path analysis is
 * involved and none is wanted; DefinitelyReturns exists for the question that does need it.
 *
 * One report per block, at the first dead statement. The engine says it once too, and a
 * warning per line of a dead tail would bury the one that matters.
 */
void CheckUnreachable(TSNode block, DiagnosticContext& ctx)
{
    // A block the parser could not make sense of has a shape that is error recovery's
    // guess, not the author's, and reading a terminator out of it says nothing. Found by
    // the corpus audit on a file whose `if(...); return true; else return false;` is not
    // AngelScript at all.
    if (ts_node_has_error(block))
    {
        return;
    }

    const uint32_t count = ts_node_named_child_count(block);
    bool terminated = false;

    for (uint32_t i = 0; i < count; ++i)
    {
        TSNode child = ts_node_named_child(block, i);
        const std::string_view childType = NodeType(child);

        // Comments are extras rather than statements, and a comment after a return is
        // ordinary - it is usually what explains the return.
        if (childType == "comment")
        {
            continue;
        }

        // A conditional compilation directive is an opaque line to this grammar, so which
        // statements around it are live is not something this pass can know. The corpus
        // has `#if FALSE ... return value; #endif` followed by the real code, and every
        // statement after it read as dead. One directive anywhere in the block retires the
        // question for the whole block, because a `#endif` alone gives no clue what its
        // `#if` decided.
        if (childType == "preproc_directive")
        {
            return;
        }

        if (terminated)
        {
            EmitAtNode(child, ctx, "as-warn-unreachable-code", DiagnosticSeverity::Warning);
            return;
        }

        terminated =
            childType == "return_statement" || childType == "break_statement" || childType == "continue_statement";
    }
}

class ControlFlowVisitor
{
  public:
    ControlFlowVisitor(std::string_view sourceCode, DiagnosticContext& ctx) : m_sourceCode(sourceCode), m_ctx(ctx)
    {
    }

    void Run(TSNode root)
    {
        Visit(root, FlowState{});
    }

  private:
    std::string_view m_sourceCode;
    DiagnosticContext& m_ctx;

    void CheckReturnStatement(TSNode node, const FlowState& state)
    {
        // A bare `return;` in a function that owes a value. Measured: the compiler answers
        // "Must return a value" and rejects the file, and nothing here saw it -
        // as-err-not-all-paths-return asks only whether a return is *reached*, so
        // `float FS(float f) { return; }` passed every check this analyzer had.
        if (!state.requiredReturn.empty() && state.requiredReturn != "void" && ts_node_named_child_count(node) == 0)
        {
            EmitAtNode(node, m_ctx, "as-err-return-value-required", state.functionName);
        }

        // The constructor half of the same rule. Reported here rather than in
        // TypeConversionChecker because that one asks the return type node what is required,
        // and a constructor has none - so the one function in the language that can only
        // return void was the one nothing checked.
        if (state.implicitVoid && ts_node_named_child_count(node) > 0)
        {
            EmitAtNode(node, m_ctx, "as-err-void-return-value");
        }

        // `int f() { return null; }` - "No conversion from '<null handle>' to 'int' available."
        // as-err-null-non-handle says exactly this about a variable and had nothing to say
        // about a return. Restricted to the primitives, and to a return type carrying no `@`,
        // so a class or a handle - where null may well be legal, and where the host may have
        // registered a conversion this analyzer cannot see - is left alone.
        if (!state.requiredReturn.empty() && state.requiredReturn.find('@') == std::string::npos &&
            IsNonNullablePrimitiveName(state.requiredReturn) && ts_node_named_child_count(node) == 1 &&
            NodeType(ts_node_named_child(node, 0)) == node_types::NullLiteral)
        {
            EmitAtNode(node, m_ctx, "as-err-null-non-handle", state.requiredReturn);
        }
    }

    void CheckFunctionDeclaration(TSNode node, FlowState& state)
    {
        // A nested function opens its own flow: a loop enclosing the declaration does not
        // make a `break` inside the nested body legal.
        state = FlowState{};

        TSNode body = parser::GetChildByField(node, parser::fields::Body);
        TSNode returnType = parser::GetChildByField(node, parser::fields::ReturnType);
        TSNode name = parser::GetChildByField(node, parser::fields::Name);

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
            requiredReturn = Trim(NodeText(returnType, m_sourceCode));
            reportedName = std::string(NodeText(name, m_sourceCode));
        }
        else if (NodeType(node) == node_types::LambdaExpression)
        {
            if (const auto target = FuncdefTargetOfLambda(node, m_ctx.request.symbolTable, m_sourceCode))
            {
                requiredReturn = CleanBaseType(target->GetFuncdef().returnType);
                reportedName = target->name;
            }
        }

        if (!ts_node_is_null(body) && !requiredReturn.empty() && requiredReturn != "void" &&
            !DefinitelyReturns(body, m_sourceCode))
        {
            EmitAtNode(ts_node_is_null(name) ? node : name, m_ctx, "as-err-not-all-paths-return", reportedName);
        }

        // Carried into the body, so every `return` inside it knows what it owes. A lambda
        // whose funcdef target could not be resolved leaves this empty and the check below
        // does nothing, which is the same silence the rule above keeps.
        state.requiredReturn = requiredReturn;
        state.functionName = reportedName;
        state.implicitVoid = ts_node_is_null(returnType) && NodeType(node) != node_types::LambdaExpression;
    }

    bool CheckLoopControl(TSNode node, std::string_view type, const FlowState& state)
    {
        if (type == "break_statement")
        {
            if (state.loopDepth == 0 && state.switchDepth == 0)
            {
                EmitAtNode(node, m_ctx, "as-err-break-outside-loop");
            }
            return true;
        }
        if (type == "continue_statement")
        {
            if (state.loopDepth == 0)
            {
                EmitAtNode(node, m_ctx, "as-err-continue-outside-loop");
            }
            return true;
        }
        return false;
    }

    void UpdateBranchState(TSNode node, std::string_view type, FlowState& state)
    {
        if (type == "while_statement" || type == "for_statement" || type == "foreach_statement" ||
            type == "do_while_statement")
        {
            ++state.loopDepth;
        }
        else if (type == "switch_statement")
        {
            CheckSwitch(node, m_sourceCode, m_ctx);
            ++state.switchDepth;
        }
        else if (type == "if_statement")
        {
            CheckEmptyBranch(node, m_ctx);
        }
    }

    void Visit(TSNode node, FlowState state, int depth = 0)
    {
        // See k_maxAstDepth in ASTUtils.h.
        if (depth > k_maxAstDepth)
        {
            return;
        }

        const std::string_view type = NodeType(node);

        if (CheckLoopControl(node, type, state))
        {
            return;
        }

        if (type == "statement_block" || type == "case_clause")
        {
            CheckUnreachable(node, m_ctx);
        }
        else if (type == "return_statement")
        {
            CheckReturnStatement(node, state);
        }
        else if (type == "func_declaration" || type == "lambda_expression")
        {
            CheckFunctionDeclaration(node, state);
        }
        else
        {
            UpdateBranchState(node, type, state);
        }

        const uint32_t count = ts_node_named_child_count(node);
        for (uint32_t i = 0; i < count; ++i)
        {
            Visit(ts_node_named_child(node, i), state, depth + 1);
        }
    }
};
} // namespace

void CheckControlFlow(const ControlFlowCheckRequest& request, DiagnosticContext& ctx)
{
    if (ts_node_is_null(request.root) || request.sourceCode.empty())
    {
        return;
    }

    ControlFlowVisitor visitor(request.sourceCode, ctx);
    visitor.Run(request.root);
}
} // namespace angel_lsp::analysis
