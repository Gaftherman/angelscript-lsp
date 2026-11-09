#include "analysis/index/EngineDialectFacts.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/resolution/CodeActionResolution.h"
#include "analysis/resolution/ExpressionTypeResolver.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/rules/EngineRuleChecker.h"
#include "analysis/types/TypeSanitization.h"

#include <cstdint>
#include <string_view>

namespace angel_lsp::analysis::rules
{

/**
 * @brief Emits errors for foreach statements when the engine configuration disallows foreach syntax.
 */
static void CheckDialectForeach(const EngineDialectFacts& facts, DiagnosticContext& ctx)
{
    if (!ctx.request.SupportsForeach())
    {
        for (const auto& span : facts.foreachStatements)
        {
            ctx.EmitAtRange({span.startRow, span.startColumn, span.startRow, span.startColumn + 7},
                            "as-err-foreach-unsupported", DiagnosticSeverity::Error);
        }
    }
}

/**
 * @brief Emits errors for empty initializer-list slots when disallowed by engine configuration.
 */
static void CheckDialectEmptyListElements(const EngineDialectFacts& facts, DiagnosticContext& ctx)
{
    if (ctx.request.DisallowsEmptyListElements())
    {
        for (const auto& span : facts.emptyListElements)
        {
            ctx.EmitAtRange({span.startRow, span.startColumn, span.endRow, span.endColumn}, "as-err-empty-list-element",
                            DiagnosticSeverity::Error);
        }
    }
}

/**
 * @brief Emits errors when single-quoted character literals initialize non-character primitive variables.
 */
static void CheckDialectCharacterLiterals(const EngineDialectFacts& facts, DiagnosticContext& ctx)
{
    if (ctx.request.CharacterLiteralMode() != 0)
    {
        return;
    }
    for (const auto& decl : facts.charLiteralDeclarators)
    {
        const std::string_view typeText = ctx.request.sourceCode.substr(
            decl.declaredTypeRange.startByte, decl.declaredTypeRange.endByte - decl.declaredTypeRange.startByte);
        const std::string_view declared = CleanBaseTypeView(typeText);
        if (!IsPrimitiveTypeName(declared) || declared == "auto" || declared == "void")
        {
            continue;
        }
        ctx.EmitAtRange(
            {decl.valueSpan.startRow, decl.valueSpan.startColumn, decl.valueSpan.endRow, decl.valueSpan.endColumn},
            "as-err-character-literal-is-string", declared, DiagnosticSeverity::Error);
    }
}

/**
 * @brief Emits hints for integer-by-integer division operations when configured to report integer division.
 */
static void CheckDialectIntegerDivision(const EngineDialectFacts& facts, DiagnosticContext& ctx)
{
    if (ctx.request.diagnostics && ctx.request.diagnostics->reportIntegerDivision &&
        !ctx.request.DisablesIntegerDivision())
    {
        for (const auto& span : facts.integerDivisions)
        {
            ctx.EmitAtRange({span.startRow, span.startColumn, span.endRow, span.endColumn}, "as-hint-integer-division",
                            DiagnosticSeverity::Hint);
        }
    }
}

/**
 * @brief Validates named argument syntax modes against host engine dialect settings.
 */
static void CheckDialectNamedArguments(const EngineDialectFacts& facts, DiagnosticContext& ctx)
{
    if (ctx.request.NamedArgumentSyntaxMode() != 2)
    {
        const auto severity =
            ctx.request.NamedArgumentSyntaxMode() == 1 ? DiagnosticSeverity::Warning : DiagnosticSeverity::Error;
        for (const auto& arg : facts.namedArguments)
        {
            const std::string_view name =
                ctx.request.sourceCode.substr(arg.nameRange.startByte, arg.nameRange.endByte - arg.nameRange.startByte);
            ctx.EmitAtRange({arg.argumentSpan.startRow, arg.argumentSpan.startColumn, arg.argumentSpan.endRow,
                             arg.argumentSpan.endColumn},
                            "as-err-named-argument-syntax", name, severity);
        }
    }
}

/**
 * @brief Determines if a type name resolves to a class symbol in the symbol table.
 */
static bool IsSymbolTableClass(const SymbolTable& table, std::string_view typeName)
{
    if (const auto symbols = table.FindSymbolsPtr(typeName))
    {
        for (const auto& sym : *symbols)
        {
            if (sym.type == SymbolType::Class)
            {
                return true;
            }
        }
    }
    return false;
}

/**
 * @brief Flags value assignment expressions when ref class assignment is disallowed by the dialect.
 */
static void CheckDialectValueAssignForRef(const NodeIndex& nodeIndex, const EngineDialectFacts& facts,
                                          DiagnosticContext& ctx)
{
    if (!ctx.request.DisallowsValueAssignForRef())
    {
        return;
    }
    for (const auto& assign : facts.assignmentExpressions)
    {
        const Scope* scope = ctx.request.scopeRoot
                                 ? FindInnermostScope(ctx.request.scopeRoot.get(), assign.targetSpan.startRow,
                                                      assign.targetSpan.startColumn)
                                 : nullptr;
        ExpressionTypeContext exprCtx(scope, ctx);
        exprCtx.nodeIndex = &nodeIndex;
        const auto rawTargetType = ResolveExpressionTypeAt(assign.targetSpan.bytes, exprCtx);
        const std::string_view targetType = CleanBaseTypeView(rawTargetType);

        if (IsSymbolTableClass(ctx.request.symbolTable, targetType))
        {
            ctx.EmitAtRange({assign.expressionSpan.startRow, assign.expressionSpan.startColumn,
                             assign.expressionSpan.endRow, assign.expressionSpan.endColumn},
                            "as-err-value-assign-for-ref", targetType, DiagnosticSeverity::Error);
        }
    }
}

/**
 * @brief Flags non-heredoc multiline string literals when multiline strings are disallowed.
 */
static void CheckDialectMultilineStrings(const EngineDialectFacts& facts, DiagnosticContext& ctx)
{
    if (!ctx.request.AllowsMultilineStrings())
    {
        for (const auto& span : facts.multilineStrings)
        {
            ctx.EmitAtRange({span.startRow, span.startColumn, span.startRow, span.startColumn + 1},
                            "as-err-multiline-string", DiagnosticSeverity::Error);
        }
    }
}

void CheckEngineDialectRules(const NodeIndex& nodeIndex, DiagnosticContext& ctx)
{
    const EngineDialectFacts facts = BuildEngineDialectFacts(nodeIndex, ctx.request.sourceCode);
    CheckDialectForeach(facts, ctx);
    CheckDialectEmptyListElements(facts, ctx);
    CheckDialectCharacterLiterals(facts, ctx);
    CheckDialectIntegerDivision(facts, ctx);
    CheckDialectNamedArguments(facts, ctx);
    CheckDialectValueAssignForRef(nodeIndex, facts, ctx);
    CheckDialectMultilineStrings(facts, ctx);
}

} // namespace angel_lsp::analysis::rules
