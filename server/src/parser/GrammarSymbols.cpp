#include "parser/GrammarSymbols.h"
#include "parser/GrammarNames.h"

#include <string_view>

extern "C" const TSLanguage* tree_sitter_angelscript();

namespace angel_lsp::parser
{

const GrammarSymbols& GetGrammarSymbols() noexcept
{
    static const GrammarSymbols s_symbols = []()
    {
        const TSLanguage* lang = tree_sitter_angelscript();
        auto symFor = [lang](std::string_view name) -> TSSymbol
        { return ts_language_symbol_for_name(lang, name.data(), static_cast<uint32_t>(name.size()), true); };
        GrammarSymbols gs;
        gs.symTemplateTypeList = symFor(parser::nodes::TemplateTypeList);
        gs.symCastExpression = symFor(parser::nodes::CastExpression);
        gs.symTemplateParameterList = symFor(parser::nodes::TemplateParameterList);
        gs.symBinaryExpression = symFor(parser::nodes::BinaryExpression);
        gs.symAssignmentExpression = symFor(parser::nodes::AssignmentExpression);
        gs.symUnaryExpression = symFor(parser::nodes::UnaryExpression);
        gs.symPostfixExpression = symFor(parser::nodes::PostfixExpression);
        gs.symParameter = symFor(parser::nodes::Parameter);
        gs.symScopedIdentifier = symFor(parser::nodes::ScopedIdentifier);
        gs.symClassDeclaration = symFor(parser::nodes::ClassDeclaration);
        gs.symType = symFor(parser::nodes::Type);
        gs.symDatatype = symFor(parser::nodes::Datatype);
        gs.symScope = symFor(parser::nodes::Scope);
        gs.symMemberAccess = symFor(parser::nodes::MemberExpression);
        gs.symNullLiteral = symFor(parser::nodes::NullLiteral);
        gs.symParenthesizedExpression = symFor(parser::nodes::ParenthesizedExpression);
        gs.symVariableDeclarator = symFor(parser::nodes::VariableDeclarator);
        gs.symForeachVariable = symFor(parser::nodes::ForeachVariable);
        gs.symLambdaParameterList = symFor(parser::nodes::LambdaParameterList);
        return gs;
    }();
    return s_symbols;
}

} // namespace angel_lsp::parser
