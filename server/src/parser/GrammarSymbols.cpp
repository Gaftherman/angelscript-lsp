#include "parser/GrammarSymbols.h"
#include "parser/GrammarNames.h"

#include <string_view>

extern "C" const TSLanguage* tree_sitter_angelscript();

namespace angel_lsp::parser
{
namespace
{

inline TSSymbol SymNamed(const TSLanguage* lang, std::string_view name) noexcept
{
    return ts_language_symbol_for_name(lang, name.data(), static_cast<uint32_t>(name.size()), true);
}

inline TSSymbol SymAnon(const TSLanguage* lang, std::string_view name) noexcept
{
    return ts_language_symbol_for_name(lang, name.data(), static_cast<uint32_t>(name.size()), false);
}

inline TSFieldId FieldId(const TSLanguage* lang, std::string_view name) noexcept
{
    return ts_language_field_id_for_name(lang, name.data(), static_cast<uint32_t>(name.size()));
}

void PopulateDeclarations(const TSLanguage* lang, GrammarSymbols& gs) noexcept
{
    gs.symScript = SymNamed(lang, parser::nodes::Script);
    gs.symClassDeclaration = SymNamed(lang, parser::nodes::ClassDeclaration);
    gs.symClassBody = SymNamed(lang, parser::nodes::ClassBody);
    gs.symInterfaceDeclaration = SymNamed(lang, parser::nodes::InterfaceDeclaration);
    gs.symInterfaceBody = SymNamed(lang, parser::nodes::InterfaceBody);
    gs.symNamespaceDeclaration = SymNamed(lang, parser::nodes::NamespaceDeclaration);
    gs.symNamespaceBody = SymNamed(lang, parser::nodes::NamespaceBody);
    gs.symMixinDeclaration = SymNamed(lang, parser::nodes::MixinDeclaration);
    gs.symEnumDeclaration = SymNamed(lang, parser::nodes::EnumDeclaration);
    gs.symEnumMember = SymNamed(lang, parser::nodes::EnumMember);
    gs.symBaseClassList = SymNamed(lang, parser::nodes::BaseClassList);
    gs.symScope = SymNamed(lang, parser::nodes::Scope);

    gs.symFuncDeclaration = SymNamed(lang, parser::nodes::FuncDeclaration);
    gs.symFuncdefDeclaration = SymNamed(lang, parser::nodes::FuncdefDeclaration);
    gs.symInterfaceMethod = SymNamed(lang, parser::nodes::InterfaceMethod);
    gs.symVariableDeclaration = SymNamed(lang, parser::nodes::VariableDeclaration);
    gs.symVariableDeclarator = SymNamed(lang, parser::nodes::VariableDeclarator);
    gs.symVirtualProperty = SymNamed(lang, parser::nodes::VirtualProperty);
    gs.symAccessor = SymNamed(lang, parser::nodes::Accessor);
    gs.symFuncAttributes = SymNamed(lang, parser::nodes::FuncAttributes);
    gs.symDeclarationModifier = SymNamed(lang, parser::nodes::DeclarationModifier);
    gs.symSharedExternalModifier = SymNamed(lang, parser::nodes::SharedExternalModifier);
    gs.symUsingDeclaration = SymNamed(lang, parser::nodes::UsingDeclaration);
    gs.symImportDeclaration = SymNamed(lang, parser::nodes::ImportDeclaration);
    gs.symTypedefDeclaration = SymNamed(lang, parser::nodes::TypedefDeclaration);
    gs.symParameter = SymNamed(lang, parser::nodes::Parameter);
    gs.symParameterList = SymNamed(lang, parser::nodes::ParameterList);
    gs.symForeachVariable = SymNamed(lang, parser::nodes::ForeachVariable);
    gs.symLambdaParameterList = SymNamed(lang, parser::nodes::LambdaParameterList);
}

void PopulateExpressionsAndLiterals(const TSLanguage* lang, GrammarSymbols& gs) noexcept
{
    gs.symIdentifier = SymNamed(lang, parser::nodes::Identifier);
    gs.symScopedIdentifier = SymNamed(lang, parser::nodes::ScopedIdentifier);
    gs.symType = SymNamed(lang, parser::nodes::Type);
    gs.symDatatype = SymNamed(lang, parser::nodes::Datatype);
    gs.symPrimitiveType = SymNamed(lang, parser::nodes::PrimitiveType);
    gs.symNestedTypeName = SymNamed(lang, parser::nodes::NestedTypeName);
    gs.symTemplateTypeList = SymNamed(lang, parser::nodes::TemplateTypeList);
    gs.symTemplateParameterList = SymNamed(lang, parser::nodes::TemplateParameterList);

    gs.symMemberExpression = SymNamed(lang, parser::nodes::MemberExpression);
    gs.symMemberAccess = gs.symMemberExpression;
    gs.symCallExpression = SymNamed(lang, parser::nodes::CallExpression);
    gs.symArgumentList = SymNamed(lang, parser::nodes::ArgumentList);
    gs.symCastExpression = SymNamed(lang, parser::nodes::CastExpression);
    gs.symFunctionalCastExpression = SymNamed(lang, parser::nodes::FunctionalCastExpression);
    gs.symConstructCallExpression = SymNamed(lang, parser::nodes::ConstructCallExpression);
    gs.symBinaryExpression = SymNamed(lang, parser::nodes::BinaryExpression);
    gs.symAssignmentExpression = SymNamed(lang, parser::nodes::AssignmentExpression);
    gs.symUnaryExpression = SymNamed(lang, parser::nodes::UnaryExpression);
    gs.symPostfixExpression = SymNamed(lang, parser::nodes::PostfixExpression);
    gs.symParenthesizedExpression = SymNamed(lang, parser::nodes::ParenthesizedExpression);
    gs.symTernaryExpression = SymNamed(lang, parser::nodes::TernaryExpression);
    gs.symIndexExpression = SymNamed(lang, parser::nodes::IndexExpression);
    gs.symLambdaExpression = SymNamed(lang, parser::nodes::LambdaExpression);
    gs.symThisExpression = SymNamed(lang, parser::nodes::ThisExpression);
    gs.symInitializerList = SymNamed(lang, parser::nodes::InitializerList);
    gs.symTypedInitializerList = SymNamed(lang, parser::nodes::TypedInitializerList);

    gs.symNullLiteral = SymNamed(lang, parser::nodes::NullLiteral);
    gs.symBooleanLiteral = SymNamed(lang, parser::nodes::BooleanLiteral);
    gs.symNumberLiteral = SymNamed(lang, parser::nodes::NumberLiteral);
    gs.symStringLiteral = SymNamed(lang, parser::nodes::StringLiteral);
    gs.symConcatenatedString = SymNamed(lang, parser::nodes::ConcatenatedString);
    gs.symMetadata = SymNamed(lang, parser::nodes::Metadata);
    gs.symComment = SymNamed(lang, parser::nodes::Comment);
    gs.symPreprocDirective = SymNamed(lang, parser::nodes::PreprocDirective);

    gs.symStatementBlock = SymNamed(lang, parser::nodes::StatementBlock);
    gs.symReturnStatement = SymNamed(lang, parser::nodes::ReturnStatement);
    gs.symBreakStatement = SymNamed(lang, parser::nodes::BreakStatement);
    gs.symContinueStatement = SymNamed(lang, parser::nodes::ContinueStatement);
    gs.symIfStatement = SymNamed(lang, parser::nodes::IfStatement);
    gs.symForStatement = SymNamed(lang, parser::nodes::ForStatement);
    gs.symForeachStatement = SymNamed(lang, parser::nodes::ForeachStatement);
    gs.symWhileStatement = SymNamed(lang, parser::nodes::WhileStatement);
    gs.symDoWhileStatement = SymNamed(lang, parser::nodes::DoWhileStatement);
    gs.symSwitchStatement = SymNamed(lang, parser::nodes::SwitchStatement);
    gs.symCaseClause = SymNamed(lang, parser::nodes::CaseClause);
    gs.symTryStatement = SymNamed(lang, parser::nodes::TryStatement);
    gs.symExpressionStatement = SymNamed(lang, parser::nodes::ExpressionStatement);
}

void PopulateTokens(const TSLanguage* lang, GrammarSymbols& gs) noexcept
{
    gs.tokConst = SymAnon(lang, "const");
    gs.tokIn = SymAnon(lang, "in");
    gs.tokOut = SymAnon(lang, "out");
    gs.tokInout = SymAnon(lang, "inout");
    gs.tokAmp = SymAnon(lang, "&");
    gs.tokAt = SymAnon(lang, "@");
    gs.tokPrivate = SymAnon(lang, "private");
    gs.tokProtected = SymAnon(lang, "protected");
    gs.tokShared = SymAnon(lang, "shared");
    gs.tokMixin = SymAnon(lang, "mixin");
    gs.tokAbstract = SymAnon(lang, "abstract");
    gs.tokFinal = SymAnon(lang, "final");
    gs.tokOverride = SymAnon(lang, "override");
    gs.tokExplicit = SymAnon(lang, "explicit");
    gs.tokProperty = SymAnon(lang, "property");
    gs.tokDelete = SymAnon(lang, "delete");
    gs.tokExternal = SymAnon(lang, "external");
    gs.tokImport = SymAnon(lang, "import");
    gs.tokOpenBrace = SymAnon(lang, "{");
    gs.tokCloseBrace = SymAnon(lang, "}");
    gs.tokGet = SymAnon(lang, "get");
    gs.tokSet = SymAnon(lang, "set");
    gs.tokColonColon = SymAnon(lang, "::");
    gs.tokClass = SymAnon(lang, "class");
    gs.tokInterface = SymAnon(lang, "interface");
    gs.tokFunction = SymAnon(lang, "function");
    gs.tokComma = SymAnon(lang, ",");
    gs.tokOpenParen = SymAnon(lang, "(");
    gs.tokCloseParen = SymAnon(lang, ")");
    gs.tokColon = SymAnon(lang, ":");
    gs.tokOpenBracket = SymAnon(lang, "[");
    gs.tokCloseBracket = SymAnon(lang, "]");
    gs.tokAssign = SymAnon(lang, "=");
    gs.tokSemicolon = SymAnon(lang, ";");
    gs.tokDefault = SymAnon(lang, "default");
    gs.tokCase = SymAnon(lang, "case");
    gs.tokDot = SymAnon(lang, ".");
    gs.tokSlash = SymAnon(lang, "/");
    gs.tokTilde = SymAnon(lang, "~");
}

void PopulateFields(const TSLanguage* lang, GrammarSymbols& gs) noexcept
{
    gs.fieldName = FieldId(lang, parser::fields::Name);
    gs.fieldType = FieldId(lang, parser::fields::Type);
    gs.fieldReturnType = FieldId(lang, parser::fields::ReturnType);
    gs.fieldParameters = FieldId(lang, parser::fields::Parameters);
    gs.fieldBody = FieldId(lang, parser::fields::Body);
    gs.fieldFunction = FieldId(lang, parser::fields::Function);
    gs.fieldArguments = FieldId(lang, parser::fields::Arguments);
    gs.fieldObject = FieldId(lang, parser::fields::Object);
    gs.fieldMember = FieldId(lang, parser::fields::Member);
    gs.fieldLeft = FieldId(lang, parser::fields::Left);
    gs.fieldRight = FieldId(lang, parser::fields::Right);
    gs.fieldOperator = FieldId(lang, parser::fields::Operator);
    gs.fieldCondition = FieldId(lang, parser::fields::Condition);
    gs.fieldValue = FieldId(lang, parser::fields::Value);
    gs.fieldIndex = FieldId(lang, parser::fields::Index);
    gs.fieldIndexName = FieldId(lang, parser::fields::IndexName);
    gs.fieldArgName = FieldId(lang, parser::fields::ArgName);
    gs.fieldHandler = FieldId(lang, parser::fields::Handler);
    gs.fieldVarType = FieldId(lang, parser::fields::VarType);
    gs.fieldKind = FieldId(lang, parser::fields::Kind);
    gs.fieldSource = FieldId(lang, parser::fields::Source);
    gs.fieldPropType = FieldId(lang, parser::fields::PropType);
    gs.fieldParamType = FieldId(lang, parser::fields::ParamType);
    gs.fieldDefaultValue = FieldId(lang, parser::fields::DefaultValue);
    gs.fieldBaseType = FieldId(lang, parser::fields::BaseType);
    gs.fieldTemplateParams = FieldId(lang, parser::fields::TemplateParams);
    gs.fieldUpdate = FieldId(lang, parser::fields::Update);
    gs.fieldInit = FieldId(lang, parser::fields::Init);
    gs.fieldConsequence = FieldId(lang, parser::fields::Consequence);
    gs.fieldOperand = FieldId(lang, parser::fields::Operand);
    gs.fieldAlternative = FieldId(lang, parser::fields::Alternative);
    gs.fieldBase = FieldId(lang, parser::fields::Base);
    gs.fieldCollection = FieldId(lang, parser::fields::Collection);
    gs.fieldModifier = FieldId(lang, parser::fields::Modifier);
    gs.fieldParam = FieldId(lang, parser::fields::Param);
    gs.fieldUnderlyingType = FieldId(lang, parser::fields::UnderlyingType);
}

} // namespace

const GrammarSymbols& GetGrammarSymbols() noexcept
{
    static const GrammarSymbols s_symbols = []()
    {
        const TSLanguage* lang = tree_sitter_angelscript();
        GrammarSymbols gs;
        PopulateDeclarations(lang, gs);
        PopulateExpressionsAndLiterals(lang, gs);
        PopulateTokens(lang, gs);
        PopulateFields(lang, gs);
        gs.conversionTargetSymbols = {
            gs.symIfStatement,
            gs.symWhileStatement,
            gs.symDoWhileStatement,
            gs.symForStatement,
            gs.symTernaryExpression,
            gs.symExpressionStatement,
            gs.symForeachStatement,
            gs.symVariableDeclaration,
            gs.symReturnStatement,
            gs.symFuncDeclaration,
            gs.symAssignmentExpression,
            gs.symBinaryExpression,
            gs.symUnaryExpression,
            gs.symPostfixExpression,
            gs.symMemberExpression,
            gs.symIndexExpression,
            gs.symCastExpression,
            gs.symCallExpression,
            gs.symConstructCallExpression,
        };
        return gs;
    }();

    return s_symbols;
}

} // namespace angel_lsp::parser
