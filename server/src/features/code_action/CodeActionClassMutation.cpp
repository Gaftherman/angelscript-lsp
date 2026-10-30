/**
 * @file CodeActionClassMutation.cpp
 * @brief Analysis of method bodies for mutations against class state.
 */

#include "features/code_action/CodeActionClassMutation.h"
#include "features/code_action/CodeActionInternal.h"

namespace angel_lsp::features
{
namespace
{

/**
 * @brief Checks if a node type represents a method or constructor declaration that should not be recursed into.
 * @param[in] nodeType Tree-sitter node type.
 * @return True if subtree should be skipped for field collection.
 */
bool IsSubtreeSkippable(TSSymbol sym, const parser::GrammarSymbols& gs) noexcept
{
    return sym == gs.symFuncDeclaration;
}

/**
 * @brief Traverses up the AST cursor to find the next available sibling or reaches root.
 * @param[in,out] cursor Active tree-sitter cursor.
 * @param[in] rootNode Enclosing root node boundary.
 * @return True if a next sibling was found, false if root was reached.
 */
bool StepUpCursor(TSTreeCursor& cursor, TSNode rootNode)
{
    while (true)
    {
        if (!ts_tree_cursor_goto_parent(&cursor))
        {
            return false;
        }
        if (ts_node_eq(ts_tree_cursor_current_node(&cursor), rootNode))
        {
            return false;
        }
        if (ts_tree_cursor_goto_next_sibling(&cursor))
        {
            return true;
        }
    }
}

/**
 * @brief Collects field declarator names from a class AST node using a flat cursor walk.
 * @param[in] classNode Class declaration AST node.
 * @param[in] sourceCode Document source text.
 * @param[in,out] classFields Set of member field names to populate.
 */
void CollectAstClassFields(TSNode classNode, std::string_view sourceCode,
                           ankerl::unordered_dense::set<std::string>& classFields)
{
    if (ts_node_is_null(classNode))
    {
        return;
    }

    TSTreeCursor cursor = ts_tree_cursor_new(classNode);

    const auto& gs = parser::GetGrammarSymbols();
    while (true)
    {
        TSNode curr = ts_tree_cursor_current_node(&cursor);
        const TSSymbol sym = ts_node_is_null(curr) ? static_cast<TSSymbol>(0) : ts_node_symbol(curr);

        if (sym == gs.symVariableDeclarator)
        {
            TSNode vNameNode = parser::GetChildByField(curr, gs.fieldName);
            std::string fName = GetNodeText(vNameNode, sourceCode);
            if (!fName.empty())
            {
                classFields.insert(std::move(fName));
            }
        }

        if (!IsSubtreeSkippable(sym, gs) && ts_tree_cursor_goto_first_child(&cursor))
        {
            continue;
        }
        if (ts_tree_cursor_goto_next_sibling(&cursor))
        {
            continue;
        }
        if (!StepUpCursor(cursor, classNode))
        {
            break;
        }
    }

    ts_tree_cursor_delete(&cursor);
}

/**
 * @brief Collects declared class field names from the symbol table and class body AST.
 * @param[in] context Mutation check context.
 * @return Set of member field names for the active class.
 */
ankerl::unordered_dense::set<std::string> CollectClassFields(const ClassMutationContext& context)
{
    ankerl::unordered_dense::set<std::string> classFields;
    if (const auto ruleIndex = context.table.GetRuleIndex())
    {
        const auto& cm = ruleIndex->Members(context.className);
        for (const auto& memberKey : cm.memberKeys)
        {
            auto symsPtr = context.table.FindSymbolsPtr(memberKey);
            if (!symsPtr)
            {
                continue;
            }
            for (const auto& sym : *symsPtr)
            {
                if (sym.containerName == context.className &&
                    (sym.type == analysis::SymbolType::Variable || sym.type == analysis::SymbolType::Property))
                {
                    classFields.insert(sym.name);
                }
            }
        }
    }

    CollectAstClassFields(context.classNode, context.sourceCode, classFields);
    return classFields;
}

/**
 * @brief Checks if a resolved variable node refers to a class field rather than a local variable.
 * @param[in] context Mutation check context.
 * @param[in] classFields Set of known class field names.
 * @param[in] idNode Identifier AST node.
 * @param[in] varName Variable name.
 * @return True if target refers to a class field.
 */
bool IsFieldOrOuterVar(const ClassMutationContext& context,
                       const ankerl::unordered_dense::set<std::string>& classFields, TSNode idNode,
                       const std::string& varName)
{
    TSPoint pt = ts_node_start_point(idNode);
    const analysis::Scope* inner = FindScopeByLineOrRoot(context.scope, pt.row);
    const analysis::LocalDefinition* localDef = analysis::ResolveInScope(inner, varName);

    bool isField = classFields.contains(varName);
    if (localDef)
    {
        if (localDef->kind == analysis::LocalDefinitionKind::Field)
        {
            isField = true;
        }
        else if (localDef->kind == analysis::LocalDefinitionKind::Variable ||
                 localDef->kind == analysis::LocalDefinitionKind::Parameter)
        {
            if (localDef->startLine >= ts_node_start_point(context.bodyNode).row &&
                localDef->endLine <= ts_node_end_point(context.bodyNode).row)
            {
                isField = false;
            }
        }
    }
    return isField;
}

/**
 * @brief Checks if an assignment expression mutates a class field.
 * @param[in] context Mutation check context.
 * @param[in] classFields Set of class field names.
 * @param[in] curr Candidate assignment AST node.
 * @return True if assignment modifies a class field.
 */
bool CheckAssignmentMutatesClassState(const ClassMutationContext& context,
                                      const ankerl::unordered_dense::set<std::string>& classFields, TSNode curr)
{
    const auto& gs = parser::GetGrammarSymbols();
    if (ts_node_symbol(curr) != gs.symAssignmentExpression)
    {
        return false;
    }
    TSNode left = parser::GetChildByField(curr, gs.fieldLeft);
    if (ts_node_is_null(left))
    {
        return false;
    }
    const TSSymbol lSym = ts_node_symbol(left);
    if (lSym == gs.symIdentifier || lSym == gs.symScopedIdentifier)
    {
        std::string varName = GetNodeText(left, context.sourceCode);
        return IsFieldOrOuterVar(context, classFields, left, varName);
    }
    if (lSym == gs.symMemberExpression)
    {
        TSNode obj = parser::GetChildByField(left, gs.fieldObject);
        TSNode mem = parser::GetChildByField(left, gs.fieldMember);
        if (!ts_node_is_null(obj) && ts_node_symbol(obj) == gs.symThisExpression)
        {
            std::string memName = GetNodeText(mem, context.sourceCode);
            return classFields.contains(memName);
        }
    }
    return false;
}

/**
 * @brief Checks if a prefix or postfix increment/decrement mutates a class field.
 * @param[in] context Mutation check context.
 * @param[in] classFields Set of class field names.
 * @param[in] curr Candidate increment/decrement AST node.
 * @return True if operator mutates a class field.
 */
bool CheckIncDecMutatesClassState(const ClassMutationContext& context,
                                  const ankerl::unordered_dense::set<std::string>& classFields, TSNode curr)
{
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol type = ts_node_symbol(curr);
    if (type != gs.symPostfixExpression && type != gs.symUnaryExpression)
    {
        return false;
    }
    TSNode opNode = parser::GetChildByField(curr, gs.fieldOperator);
    const TSSymbol opSym = ts_node_symbol(opNode);
    if (opSym != gs.tokPlusPlus && opSym != gs.tokDashDash)
    {
        return false;
    }
    TSNode arg = parser::GetChildByField(curr, gs.fieldOperand);
    if (ts_node_is_null(arg))
    {
        return false;
    }
    const TSSymbol aType = ts_node_symbol(arg);
    if (aType == gs.symIdentifier || aType == gs.symScopedIdentifier)
    {
        std::string varName = GetNodeText(arg, context.sourceCode);
        return IsFieldOrOuterVar(context, classFields, arg, varName);
    }
    return false;
}

/**
 * @brief Resolves method name called on `this` implicitly or explicitly.
 * @param[in] callee Function expression AST node.
 * @param[in] sourceCode Document source text.
 * @return Method identifier text if invoked on `this`, or empty string.
 */
std::string ExtractThisCallMethodName(TSNode callee, std::string_view sourceCode)
{
    if (ts_node_is_null(callee))
    {
        return {};
    }
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol cType = ts_node_symbol(callee);
    if (cType == gs.symIdentifier || cType == gs.symScopedIdentifier)
    {
        return GetNodeText(callee, sourceCode);
    }
    if (cType == gs.symMemberExpression)
    {
        TSNode obj = parser::GetChildByField(callee, gs.fieldObject);
        TSNode mem = parser::GetChildByField(callee, gs.fieldMember);
        if (!ts_node_is_null(obj) && ts_node_symbol(obj) == gs.symThisExpression)
        {
            return GetNodeText(mem, sourceCode);
        }
    }
    return {};
}

/**
 * @brief Checks if a method call on `this` calls a non-const method.
 * @param[in] context Mutation check context.
 * @param[in] curr Candidate call expression AST node.
 * @return True if method call mutates class state.
 */
bool CheckMethodCallMutatesClassState(const ClassMutationContext& context, TSNode curr)
{
    const auto& gs = parser::GetGrammarSymbols();
    if (ts_node_symbol(curr) != gs.symCallExpression)
    {
        return false;
    }
    TSNode callee = parser::GetChildByField(curr, gs.fieldFunction);
    std::string callMethodName = ExtractThisCallMethodName(callee, context.sourceCode);
    if (callMethodName.empty())
    {
        return false;
    }

    auto symsPtr = context.table.FindMemberSymbolPtr(context.className, callMethodName);
    if (!symsPtr)
    {
        return false;
    }
    for (const auto& sym : *symsPtr)
    {
        if (sym.type == analysis::SymbolType::Function && sym.containerName == context.className &&
            sym.name == callMethodName && !sym.GetFunction().modifiers.isConst)
        {
            return true;
        }
    }
    return false;
}

} // namespace

bool MethodBodyMutatesClassState(const ClassMutationContext& context)
{
    if (ts_node_is_null(context.bodyNode))
    {
        return false;
    }

    auto classFields = CollectClassFields(context);
    TSTreeCursor cursor = ts_tree_cursor_new(context.bodyNode);

    while (true)
    {
        TSNode curr = ts_tree_cursor_current_node(&cursor);
        if (!ts_node_is_null(curr) && (CheckAssignmentMutatesClassState(context, classFields, curr) ||
                                       CheckIncDecMutatesClassState(context, classFields, curr) ||
                                       CheckMethodCallMutatesClassState(context, curr)))
        {
            ts_tree_cursor_delete(&cursor);
            return true;
        }

        if (ts_tree_cursor_goto_first_child(&cursor))
        {
            continue;
        }
        if (ts_tree_cursor_goto_next_sibling(&cursor))
        {
            continue;
        }
        if (!StepUpCursor(cursor, context.bodyNode))
        {
            break;
        }
    }

    ts_tree_cursor_delete(&cursor);
    return false;
}

} // namespace angel_lsp::features
