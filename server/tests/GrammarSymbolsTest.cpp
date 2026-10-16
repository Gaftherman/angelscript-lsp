#include <doctest/doctest.h>

#include "parser/GrammarSymbols.h"

namespace
{
using namespace angel_lsp::parser;
}

TEST_CASE("GrammarSymbols - all cached named symbols, anonymous tokens, and field IDs resolve to non-zero")
{
    const auto& gs = GetGrammarSymbols();

    // Named symbols
    CHECK(gs.symScript != 0);
    CHECK(gs.symClassDeclaration != 0);
    CHECK(gs.symClassBody != 0);
    CHECK(gs.symInterfaceDeclaration != 0);
    CHECK(gs.symInterfaceBody != 0);
    CHECK(gs.symNamespaceDeclaration != 0);
    CHECK(gs.symNamespaceBody != 0);
    CHECK(gs.symMixinDeclaration != 0);
    CHECK(gs.symEnumDeclaration != 0);
    CHECK(gs.symEnumMember != 0);
    CHECK(gs.symBaseClassList != 0);
    CHECK(gs.symScope != 0);

    CHECK(gs.symFuncDeclaration != 0);
    CHECK(gs.symFuncdefDeclaration != 0);
    CHECK(gs.symInterfaceMethod != 0);
    CHECK(gs.symVariableDeclaration != 0);
    CHECK(gs.symVariableDeclarator != 0);
    CHECK(gs.symVirtualProperty != 0);
    CHECK(gs.symAccessor != 0);
    CHECK(gs.symFuncAttributes != 0);
    CHECK(gs.symDeclarationModifier != 0);
    CHECK(gs.symSharedExternalModifier != 0);
    CHECK(gs.symUsingDeclaration != 0);
    CHECK(gs.symImportDeclaration != 0);
    CHECK(gs.symTypedefDeclaration != 0);
    CHECK(gs.symParameter != 0);
    CHECK(gs.symParameterList != 0);
    CHECK(gs.symForeachVariable != 0);
    CHECK(gs.symLambdaParameterList != 0);

    CHECK(gs.symIdentifier != 0);
    CHECK(gs.symScopedIdentifier != 0);
    CHECK(gs.symType != 0);
    CHECK(gs.symDatatype != 0);
    CHECK(gs.symPrimitiveType != 0);
    CHECK(gs.symTemplateTypeList != 0);
    CHECK(gs.symTemplateParameterList != 0);

    CHECK(gs.symMemberExpression != 0);
    CHECK(gs.symMemberAccess != 0);
    CHECK(gs.symCallExpression != 0);
    CHECK(gs.symArgumentList != 0);
    CHECK(gs.symCastExpression != 0);
    CHECK(gs.symFunctionalCastExpression != 0);
    CHECK(gs.symConstructCallExpression != 0);
    CHECK(gs.symBinaryExpression != 0);
    CHECK(gs.symAssignmentExpression != 0);
    CHECK(gs.symUnaryExpression != 0);
    CHECK(gs.symPostfixExpression != 0);
    CHECK(gs.symParenthesizedExpression != 0);
    CHECK(gs.symTernaryExpression != 0);
    CHECK(gs.symIndexExpression != 0);
    CHECK(gs.symLambdaExpression != 0);
    CHECK(gs.symInitializerList != 0);
    CHECK(gs.symTypedInitializerList != 0);

    CHECK(gs.symNullLiteral != 0);
    CHECK(gs.symBooleanLiteral != 0);
    CHECK(gs.symNumberLiteral != 0);
    CHECK(gs.symStringLiteral != 0);
    CHECK(gs.symConcatenatedString != 0);
    CHECK(gs.symMetadata != 0);
    CHECK(gs.symComment != 0);
    CHECK(gs.symPreprocDirective != 0);

    CHECK(gs.symStatementBlock != 0);
    CHECK(gs.symReturnStatement != 0);
    CHECK(gs.symIfStatement != 0);
    CHECK(gs.symForStatement != 0);
    CHECK(gs.symForeachStatement != 0);
    CHECK(gs.symWhileStatement != 0);
    CHECK(gs.symDoWhileStatement != 0);
    CHECK(gs.symSwitchStatement != 0);
    CHECK(gs.symCaseClause != 0);
    CHECK(gs.symTryStatement != 0);

    // Anonymous tokens
    CHECK(gs.tokConst != 0);
    CHECK(gs.tokIn != 0);
    CHECK(gs.tokOut != 0);
    CHECK(gs.tokInout != 0);
    CHECK(gs.tokAmp != 0);
    CHECK(gs.tokAt != 0);
    CHECK(gs.tokPrivate != 0);
    CHECK(gs.tokProtected != 0);
    CHECK(gs.tokShared != 0);
    CHECK(gs.tokMixin != 0);
    CHECK(gs.tokAbstract != 0);
    CHECK(gs.tokFinal != 0);
    CHECK(gs.tokOverride != 0);
    CHECK(gs.tokExplicit != 0);
    CHECK(gs.tokProperty != 0);
    CHECK(gs.tokDelete != 0);
    CHECK(gs.tokExternal != 0);
    CHECK(gs.tokImport != 0);
    CHECK(gs.tokOpenBrace != 0);
    CHECK(gs.tokGet != 0);
    CHECK(gs.tokSet != 0);
    CHECK(gs.tokColonColon != 0);

    // Field IDs
    CHECK(gs.fieldName != 0);
    CHECK(gs.fieldType != 0);
    CHECK(gs.fieldReturnType != 0);
    CHECK(gs.fieldParameters != 0);
    CHECK(gs.fieldBody != 0);
    CHECK(gs.fieldFunction != 0);
    CHECK(gs.fieldArguments != 0);
    CHECK(gs.fieldObject != 0);
    CHECK(gs.fieldMember != 0);
    CHECK(gs.fieldLeft != 0);
    CHECK(gs.fieldRight != 0);
    CHECK(gs.fieldOperator != 0);
    CHECK(gs.fieldCondition != 0);
    CHECK(gs.fieldValue != 0);
    CHECK(gs.fieldIndex != 0);
    CHECK(gs.fieldArgName != 0);
    CHECK(gs.fieldHandler != 0);
    CHECK(gs.fieldVarType != 0);
    CHECK(gs.fieldKind != 0);
    CHECK(gs.fieldSource != 0);
    CHECK(gs.fieldPropType != 0);
    CHECK(gs.fieldParamType != 0);
    CHECK(gs.fieldDefaultValue != 0);
    CHECK(gs.fieldBaseType != 0);
    CHECK(gs.fieldTemplateParams != 0);
}
