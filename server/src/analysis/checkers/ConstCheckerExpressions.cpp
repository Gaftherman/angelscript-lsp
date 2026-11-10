#include "analysis/checkers/ConstCheckerCommon.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarNames.h"
#include "parser/Keywords.h"
#include "utils/ScopedNameUtils.h"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{

/**
 * @brief Checks if a declared type's written form begins with const qualifier.
 * @param[in] typeText Type name string view to inspect.
 * @return True if typeText starts with const, false otherwise.
 */
static bool TypeTextIsConst(std::string_view typeText)
{
    return HasLeadingConst(typeText);
}

/**
 * @brief Checks if a handle type is declared const itself (e.g. Type@ const).
 * @param[in] typeText Type name string view to inspect.
 * @return True if handle type has trailing const, false otherwise.
 */
static bool TypeTextIsHandleConst(std::string_view typeText)
{
    return HasTrailingConst(typeText);
}

/**
 * @brief Constness of a parameter, read off the enclosing declaration's parameter list.
 *        The scope tree records a parameter as a definition but not its written type - it fills
 * @param[in] node AST node inside the function or method.
 * @param[in] name Identifier name of the variable to search for.
 * @param[in] sourceCode Document source text.
 * @param[in] checkHandleConst True to check if handle itself is const (Type@ const).
 * @return True/false if parameter found and whether its type is const; std::nullopt if not found.
 */
static std::optional<bool> ParameterIsConst(TSNode node, std::string_view name, std::string_view sourceCode,
                                            bool checkHandleConst = false)
{
    const auto& gs = parser::GetGrammarSymbols();
    const std::array<TSSymbol, 4> ownerSyms = {gs.symFuncDeclaration, gs.symLambdaExpression, gs.symVirtualProperty,
                                               gs.symAccessor};
    TSNode owner = parser::FindAncestorWithAnySymbol(node, ownerSyms);
    if (ts_node_is_null(owner))
    {
        return std::nullopt;
    }

    TSNode parameters = parser::GetChildByField(owner, parser::GetGrammarSymbols().fieldParameters);
    if (ts_node_is_null(parameters))
    {
        return std::nullopt;
    }

    const uint32_t count = ts_node_named_child_count(parameters);
    for (uint32_t i = 0; i < count; ++i)
    {
        TSNode parameter = ts_node_named_child(parameters, i);
        if (ts_node_symbol(parameter) != gs.symParameter)
        {
            continue;
        }

        TSNode nameNode = parser::GetChildByField(parameter, gs.fieldName);
        if (ts_node_is_null(nameNode) || parser::NodeText(nameNode, sourceCode) != name)
        {
            continue;
        }

        TSNode typeNode = parser::GetChildByField(parameter, parser::GetGrammarSymbols().fieldParamType);
        if (ts_node_is_null(typeNode))
        {
            return false;
        }
        const std::string_view typeText = parser::NodeText(typeNode, sourceCode);
        return checkHandleConst ? TypeTextIsHandleConst(typeText) : TypeTextIsConst(typeText);
    }
    return std::nullopt;
}

/**
 * @brief Resolves the constness of a plain identifier name across local and global scopes.
 * @param[in] name Identifier name of the variable or property.
 * @param[in] node AST node representing the identifier usage.
 * @param[in] ctx Expression type context containing scope and symbol table.
 * @return Resolved Constness enum value.
 */
static Constness ResolveNameConstness(std::string_view name, TSNode node, const ExpressionTypeContext& ctx)
{
    if (name.empty())
    {
        return Constness::Unknown;
    }

    // A local or a parameter first, since either shadows a global of the same name.
    if (ctx.scope)
    {
        if (const LocalDefinition* def = ResolveInScopeAt(ctx.scope, name, ts_node_start_byte(node)))
        {
            if (!def->typeName.empty())
            {
                return TypeTextIsConst(def->typeName) ? Constness::Const : Constness::Mutable;
            }

            // No recorded type means either a parameter, whose type the scope tree does not
            // keep, or a foreach variable, whose type is not written anywhere at all. The
            // first is answerable from the tree; the second is not, and Unknown is the only
            // honest answer for it.
            const auto isConst = ParameterIsConst(node, name, ctx.sourceCode);
            if (isConst.has_value())
            {
                return *isConst ? Constness::Const : Constness::Mutable;
            }
            return Constness::Unknown;
        }
    }

    const auto symbols = ctx.symbolTable.FindSymbolsPtr(name);
    if (!symbols)
    {
        return Constness::Unknown;
    }

    for (const auto& sym : *symbols)
    {
        if (sym.type == SymbolType::Variable || sym.type == SymbolType::Property)
        {
            return sym.GetVariable().modifiers.isConst ? Constness::Const : Constness::Mutable;
        }
    }
    return Constness::Unknown;
}

