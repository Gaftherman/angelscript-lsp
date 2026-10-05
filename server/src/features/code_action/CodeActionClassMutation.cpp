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
bool IsSubtreeSkippable(std::string_view nodeType) noexcept
{
    return nodeType == "function_declaration" || nodeType == "method_declaration" ||
           nodeType == "constructor_declaration" || nodeType == "destructor_declaration";
}

/**
 * @brief Traverses up the AST cursor to find the next available sibling or reaches root.
 * @param[in,out] cursor Active tree-sitter cursor.
 * @param[in,out] depth Current traversal depth counter.
 * @param[in] rootNode Enclosing root node boundary.
 * @return True if a next sibling was found, false if root was reached.
 */
bool StepUpCursor(TSTreeCursor& cursor, size_t& depth, TSNode rootNode)
{
    while (true)
    {
        if (!ts_tree_cursor_goto_parent(&cursor))
        {
            return false;
        }
        if (depth > 0)
        {
            --depth;
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
    size_t depth = 0;
    constexpr size_t k_maxAstDepth = 64;

    while (true)
    {
        TSNode curr = ts_tree_cursor_current_node(&cursor);
        std::string_view nodeType = ts_node_is_null(curr) ? std::string_view{} : std::string_view(ts_node_type(curr));

        if (nodeType == "variable_declarator")
        {
            TSNode vNameNode = parser::GetChildByField(curr, parser::fields::Name);
            std::string fName = GetNodeText(vNameNode, sourceCode);
            if (!fName.empty())
            {
                classFields.insert(std::move(fName));
            }
        }

        if (!IsSubtreeSkippable(nodeType) && depth < k_maxAstDepth && ts_tree_cursor_goto_first_child(&cursor))
        {
            ++depth;
            continue;
        }
        if (ts_tree_cursor_goto_next_sibling(&cursor))
        {
            continue;
        }
        if (!StepUpCursor(cursor, depth, classNode))
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
    if (std::string_view(ts_node_type(curr)) != "assignment_expression")
    {
        return false;
    }
    TSNode left = parser::GetChildByField(curr, parser::fields::Left);
    if (ts_node_is_null(left))
    {
        return false;
    }
    std::string_view lType = ts_node_type(left);
    if (lType == "identifier" || lType == "scoped_identifier")
    {
        std::string varName = GetNodeText(left, context.sourceCode);
        return IsFieldOrOuterVar(context, classFields, left, varName);
    }
    if (lType == "member_expression")
    {
        TSNode obj = parser::GetChildByField(left, parser::fields::Object);
        TSNode mem = parser::GetChildByField(left, parser::fields::Member);
        if (!ts_node_is_null(obj) && std::string_view(ts_node_type(obj)) == "this_expression")
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
    std::string_view type = ts_node_type(curr);
    if (type != "postfix_expression" && type != "unary_expression")
    {
        return false;
    }
    TSNode opNode = parser::GetChildByField(curr, parser::fields::Operator);
    std::string op = GetNodeText(opNode, context.sourceCode);
    if (op != "++" && op != "--")
    {
        return false;
    }
    TSNode arg = parser::GetChildByField(curr, parser::fields::Operand);
    if (ts_node_is_null(arg))
    {
        return false;
    }
    std::string_view aType = ts_node_type(arg);
    if (aType == "identifier" || aType == "scoped_identifier")
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
    std::string_view cType = ts_node_type(callee);
    if (cType == "identifier" || cType == "scoped_identifier")
    {
        return GetNodeText(callee, sourceCode);
    }
    if (cType == "member_expression")
    {
        TSNode obj = parser::GetChildByField(callee, parser::fields::Object);
        TSNode mem = parser::GetChildByField(callee, parser::fields::Member);
        if (!ts_node_is_null(obj) && std::string_view(ts_node_type(obj)) == "this_expression")
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
    if (std::string_view(ts_node_type(curr)) != "call_expression")
    {
        return false;
    }
    TSNode callee = parser::GetChildByField(curr, parser::fields::Function);
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
    size_t depth = 0;
    constexpr size_t k_maxAstDepth = 64;

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

        if (depth < k_maxAstDepth && ts_tree_cursor_goto_first_child(&cursor))
        {
            ++depth;
            continue;
        }
        if (ts_tree_cursor_goto_next_sibling(&cursor))
        {
            continue;
        }
        if (!StepUpCursor(cursor, depth, context.bodyNode))
        {
            break;
        }
    }

    ts_tree_cursor_delete(&cursor);
    return false;
}

} // namespace angel_lsp::features
