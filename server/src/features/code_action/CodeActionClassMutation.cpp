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
 * @brief Collects declared class field names from the symbol table and class body AST.
 * @param[in] context Mutation check context.
 * @return Set of member field names for the active class.
 */
ankerl::unordered_dense::set<std::string> CollectClassFields(const ClassMutationContext& context)
{
    ankerl::unordered_dense::set<std::string> classFields;
    context.table.ForEachSymbol(
        [&]([[maybe_unused]] const std::string& qualifiedName, const std::vector<analysis::Symbol>& symList)
        {
            for (const auto& sym : symList)
            {
                if (sym.containerName == context.className &&
                    (sym.type == analysis::SymbolType::Variable || sym.type == analysis::SymbolType::Property))
                {
                    classFields.insert(sym.name);
                }
            }
        });

    if (ts_node_is_null(context.classNode))
    {
        return classFields;
    }

    TSNode cBody = parser::GetChildByField(context.classNode, parser::fields::Body);
    if (ts_node_is_null(cBody))
    {
        uint32_t cnt = ts_node_child_count(context.classNode);
        for (uint32_t i = 0; i < cnt; ++i)
        {
            TSNode ch = ts_node_child(context.classNode, i);
            if (std::string_view(ts_node_type(ch)) == "class_body")
            {
                cBody = ch;
                break;
            }
        }
    }

    if (!ts_node_is_null(cBody))
    {
        uint32_t bCnt = ts_node_child_count(cBody);
        for (uint32_t i = 0; i < bCnt; ++i)
        {
            TSNode ch = ts_node_child(cBody, i);
            if (std::string_view(ts_node_type(ch)) == "variable_declaration")
            {
                uint32_t vCnt = ts_node_child_count(ch);
                for (uint32_t j = 0; j < vCnt; ++j)
                {
                    TSNode vCh = ts_node_child(ch, j);
                    if (std::string_view(ts_node_type(vCh)) == "variable_declarator")
                    {
                        TSNode vNameNode = parser::GetChildByField(vCh, parser::fields::Name);
                        std::string fName = GetNodeText(vNameNode, context.sourceCode);
                        if (!fName.empty())
                        {
                            classFields.insert(fName);
                        }
                    }
                }
            }
        }
    }
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
    if (ts_node_is_null(callee))
    {
        return false;
    }
    std::string_view cType = ts_node_type(callee);
    std::string callMethodName;
    bool isMemberOnThis = false;

    if (cType == "identifier" || cType == "scoped_identifier")
    {
        callMethodName = GetNodeText(callee, context.sourceCode);
        isMemberOnThis = true;
    }
    else if (cType == "member_expression")
    {
        TSNode obj = parser::GetChildByField(callee, parser::fields::Object);
        TSNode mem = parser::GetChildByField(callee, parser::fields::Member);
        if (!ts_node_is_null(obj) && std::string_view(ts_node_type(obj)) == "this_expression")
        {
            callMethodName = GetNodeText(mem, context.sourceCode);
            isMemberOnThis = true;
        }
    }

    if (isMemberOnThis && !callMethodName.empty())
    {
        bool mutates = false;
        context.table.ForEachSymbol(
            [&]([[maybe_unused]] const std::string& qualifiedName, const std::vector<analysis::Symbol>& symList)
            {
                for (const auto& sym : symList)
                {
                    if (sym.type == analysis::SymbolType::Function && sym.containerName == context.className &&
                        sym.name == callMethodName)
                    {
                        if (!sym.GetFunction().modifiers.isConst)
                        {
                            mutates = true;
                        }
                    }
                }
            });
        return mutates;
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
    std::vector<TSNode> stack = {context.bodyNode};
    while (!stack.empty())
    {
        TSNode curr = stack.back();
        stack.pop_back();

        if (CheckAssignmentMutatesClassState(context, classFields, curr) ||
            CheckIncDecMutatesClassState(context, classFields, curr) || CheckMethodCallMutatesClassState(context, curr))
        {
            return true;
        }

        uint32_t childCount = ts_node_child_count(curr);
        for (uint32_t i = 0; i < childCount; ++i)
        {
            stack.push_back(ts_node_child(curr, i));
        }
    }
    return false;
}

} // namespace angel_lsp::features