/**
 * @brief Resolves the constness of a leaf AST expression (identifier or this).
 * @param[in] node AST leaf node under evaluation.
 * @param[in] ctx Expression type context containing symbols and source.
 * @return Resolved Constness enum value.
 */
static Constness ResolveLeafConstness(TSNode node, const ExpressionTypeContext& ctx)
{
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol nodeSym = ts_node_symbol(node);

    if (nodeSym == gs.symThisExpression ||
        (nodeSym == gs.symIdentifier && parser::NodeText(node, ctx.sourceCode) == parser::keywords::This))
    {
        const auto encInfo = FindEnclosingMethod(node, ctx.sourceCode);
        return (encInfo.inClass && encInfo.isConstMethod) ? Constness::Const : Constness::Mutable;
    }

    if (nodeSym == gs.symIdentifier)
    {
        return ResolveNameConstness(parser::NodeText(node, ctx.sourceCode), node, ctx);
    }

    if (nodeSym == gs.symScopedIdentifier)
    {
        const std::string whole(parser::NodeText(node, ctx.sourceCode));
        const Constness qualified = ResolveNameConstness(whole, node, ctx);
        if (qualified != Constness::Unknown)
        {
            return qualified;
        }
        return ResolveNameConstness(LastScopeSegment(whole), node, ctx);
    }

    return Constness::Unknown;
}

/**
 * @brief Resolves whether an AST expression evaluates to a const or mutable value.
 * @param[in] node AST expression node to inspect.
 * @param[in] ctx Expression type context containing symbols and source.
 * @return Resolved Constness enum value.
 */
Constness ResolveConstness(TSNode node, const ExpressionTypeContext& ctx)
{
    const auto& gs = parser::GetGrammarSymbols();
    TSNode current = node;
    bool inMemberChain = false;

    while (!ts_node_is_null(current))
    {
        const TSSymbol nodeSym = ts_node_symbol(current);

        if (nodeSym == gs.symMemberExpression)
        {
            inMemberChain = true;
            current = parser::GetChildByField(current, gs.fieldObject);
            continue;
        }

        if (nodeSym == gs.symParenthesizedExpression)
        {
            current = ts_node_named_child_count(current) > 0 ? ts_node_named_child(current, 0) : TSNode{};
            continue;
        }

        const Constness leaf = ResolveLeafConstness(current, ctx);
        if (inMemberChain)
        {
            return leaf == Constness::Const ? Constness::Const : Constness::Unknown;
        }
        return leaf;
    }

    return Constness::Unknown;
}

/**
 * @brief Resolves constness for a handle receiver expression.
 * @param[in] node AST node representing the handle receiver.
 * @param[in] ctx Expression type context containing symbols and source.
 * @return Resolved Constness enum value.
 */
Constness ResolveHandleConstness(TSNode node, const ExpressionTypeContext& ctx)
{
    if (ts_node_is_null(node))
    {
        return Constness::Unknown;
    }

    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol nodeSym = ts_node_symbol(node);
    if (nodeSym == gs.symIdentifier || nodeSym == gs.symScopedIdentifier)
    {
        const std::string name(parser::NodeText(node, ctx.sourceCode));
        if (ctx.scope)
        {
            if (const LocalDefinition* def =
                    ResolveInScopeAt(ctx.scope, LastScopeSegment(name), ts_node_start_byte(node)))
            {
                if (!def->typeName.empty())
                {
                    return TypeTextIsHandleConst(def->typeName) ? Constness::Const : Constness::Mutable;
                }
                const auto isConst = ParameterIsConst(node, name, ctx.sourceCode, true);
                if (isConst.has_value())
                {
                    return *isConst ? Constness::Const : Constness::Mutable;
                }
            }
        }
        if (const auto symbols = ctx.symbolTable.FindSymbolsPtr(name))
        {
            for (const auto& sym : *symbols)
            {
                if (sym.type == SymbolType::Variable || sym.type == SymbolType::Property)
                {
                    return TypeTextIsHandleConst(sym.GetVariable().typeName) ? Constness::Const : Constness::Mutable;
                }
            }
        }
    }
    return Constness::Unknown;
}

} // namespace angel_lsp::analysis
