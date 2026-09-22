#include "features/code_action/CodeActionHandler.h"
#include "analysis/DiagnosticCodes.h"
#include "analysis/ScopeTree.h"
#include "analysis/SemanticHelpers.h"
#include "analysis/SymbolTable.h"
#include "parser/GrammarNames.h"
#include "utils/IncludeResolver.h"
#include "utils/PositionEncoding.h"
#include "utils/Utils.h"
#include <algorithm>
#include <ankerl/unordered_dense.h>
#include <cctype>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace angel_lsp::features
{
namespace
{
/**
 * @brief Extracts text slice of an AST node from the source code.
 * @param[in] node Tree-sitter AST node.
 * @param[in] sourceCode Source document text.
 * @return Extracted node text.
 */
std::string GetNodeText(TSNode node, std::string_view sourceCode)
{
    if (ts_node_is_null(node))
    {
        return "";
    }
    uint32_t startByte = ts_node_start_byte(node);
    uint32_t endByte = ts_node_end_byte(node);
    if (startByte >= sourceCode.size() || endByte > sourceCode.size() || startByte >= endByte)
    {
        return "";
    }
    return std::string(sourceCode.substr(startByte, endByte - startByte));
}

/**
 * @brief Extracts leading whitespace indentation of a given 0-indexed line.
 * @param[in] sourceCode Source document text.
 * @param[in] line 0-based target line index.
 * @return Leading indentation string.
 */
std::string GetLineIndentation(std::string_view sourceCode, uint32_t line)
{
    uint32_t currLine = 0;
    size_t lineStart = 0;
    for (size_t i = 0; i < sourceCode.size(); ++i)
    {
        if (currLine == line)
        {
            lineStart = i;
            break;
        }
        if (sourceCode[i] == '\n')
        {
            currLine++;
        }
    }
    if (currLine != line)
    {
        return "";
    }
    size_t i = lineStart;
    while (i < sourceCode.size() && (sourceCode[i] == ' ' || sourceCode[i] == '\t'))
    {
        i++;
    }
    return std::string(sourceCode.substr(lineStart, i - lineStart));
}

/**
 * @brief Locates the innermost lexical scope containing the given line.
 * @param[in] root Root scope node.
 * @param[in] line 0-based line number.
 * @return Innermost matching scope or root if uncontained.
 */
const analysis::Scope* FindScopeByLine(const analysis::Scope* root, uint32_t line)
{
    if (!root)
    {
        return nullptr;
    }
    const analysis::Scope* curr = root;
    bool movedDeeper = true;
    while (movedDeeper)
    {
        movedDeeper = false;
        for (const auto& child : curr->children)
        {
            if (child->startLine <= line && child->endLine >= line)
            {
                curr = child.get();
                movedDeeper = true;
                break;
            }
        }
    }
    return curr;
}

/**
 * @brief Locates the innermost lexical scope containing the given point, falling back to root.
 * @param[in] root Root scope node.
 * @param[in] line 0-based line number.
 * @param[in] character 0-based character offset.
 * @return Innermost scope or root if none matches.
 */
const analysis::Scope* FindScopeByLineOrRoot(const analysis::Scope* root, uint32_t line, uint32_t character)
{
    (void)character;
    return FindScopeByLine(root, line);
}

/**
 * @brief Collects all identifier reference names across the scope hierarchy using a worklist.
 * @param[in] rootScope Root of scope subtree.
 * @param[out] refs Destination reference name set.
 */
void CollectAllReferences(const analysis::Scope* rootScope, ankerl::unordered_dense::set<std::string>& refs)
{
    if (!rootScope)
    {
        return;
    }
    std::vector<const analysis::Scope*> worklist = {rootScope};
    while (!worklist.empty())
    {
        const analysis::Scope* sc = worklist.back();
        worklist.pop_back();
        for (const auto& ref : sc->references)
        {
            refs.insert(ref.name);
        }
        for (const auto& child : sc->children)
        {
            worklist.push_back(child.get());
        }
    }
}

/**
 * @brief Collects definitions that have at least one valid reference across scopes using a worklist.
 * @param[in] rootScope Root of scope subtree.
 * @param[out] used Destination set of referenced definitions.
 */
void CollectUsedDefinitions(const analysis::Scope* rootScope,
                            ankerl::unordered_dense::set<const analysis::LocalDefinition*>& used)
{
    if (!rootScope)
    {
        return;
    }
    std::vector<const analysis::Scope*> worklist = {rootScope};
    while (!worklist.empty())
    {
        const analysis::Scope* sc = worklist.back();
        worklist.pop_back();

        for (const auto& ref : sc->references)
        {
            if (ref.isMemberAccess)
            {
                continue;
            }
            const analysis::LocalDefinition* def = analysis::ResolveInScope(sc, ref.name);
            if (!def)
            {
                continue;
            }
            if (def->startLine == ref.startLine && def->startCharacter == ref.startCharacter &&
                def->endLine == ref.endLine && def->endCharacter == ref.endCharacter)
            {
                continue;
            }
            used.insert(def);
        }

        for (const auto& child : sc->children)
        {
            worklist.push_back(child.get());
        }
    }
}

/**
 * @brief Traverses scopes with a worklist to collect unused local variables.
 * @param[in] rootScope Root of scope subtree.
 * @param[in] used Set of referenced definitions.
 * @param[out] unused Destination vector for unused variable definitions.
 */
void CollectUnusedVariables(const analysis::Scope* rootScope,
                            const ankerl::unordered_dense::set<const analysis::LocalDefinition*>& used,
                            std::vector<const analysis::LocalDefinition*>& unused)
{
    if (!rootScope)
    {
        return;
    }
    struct ScopeItem
    {
        const analysis::Scope* scope;
        bool isFuncNested;
    };
    std::vector<ScopeItem> worklist = {{rootScope, rootScope->isFunctionScope}};

    while (!worklist.empty())
    {
        auto [sc, funcNested] = worklist.back();
        worklist.pop_back();

        bool nested = funcNested || sc->isFunctionScope;
        if (nested)
        {
            for (const auto& def : sc->definitions)
            {
                if (def.kind == analysis::LocalDefinitionKind::Variable && !used.contains(&def))
                {
                    unused.push_back(&def);
                }
            }
        }
        for (const auto& child : sc->children)
        {
            worklist.push_back({child.get(), nested});
        }
    }
}

/**
 * @brief Returns default literal return string for a return type.
 * @param[in] returnType Return type text.
 * @return Default literal return text ("null", "false", "0", "\"\"").
 */
std::string GetDefaultReturnValue(std::string_view returnType)
{
    std::string cleanRet = analysis::CleanBaseType(returnType);
    if (returnType.ends_with("@"))
    {
        return "null";
    }
    if (cleanRet == "bool")
    {
        return "false";
    }
    if (cleanRet == "string")
    {
        return "\"\"";
    }
    static const ankerl::unordered_dense::set<std::string_view> kNumericTypes = {
        "int", "int8", "int16", "int32", "int64", "uint", "uint8", "uint16", "uint32", "uint64", "float", "double"};
    if (kNumericTypes.contains(cleanRet))
    {
        return "0";
    }
    return "null";
}

static constexpr std::string_view kExtractableExpressionTypes[] = {"binary_expression",
                                                                   "unary_expression",
                                                                   "postfix_expression",
                                                                   "call_expression",
                                                                   "member_expression",
                                                                   "ternary_expression",
                                                                   "cast_expression",
                                                                   "functional_cast_expression",
                                                                   "construct_call_expression",
                                                                   "index_expression",
                                                                   "parenthesized_expression",
                                                                   "scoped_identifier",
                                                                   "identifier",
                                                                   "number_literal",
                                                                   "string_literal",
                                                                   "boolean_literal"};

/**
 * @brief Checks whether an AST node is an extractable expression.
 * @param[in] node AST node.
 * @return True if candidate node can be extracted into a variable or method.
 */
bool IsExtractableExpression(TSNode node)
{
    if (ts_node_is_null(node))
    {
        return false;
    }
    std::string_view type = ts_node_type(node);
    for (std::string_view candidate : kExtractableExpressionTypes)
    {
        if (type == candidate)
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief Checks whether an AST node is the left-hand side of an assignment.
 * @param[in] node Candidate expression AST node.
 * @return True if node is on the LHS of an assignment.
 */
bool IsLhsOfAssignment(TSNode node)
{
    TSNode curr = node;
    while (!ts_node_is_null(curr))
    {
        TSNode parent = ts_node_parent(curr);
        if (!ts_node_is_null(parent))
        {
            std::string_view pType = ts_node_type(parent);
            if (pType == "assignment_expression")
            {
                TSNode left = parser::GetChildByField(parent, parser::fields::Left);
                if (ts_node_start_byte(left) == ts_node_start_byte(curr) &&
                    ts_node_end_byte(left) == ts_node_end_byte(curr))
                {
                    return true;
                }
            }
        }
        curr = parent;
    }
    return false;
}

/**
 * @brief Cleans property name by stripping m_ or _ prefix and capitalizing the first character.
 * @param[in] fieldName Member variable name.
 * @return Cleaned property name suitable for accessor generation.
 */
std::string CleanPropertyName(std::string_view fieldName)
{
    std::string prop(fieldName);
    if (prop.starts_with("m_") && prop.size() > 2)
    {
        prop = prop.substr(2);
    }
    else if (prop.starts_with("_") && prop.size() > 1)
    {
        prop = prop.substr(1);
    }
    if (!prop.empty())
    {
        prop[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(prop[0])));
    }
    return prop;
}

struct ClassMutationContext
{
    TSNode bodyNode;
    TSNode classNode;
    std::string_view sourceCode;
    const analysis::SymbolTable& table;
    const std::string& className;
    const analysis::Scope* scope = nullptr;
};

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
    const analysis::Scope* inner = FindScopeByLineOrRoot(context.scope, pt.row, pt.column);
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

/**
 * @brief Checks if a method body mutates class fields or calls non-const methods on `this`.
 * @param[in] context Mutation evaluation context.
 * @return True if method body mutates class state.
 */
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

/**
 * @brief Locates the target extractable expression within the requested selection range.
 * @param[in] rootNode Root of AST.
 * @param[in] range Selected text range.
 * @return Valid extractable node or null node if none matches.
 */
TSNode FindExtractableExpression(TSNode rootNode, const lsp::Range& range)
{
    TSPoint startPt = {range.start.line, range.start.character};
    TSPoint endPt = {range.end.line, range.end.character};
    TSNode targetNode = ts_node_descendant_for_point_range(rootNode, startPt, endPt);
    while (!ts_node_is_null(targetNode) && !IsExtractableExpression(targetNode))
    {
        TSNode p = ts_node_parent(targetNode);
        if (ts_node_is_null(p))
        {
            break;
        }
        targetNode = p;
    }
    if (ts_node_is_null(targetNode) || !IsExtractableExpression(targetNode))
    {
        return TSNode{};
    }
    std::string_view nodeType = ts_node_type(targetNode);
    if (nodeType.ends_with("_statement") || nodeType.ends_with("_declaration") || nodeType == "statement_block" ||
        nodeType == "class_body" || nodeType == "parameter" || nodeType == "primitive_type" ||
        IsLhsOfAssignment(targetNode))
    {
        return TSNode{};
    }
    return targetNode;
}

/**
 * @brief Finds the innermost block-level statement enclosing the target node.
 * @param[in] node AST node.
 * @return Enclosing statement AST node.
 */
TSNode FindEnclosingBlockStatement(TSNode node)
{
    TSNode stmtNode = node;
    while (!ts_node_is_null(stmtNode))
    {
        TSNode parent = ts_node_parent(stmtNode);
        if (!ts_node_is_null(parent) && std::string_view(ts_node_type(parent)) == "statement_block")
        {
            return stmtNode;
        }
        stmtNode = parent;
    }
    return TSNode{};
}

/**
 * @brief Constructs an Extract Variable refactoring code action.
 * @param[in] request Code action request context.
 * @param[in] targetNode Target expression node.
 * @param[in] stmtNode Statement node above which declaration is inserted.
 * @return Code action if construction succeeds, std::nullopt otherwise.
 */
std::optional<lsp::CodeAction> BuildExtractVariableAction(const CodeActionRequest& request, TSNode targetNode,
                                                          TSNode stmtNode)
{
    auto rootScope = request.scopeIndex.GetRoot(request.uri);
    TSPoint exprStart = ts_node_start_point(targetNode);
    TSPoint exprEnd = ts_node_end_point(targetNode);
    const analysis::Scope* scope = FindScopeByLineOrRoot(rootScope.get(), exprStart.row, exprStart.column);

    std::string varType =
        analysis::ResolveExpressionType(targetNode, {scope, request.symbolTable, request.sourceCode, request.uri});
    if (varType.empty() || varType == "null")
    {
        varType = "auto";
    }

    std::string varName = "newVar";
    std::string exprText = GetNodeText(targetNode, request.sourceCode);
    if (exprText.empty())
    {
        return std::nullopt;
    }

    uint32_t stmtRow = ts_node_start_point(stmtNode).row;
    std::string indent = GetLineIndentation(request.sourceCode, stmtRow);

    lsp::TextEdit declEdit;
    declEdit.range = lsp::Range{{stmtRow, 0}, {stmtRow, 0}};
    declEdit.newText = indent + varType + " " + varName + " = " + exprText + ";\n";

    lsp::TextEdit replEdit;
    replEdit.range = lsp::Range{{exprStart.row, exprStart.column}, {exprEnd.row, exprEnd.column}};
    replEdit.newText = varName;

    lsp::CodeAction action;
    action.title = "Extract Variable";
    action.kind = lsp::CodeActionKindEnum(lsp::CodeActionKind::RefactorExtract);

    lsp::WorkspaceEdit wsEdit;
    lsp::Map<lsp::DocumentUri, std::vector<lsp::TextEdit>> changes;
    changes[lsp::DocumentUri::parse(request.uri)] = {std::move(declEdit), std::move(replEdit)};
    wsEdit.changes = std::move(changes);
    action.edit = std::move(wsEdit);

    return action;
}

/**
 * @brief Tries to generate an Extract Variable refactoring code action.
 * @param[in] request Code action request context.
 * @param[in] rootNode Root AST node.
 * @param[out] actions Destination actions vector.
 */
void TryAddExtractVariableAction(const CodeActionRequest& request, TSNode rootNode,
                                 std::vector<lsp::CodeAction>& actions)
{
    if (ts_node_is_null(rootNode) || request.sourceCode.empty())
    {
        return;
    }
    TSNode targetNode = FindExtractableExpression(rootNode, request.range);
    if (ts_node_is_null(targetNode))
    {
        return;
    }
    TSNode stmtNode = FindEnclosingBlockStatement(targetNode);
    if (ts_node_is_null(stmtNode))
    {
        return;
    }
    if (auto action = BuildExtractVariableAction(request, targetNode, stmtNode))
    {
        actions.push_back(std::move(*action));
    }
}

struct ExtractMethodStatements
{
    TSNode fnNode;
    TSNode classNode;
    std::vector<TSNode> selectedStmts;
    uint32_t startByte = 0;
    uint32_t endByte = 0;
    TSPoint firstStart = {0, 0};
    TSPoint lastEnd = {0, 0};
    std::string selectedCode;
};

struct VarInfo
{
    std::string name;
    std::string typeName;
    bool declaredInside = false;
};

struct ExtractedMethodVariables
{
    std::vector<VarInfo> inputParams;
    std::vector<VarInfo> outputVars;
};

/**
 * @brief Represents deduced signature and edits for extracted method.
 */
struct ExtractedMethodPlan
{
    std::string returnType;
    std::string paramsStr;
    std::string argsStr;
    std::string callSiteText;
    std::string extractedBody;
};

/**
 * @brief Context for discovering method outputs.
 */
struct MethodOutputContext
{
    const ExtractMethodStatements& stmts;
    const analysis::Scope* fnScope;
    const analysis::Scope* stmtScope;
};

/**
 * @brief Finds the enclosing function node for a selection range.
 * @param[in] rootNode AST root node.
 * @param[in] range Selection range.
 * @return Function declaration node or null node.
 */
TSNode FindEnclosingFunctionNode(TSNode rootNode, const lsp::Range& range)
{
    TSPoint startPt = {range.start.line, range.start.character};
    TSPoint endPt = {range.end.line, range.end.character};
    TSNode selNode = ts_node_descendant_for_point_range(rootNode, startPt, endPt);
    TSNode fnNode = selNode;
    while (!ts_node_is_null(fnNode) && std::string_view(ts_node_type(fnNode)) != "func_declaration")
    {
        fnNode = ts_node_parent(fnNode);
    }
    return fnNode;
}

/**
 * @brief Finds the statement block body of a function node.
 * @param[in] fnNode Function declaration node.
 * @return Statement block node or null node.
 */
TSNode FindFunctionBodyBlock(TSNode fnNode)
{
    TSNode bodyNode = parser::GetChildByField(fnNode, parser::fields::Body);
    if (!ts_node_is_null(bodyNode))
    {
        return bodyNode;
    }
    uint32_t cnt = ts_node_child_count(fnNode);
    for (uint32_t i = 0; i < cnt; ++i)
    {
        TSNode ch = ts_node_child(fnNode, i);
        if (std::string_view(ts_node_type(ch)) == "statement_block")
        {
            return ch;
        }
    }
    return bodyNode;
}

/**
 * @brief Collects statements within a block that intersect the selection range.
 * @param[in] bodyNode Statement block node.
 * @param[in] range Selection range.
 * @return Vector of intersecting statement nodes.
 */
std::vector<TSNode> CollectStatementsInRange(TSNode bodyNode, const lsp::Range& range)
{
    std::vector<TSNode> selectedStmts;
    uint32_t childCount = ts_node_child_count(bodyNode);
    for (uint32_t i = 0; i < childCount; ++i)
    {
        TSNode ch = ts_node_child(bodyNode, i);
        if (!ts_node_is_named(ch))
        {
            continue;
        }
        std::string_view t = ts_node_type(ch);
        if (t == "{" || t == "}")
        {
            continue;
        }
        TSPoint cStart = ts_node_start_point(ch);
        TSPoint cEnd = ts_node_end_point(ch);
        if (cStart.row <= range.end.line && cEnd.row >= range.start.line)
        {
            selectedStmts.push_back(ch);
        }
    }
    return selectedStmts;
}

/**
 * @brief Finds the statement sequence selected for method extraction.
 * @param[in] rootNode AST root node.
 * @param[in] request Code action request.
 * @return Extracted statement metadata or std::nullopt.
 */
std::optional<ExtractMethodStatements> FindSelectedStatements(TSNode rootNode, const CodeActionRequest& request)
{
    if (ts_node_is_null(rootNode) || request.sourceCode.empty())
    {
        return std::nullopt;
    }
    TSNode fnNode = FindEnclosingFunctionNode(rootNode, request.range);
    if (ts_node_is_null(fnNode))
    {
        return std::nullopt;
    }
    TSNode bodyNode = FindFunctionBodyBlock(fnNode);
    if (ts_node_is_null(bodyNode))
    {
        return std::nullopt;
    }

    std::vector<TSNode> selectedStmts = CollectStatementsInRange(bodyNode, request.range);
    if (selectedStmts.empty())
    {
        return std::nullopt;
    }

    TSNode firstStmt = selectedStmts.front();
    TSNode lastStmt = selectedStmts.back();
    uint32_t startByte = ts_node_start_byte(firstStmt);
    uint32_t endByte = ts_node_end_byte(lastStmt);
    if (startByte >= request.sourceCode.size() || endByte > request.sourceCode.size() || startByte >= endByte)
    {
        return std::nullopt;
    }

    TSNode classNode = fnNode;
    while (!ts_node_is_null(classNode) && std::string_view(ts_node_type(classNode)) != "class_declaration")
    {
        classNode = ts_node_parent(classNode);
    }

    ExtractMethodStatements result;
    result.fnNode = fnNode;
    result.classNode = classNode;
    result.selectedStmts = std::move(selectedStmts);
    result.startByte = startByte;
    result.endByte = endByte;
    result.firstStart = ts_node_start_point(firstStmt);
    result.lastEnd = ts_node_end_point(lastStmt);
    result.selectedCode = request.sourceCode.substr(startByte, endByte - startByte);
    return result;
}

/**
 * @brief Validates if a referenced symbol is an outer input parameter or local variable.
 * @param[in] ref Scope reference.
 * @param[in] sc Enclosing scope.
 * @param[in] stmts Selected statement information.
 * @param[out] outInfo Discovered VarInfo.
 * @return True if reference is a valid extracted method input.
 */
bool IsValidMethodInputRef(const analysis::LocalReference& ref, const analysis::Scope* sc,
                           const ExtractMethodStatements& stmts, VarInfo& outInfo)
{
    if (ref.isMemberAccess || ref.startLine < stmts.firstStart.row || ref.endLine > stmts.lastEnd.row)
    {
        return false;
    }
    const analysis::LocalDefinition* def = analysis::ResolveInScope(sc, ref.name);
    if (!def)
    {
        return false;
    }
    if (!ts_node_is_null(stmts.classNode) && def->kind == analysis::LocalDefinitionKind::Field)
    {
        return false;
    }
    if (def->kind != analysis::LocalDefinitionKind::Variable && def->kind != analysis::LocalDefinitionKind::Parameter)
    {
        return false;
    }
    bool declaredBefore = (def->endLine < stmts.firstStart.row ||
                           (def->endLine == stmts.firstStart.row && def->endCharacter <= stmts.firstStart.column) ||
                           def->kind == analysis::LocalDefinitionKind::Parameter);
    if (!declaredBefore)
    {
        return false;
    }
    std::string tName = def->typeName.empty() ? "auto" : def->typeName;
    outInfo = {def->name, std::move(tName), false};
    return true;
}

/**
 * @brief Collects input variables referenced within extracted statements.
 * @param[in] stmts Selected statement information.
 * @param[in] fnScope Enclosing function scope.
 * @return Vector of input parameter definitions.
 */
std::vector<VarInfo> CollectMethodInputs(const ExtractMethodStatements& stmts, const analysis::Scope* fnScope)
{
    std::vector<VarInfo> inputParams;
    ankerl::unordered_dense::set<std::string> seenInputs;

    std::vector<const analysis::Scope*> scopeWorklist = {fnScope};
    while (!scopeWorklist.empty())
    {
        const analysis::Scope* sc = scopeWorklist.back();
        scopeWorklist.pop_back();
        if (!sc)
        {
            continue;
        }
        for (const auto& ref : sc->references)
        {
            VarInfo info;
            if (IsValidMethodInputRef(ref, sc, stmts, info) && !seenInputs.contains(info.name))
            {
                seenInputs.insert(info.name);
                inputParams.push_back(std::move(info));
            }
        }
        for (const auto& child : sc->children)
        {
            scopeWorklist.push_back(child.get());
        }
    }
    return inputParams;
}

/**
 * @brief Unwraps parenthesized expression and extracts identifier or single-token scoped identifier.
 * @param[in] node AST node to unwrap.
 * @param[in] sourceCode Source text.
 * @return Extracted identifier text, or empty string.
 */
std::string UnwrapIdentifierName(TSNode node, std::string_view sourceCode)
{
    TSNode curr = node;
    while (!ts_node_is_null(curr) && std::string_view(ts_node_type(curr)) == "parenthesized_expression")
    {
        if (ts_node_named_child_count(curr) > 0)
        {
            curr = ts_node_named_child(curr, 0);
        }
        else
        {
            break;
        }
    }
    if (ts_node_is_null(curr))
    {
        return "";
    }
    std::string_view lType = ts_node_type(curr);
    if (lType == "identifier")
    {
        return GetNodeText(curr, sourceCode);
    }
    if (lType == "scoped_identifier" && ts_node_named_child_count(curr) == 1)
    {
        return GetNodeText(ts_node_named_child(curr, 0), sourceCode);
    }
    return "";
}

/**
 * @brief Checks if an assignment expression mutates a target variable.
 * @param[in] curr Assignment expression node.
 * @param[in] sourceCode Source text.
 * @param[out] mutatedVars Set of recorded mutated variable names.
 */
void CheckAssignmentMutation(TSNode curr, std::string_view sourceCode,
                             ankerl::unordered_dense::set<std::string>& mutatedVars)
{
    TSNode left = parser::GetChildByField(curr, parser::fields::Left);
    if (ts_node_is_null(left) && ts_node_child_count(curr) > 0)
    {
        left = ts_node_child(curr, 0);
    }
    std::string varName = UnwrapIdentifierName(left, sourceCode);
    if (!varName.empty())
    {
        mutatedVars.insert(varName);
    }
}

/**
 * @brief Finds the target operand of an increment or decrement expression.
 * @param[in] curr Unary or postfix expression node.
 * @param[in] sourceCode Source text.
 * @param[out] targetArg Target operand node if found.
 * @return True if expression is increment or decrement and operand was found.
 */
bool FindIncDecTarget(TSNode curr, std::string_view sourceCode, TSNode& targetArg)
{
    TSNode opNode = parser::GetChildByField(curr, parser::fields::Operator);
    std::string op = !ts_node_is_null(opNode) ? GetNodeText(opNode, sourceCode) : "";
    bool isIncDec = (op == "++" || op == "--");
    targetArg = parser::GetChildByField(curr, parser::fields::Operand);
    if (!isIncDec || ts_node_is_null(targetArg))
    {
        uint32_t cnt = ts_node_child_count(curr);
        for (uint32_t i = 0; i < cnt; ++i)
        {
            TSNode ch = ts_node_child(curr, i);
            std::string chText = GetNodeText(ch, sourceCode);
            if (chText == "++" || chText == "--")
            {
                isIncDec = true;
            }
            else if (ts_node_is_named(ch))
            {
                targetArg = ch;
            }
        }
    }
    return isIncDec && !ts_node_is_null(targetArg);
}

/**
 * @brief Checks if a postfix/unary expression increments or decrements a variable.
 * @param[in] curr Unary or postfix expression node.
 * @param[in] sourceCode Source text.
 * @param[out] mutatedVars Set of recorded mutated variable names.
 */
void CheckIncDecMutation(TSNode curr, std::string_view sourceCode,
                         ankerl::unordered_dense::set<std::string>& mutatedVars)
{
    TSNode targetArg{};
    if (FindIncDecTarget(curr, sourceCode, targetArg))
    {
        std::string varName = UnwrapIdentifierName(targetArg, sourceCode);
        if (!varName.empty())
        {
            mutatedVars.insert(varName);
        }
    }
}

/**
 * @brief Checks if a function call mutates arguments passed with out/inout qualifiers.
 * @param[in] curr Call expression node.
 * @param[in] sourceCode Source text.
 * @param[out] mutatedVars Set of recorded mutated variable names.
 */
void CheckCallMutation(TSNode curr, std::string_view sourceCode, ankerl::unordered_dense::set<std::string>& mutatedVars)
{
    TSNode argsNode = parser::GetChildByField(curr, parser::fields::Arguments);
    if (ts_node_is_null(argsNode))
    {
        return;
    }
    uint32_t argCnt = ts_node_named_child_count(argsNode);
    for (uint32_t i = 0; i < argCnt; ++i)
    {
        TSNode arg = ts_node_named_child(argsNode, i);
        std::string argText = GetNodeText(arg, sourceCode);
        if (argText.starts_with("&out ") || argText.starts_with("&inout ") || argText.starts_with("out ") ||
            argText.starts_with("inout "))
        {
            TSNode idNode = parser::GetChildByField(arg, parser::fields::Name);
            if (ts_node_is_null(idNode) && ts_node_named_child_count(arg) > 0)
            {
                idNode = ts_node_named_child(arg, ts_node_named_child_count(arg) - 1);
            }
            if (!ts_node_is_null(idNode))
            {
                std::string varName = GetNodeText(idNode, sourceCode);
                if (!varName.empty())
                {
                    mutatedVars.insert(varName);
                }
            }
        }
    }
}

/**
 * @brief Checks if an AST node mutates a variable via assignment, inc/dec, or out parameter.
 * @param[in] curr AST node.
 * @param[in] sourceCode Source text.
 * @param[out] mutatedVars Set of recorded mutated variable names.
 */
void CheckNodeMutations(TSNode curr, std::string_view sourceCode,
                        ankerl::unordered_dense::set<std::string>& mutatedVars)
{
    std::string_view type = ts_node_type(curr);
    if (type == "assignment_expression")
    {
        CheckAssignmentMutation(curr, sourceCode, mutatedVars);
    }
    else if (type == "postfix_expression" || type == "unary_expression")
    {
        CheckIncDecMutation(curr, sourceCode, mutatedVars);
    }
    else if (type == "call_expression")
    {
        CheckCallMutation(curr, sourceCode, mutatedVars);
    }
}

/**
 * @brief Collects all variables mutated within the selected statements.
 * @param[in] selectedStmts Statement node list.
 * @param[in] sourceCode Source text.
 * @return Set of mutated variable names.
 */
ankerl::unordered_dense::set<std::string> CollectMutatedVariables(const std::vector<TSNode>& selectedStmts,
                                                                  std::string_view sourceCode)
{
    ankerl::unordered_dense::set<std::string> mutatedVars;
    for (const auto& stmt : selectedStmts)
    {
        std::vector<TSNode> stack = {stmt};
        while (!stack.empty())
        {
            TSNode curr = stack.back();
            stack.pop_back();
            CheckNodeMutations(curr, sourceCode, mutatedVars);
            uint32_t nodeChildCount = ts_node_child_count(curr);
            for (uint32_t i = 0; i < nodeChildCount; ++i)
            {
                stack.push_back(ts_node_child(curr, i));
            }
        }
    }
    return mutatedVars;
}

/**
 * @brief Checks if a variable has a reference occurring after the selection end point.
 * @param[in] fnScope Enclosing function scope.
 * @param[in] name Variable identifier name.
 * @param[in] lastEnd End point of selection.
 * @return True if variable is referenced after selection.
 */
bool IsVariableUsedAfter(const analysis::Scope* fnScope, const std::string& name, TSPoint lastEnd)
{
    std::vector<const analysis::Scope*> worklist = {fnScope};
    while (!worklist.empty())
    {
        const analysis::Scope* s = worklist.back();
        worklist.pop_back();
        if (!s)
        {
            continue;
        }
        for (const auto& r : s->references)
        {
            if (!r.isMemberAccess && r.name == name)
            {
                if (r.startLine > lastEnd.row || (r.startLine == lastEnd.row && r.startCharacter >= lastEnd.column))
                {
                    return true;
                }
            }
        }
        for (const auto& c : s->children)
        {
            worklist.push_back(c.get());
        }
    }
    return false;
}

/**
 * @brief Finds a local definition in the scope subtree by variable name.
 * @param[in] root Root scope.
 * @param[in] name Variable identifier name.
 * @return Matching LocalDefinition pointer or nullptr.
 */
const analysis::LocalDefinition* FindDefinitionInScopeTree(const analysis::Scope* root, const std::string& name)
{
    std::vector<const analysis::Scope*> worklist = {root};
    while (!worklist.empty())
    {
        const analysis::Scope* s = worklist.back();
        worklist.pop_back();
        if (!s)
        {
            continue;
        }
        for (const auto& d : s->definitions)
        {
            if (d.name == name)
            {
                return &d;
            }
        }
        for (const auto& c : s->children)
        {
            worklist.push_back(c.get());
        }
    }
    return nullptr;
}

/**
 * @brief Resolves a variable definition across statement scope, function scope, and child scopes.
 * @param[in] ctx Method output context.
 * @param[in] name Variable identifier name.
 * @return Found LocalDefinition pointer, or nullptr.
 */
const analysis::LocalDefinition* FindVariableDefinition(const MethodOutputContext& ctx, const std::string& name)
{
    if (ctx.stmtScope)
    {
        if (const auto* def = analysis::ResolveInScope(ctx.stmtScope, name))
        {
            return def;
        }
    }
    if (ctx.fnScope)
    {
        if (const auto* def = analysis::ResolveInScope(ctx.fnScope, name))
        {
            return def;
        }
        return FindDefinitionInScopeTree(ctx.fnScope, name);
    }
    return nullptr;
}

/**
 * @brief Checks if a variable definition was declared before extraction and referenced after it.
 * @param[in] ctx Method output context.
 * @param[in] def Target local definition.
 * @return True if definition represents an output variable.
 */
bool IsValidMutatedOutput(const MethodOutputContext& ctx, const analysis::LocalDefinition* def)
{
    if (!def)
    {
        return false;
    }
    if (!ts_node_is_null(ctx.stmts.classNode) && def->kind == analysis::LocalDefinitionKind::Field)
    {
        return false;
    }
    if (def->kind != analysis::LocalDefinitionKind::Variable && def->kind != analysis::LocalDefinitionKind::Parameter)
    {
        return false;
    }
    bool declaredBefore =
        (def->endLine < ctx.stmts.firstStart.row ||
         (def->endLine == ctx.stmts.firstStart.row && def->endCharacter <= ctx.stmts.firstStart.column) ||
         def->kind == analysis::LocalDefinitionKind::Parameter);
    return declaredBefore && IsVariableUsedAfter(ctx.fnScope, def->name, ctx.stmts.lastEnd);
}

/**
 * @brief Collects mutated variables declared prior to extraction that are used afterwards.
 * @param[in] ctx Method output extraction context.
 * @param[in] mutatedVars Set of mutated variable names.
 * @param[in,out] seenOutputs Set of deduplicated output variable names.
 * @param[out] outputVars Output variable collection.
 */
void CollectMutatedOutputs(const MethodOutputContext& ctx, const ankerl::unordered_dense::set<std::string>& mutatedVars,
                           ankerl::unordered_dense::set<std::string>& seenOutputs, std::vector<VarInfo>& outputVars)
{
    for (const auto& mName : mutatedVars)
    {
        const auto* def = FindVariableDefinition(ctx, mName);
        if (IsValidMutatedOutput(ctx, def) && !seenOutputs.contains(def->name))
        {
            seenOutputs.insert(def->name);
            std::string tName = def->typeName.empty() ? "auto" : def->typeName;
            outputVars.push_back({def->name, std::move(tName), false});
        }
    }
}

/**
 * @brief Checks if a variable has a reference occurring strictly after the specified line number.
 * @param[in] fnScope Enclosing function scope.
 * @param[in] name Variable identifier name.
 * @param[in] line 0-based source line index.
 * @return True if variable is referenced after line.
 */
bool IsVariableUsedAfterLine(const analysis::Scope* fnScope, const std::string& name, uint32_t line)
{
    std::vector<const analysis::Scope*> worklist = {fnScope};
    while (!worklist.empty())
    {
        const analysis::Scope* s = worklist.back();
        worklist.pop_back();
        if (!s)
        {
            continue;
        }
        for (const auto& r : s->references)
        {
            if (!r.isMemberAccess && r.name == name && r.startLine > line)
            {
                return true;
            }
        }
        for (const auto& c : s->children)
        {
            worklist.push_back(c.get());
        }
    }
    return false;
}

/**
 * @brief Collects definitions declared inside the extracted block that are used after it.
 * @param[in] stmts Selected statement metadata.
 * @param[in] fnScope Enclosing function scope.
 * @param[in,out] seenOutputs Set of deduplicated output variable names.
 * @param[out] outputVars Output variable collection.
 */
void CollectInternalDefinitionsUsedAfter(const ExtractMethodStatements& stmts, const analysis::Scope* fnScope,
                                         ankerl::unordered_dense::set<std::string>& seenOutputs,
                                         std::vector<VarInfo>& outputVars)
{
    std::vector<const analysis::Scope*> worklist = {fnScope};
    while (!worklist.empty())
    {
        const analysis::Scope* sc = worklist.back();
        worklist.pop_back();
        if (!sc)
        {
            continue;
        }
        for (const auto& def : sc->definitions)
        {
            if (def.startLine >= stmts.firstStart.row && def.endLine <= stmts.lastEnd.row)
            {
                if (IsVariableUsedAfterLine(fnScope, def.name, stmts.lastEnd.row) && !seenOutputs.contains(def.name))
                {
                    seenOutputs.insert(def.name);
                    std::string tName = def.typeName.empty() ? "auto" : def.typeName;
                    outputVars.push_back({def.name, std::move(tName), true});
                }
            }
        }
        for (const auto& child : sc->children)
        {
            worklist.push_back(child.get());
        }
    }
}

/**
 * @brief Collects output variables that must be returned or passed out from the extracted method.
 * @param[in] stmts Selected statement information.
 * @param[in] fnScope Enclosing function scope.
 * @param[in] mutatedVars Set of mutated variable names.
 * @param[in] stmtScope Scope of first statement.
 * @return Vector of output variable definitions.
 */
std::vector<VarInfo> CollectMethodOutputs(const ExtractMethodStatements& stmts, const analysis::Scope* fnScope,
                                          const ankerl::unordered_dense::set<std::string>& mutatedVars,
                                          const analysis::Scope* stmtScope)
{
    std::vector<VarInfo> outputVars;
    ankerl::unordered_dense::set<std::string> seenOutputs;

    MethodOutputContext ctx{stmts, fnScope, stmtScope};
    CollectMutatedOutputs(ctx, mutatedVars, seenOutputs, outputVars);
    CollectInternalDefinitionsUsedAfter(stmts, fnScope, seenOutputs, outputVars);

    return outputVars;
}

/**
 * @brief Constructs the input parameter list and invocation arguments string.
 * @param[in] inputParams Discovered input parameters.
 * @param[in] outputVars Discovered output variables.
 * @param[out] paramsStr Constructed parameter list string.
 * @param[out] argsStr Constructed argument list string.
 */
void BuildMethodParamsAndArgs(const std::vector<VarInfo>& inputParams, const std::vector<VarInfo>& outputVars,
                              std::string& paramsStr, std::string& argsStr)
{
    std::vector<VarInfo> effectiveInputs;
    for (const auto& inp : inputParams)
    {
        bool isOutParam = false;
        for (size_t k = 1; k < outputVars.size(); ++k)
        {
            if (outputVars[k].name == inp.name)
            {
                isOutParam = true;
                break;
            }
        }
        if (!isOutParam)
        {
            effectiveInputs.push_back(inp);
        }
    }

    for (size_t i = 0; i < effectiveInputs.size(); ++i)
    {
        if (i > 0)
        {
            paramsStr += ", ";
            argsStr += ", ";
        }
        paramsStr += effectiveInputs[i].typeName + " " + effectiveInputs[i].name;
        argsStr += effectiveInputs[i].name;
    }
}

/**
 * @brief Deduces method return type and call site statement for single or multiple outputs.
 * @param[in] vars Discovered input and output variables.
 * @param[in] stmts Statement extraction metadata.
 * @param[in] request Code action request.
 * @param[out] plan In-progress extracted method plan.
 */
void DeduceReturnAndCallSite(const ExtractedMethodVariables& vars, const ExtractMethodStatements& stmts,
                             const CodeActionRequest& request, ExtractedMethodPlan& plan)
{
    const std::string methodName = "NewMethod";
    plan.returnType = "void";

    if (vars.outputVars.size() == 1)
    {
        plan.returnType = vars.outputVars[0].typeName;
        if (plan.extractedBody.find("return " + vars.outputVars[0].name) == std::string::npos &&
            !plan.extractedBody.ends_with("return " + vars.outputVars[0].name + ";"))
        {
            plan.extractedBody += "\n    return " + vars.outputVars[0].name + ";";
        }
        if (vars.outputVars[0].declaredInside)
        {
            plan.callSiteText =
                plan.returnType + " " + vars.outputVars[0].name + " = " + methodName + "(" + plan.argsStr + ");";
        }
        else
        {
            plan.callSiteText = vars.outputVars[0].name + " = " + methodName + "(" + plan.argsStr + ");";
        }
    }
    else if (vars.outputVars.empty())
    {
        TSNode lastNode = stmts.selectedStmts.back();
        if (std::string_view(ts_node_type(lastNode)) == "return_statement")
        {
            TSNode retTypeNode = parser::GetChildByField(stmts.fnNode, parser::fields::Type);
            if (!ts_node_is_null(retTypeNode))
            {
                plan.returnType = GetNodeText(retTypeNode, request.sourceCode);
            }
        }
        plan.callSiteText = methodName + "(" + plan.argsStr + ");";
    }
    else
    {
        plan.returnType = vars.outputVars[0].typeName;
        for (size_t i = 1; i < vars.outputVars.size(); ++i)
        {
            if (!plan.paramsStr.empty())
                plan.paramsStr += ", ";
            if (!plan.argsStr.empty())
                plan.argsStr += ", ";
            plan.paramsStr += vars.outputVars[i].typeName + " &out " + vars.outputVars[i].name;
            plan.argsStr += vars.outputVars[i].name;
        }
        if (plan.extractedBody.find("return " + vars.outputVars[0].name) == std::string::npos &&
            !plan.extractedBody.ends_with("return " + vars.outputVars[0].name + ";"))
        {
            plan.extractedBody += "\n    return " + vars.outputVars[0].name + ";";
        }
        if (vars.outputVars[0].declaredInside)
        {
            plan.callSiteText =
                plan.returnType + " " + vars.outputVars[0].name + " = " + methodName + "(" + plan.argsStr + ");";
        }
        else
        {
            plan.callSiteText = vars.outputVars[0].name + " = " + methodName + "(" + plan.argsStr + ");";
        }
    }
}

/**
 * @brief Computes extracted method signature, parameters, and invocation statement.
 * @param[in] vars Discovered input and output variables.
 * @param[in] stmts Statement extraction metadata.
 * @param[in] request Code action request.
 * @return Computed ExtractedMethodPlan.
 */
ExtractedMethodPlan DeduceExtractedMethodSignature(const ExtractedMethodVariables& vars,
                                                   const ExtractMethodStatements& stmts,
                                                   const CodeActionRequest& request)
{
    ExtractedMethodPlan plan;
    plan.extractedBody = stmts.selectedCode;
    BuildMethodParamsAndArgs(vars.inputParams, vars.outputVars, plan.paramsStr, plan.argsStr);
    DeduceReturnAndCallSite(vars, stmts, request, plan);
    return plan;
}

/**
 * @brief Finds the insertion point for adding a method definition inside a class.
 * @param[in] classNode Class declaration node.
 * @return Insertion point.
 */
TSPoint FindClassBodyInsertionPoint(TSNode classNode)
{
    TSNode classBody = parser::GetChildByField(classNode, parser::fields::Body);
    if (ts_node_is_null(classBody))
    {
        uint32_t cnt = ts_node_child_count(classNode);
        for (uint32_t i = 0; i < cnt; ++i)
        {
            TSNode ch = ts_node_child(classNode, i);
            if (std::string_view(ts_node_type(ch)) == "class_body")
            {
                classBody = ch;
                break;
            }
        }
    }

    if (!ts_node_is_null(classBody))
    {
        uint32_t cnt = ts_node_child_count(classBody);
        for (int i = static_cast<int>(cnt) - 1; i >= 0; --i)
        {
            TSNode ch = ts_node_child(classBody, static_cast<uint32_t>(i));
            if (std::string_view(ts_node_type(ch)) == "}")
            {
                return ts_node_start_point(ch);
            }
        }
    }
    return TSPoint{0, 0};
}

/**
 * @brief Constructs the workspace edits for the new extracted method and invocation site.
 * @param[in] request Code action request.
 * @param[in] stmts Extracted statement metadata.
 * @param[in] plan Extracted method plan.
 * @return Constructed WorkspaceEdit.
 */
lsp::WorkspaceEdit BuildExtractMethodEdits(const CodeActionRequest& request, const ExtractMethodStatements& stmts,
                                           const ExtractedMethodPlan& plan)
{
    const std::string methodName = "NewMethod";
    lsp::TextEdit methodDefEdit;

    if (!ts_node_is_null(stmts.classNode))
    {
        TSPoint insertPt = FindClassBodyInsertionPoint(stmts.classNode);
        std::string methodCode = "\n    " + plan.returnType + " " + methodName + "(" + plan.paramsStr +
                                 ")\n    {\n        " + plan.extractedBody + "\n    }\n";
        methodDefEdit.range = lsp::Range{{insertPt.row, insertPt.column}, {insertPt.row, insertPt.column}};
        methodDefEdit.newText = methodCode;
    }
    else
    {
        TSPoint fnEnd = ts_node_end_point(stmts.fnNode);
        std::string methodCode = "\n\n" + plan.returnType + " " + methodName + "(" + plan.paramsStr + ")\n{\n    " +
                                 plan.extractedBody + "\n}\n";
        methodDefEdit.range = lsp::Range{{fnEnd.row + 1, 0}, {fnEnd.row + 1, 0}};
        methodDefEdit.newText = methodCode;
    }

    lsp::TextEdit callEdit;
    callEdit.range =
        lsp::Range{{stmts.firstStart.row, stmts.firstStart.column}, {stmts.lastEnd.row, stmts.lastEnd.column}};
    callEdit.newText = plan.callSiteText;

    lsp::WorkspaceEdit wsEdit;
    lsp::Map<lsp::DocumentUri, std::vector<lsp::TextEdit>> changes;
    changes[lsp::DocumentUri::parse(request.uri)] = {std::move(callEdit), std::move(methodDefEdit)};
    wsEdit.changes = std::move(changes);
    return wsEdit;
}

/**
 * @brief Tries to generate an Extract Method refactoring code action.
 * @param[in] request Code action request context.
 * @param[in] rootNode Root AST node.
 * @param[out] actions Destination actions vector.
 */
void TryAddExtractMethodAction(const CodeActionRequest& request, TSNode rootNode, std::vector<lsp::CodeAction>& actions)
{
    auto stmts = FindSelectedStatements(rootNode, request);
    if (!stmts)
    {
        return;
    }

    auto rootScope = request.scopeIndex.GetRoot(request.uri);
    const analysis::Scope* fnScope = FindScopeByLineOrRoot(rootScope.get(), ts_node_start_point(stmts->fnNode).row,
                                                           ts_node_start_point(stmts->fnNode).column);
    const analysis::Scope* stmtScope =
        FindScopeByLineOrRoot(rootScope.get(), stmts->firstStart.row, stmts->firstStart.column);

    ExtractedMethodVariables vars;
    if (fnScope)
    {
        vars.inputParams = CollectMethodInputs(*stmts, fnScope);
        auto mutated = CollectMutatedVariables(stmts->selectedStmts, request.sourceCode);
        vars.outputVars = CollectMethodOutputs(*stmts, fnScope, mutated, stmtScope);
    }

    ExtractedMethodPlan plan = DeduceExtractedMethodSignature(vars, *stmts, request);

    lsp::CodeAction action;
    action.title = "Extract Method";
    action.kind = lsp::CodeActionKindEnum(lsp::CodeActionKind::RefactorExtract);
    action.edit = BuildExtractMethodEdits(request, *stmts, plan);
    actions.push_back(std::move(action));
}

/**
 * @brief Finds enclosing class declaration node and its class body node.
 * @param[in] leaf AST leaf at cursor position.
 * @param[in] sourceCode Source text.
 * @param[out] className Extracted class name.
 * @param[out] classBody Extracted class body node.
 * @return True if class declaration and body were located.
 */
bool FindEnclosingClassAndBody(TSNode leaf, std::string_view sourceCode, std::string& className, TSNode& classBody)
{
    TSNode classNode = leaf;
    while (!ts_node_is_null(classNode) && std::string_view(ts_node_type(classNode)) != "class_declaration")
    {
        classNode = ts_node_parent(classNode);
    }
    if (ts_node_is_null(classNode))
    {
        return false;
    }
    classBody = parser::GetChildByField(classNode, parser::fields::Body);
    if (ts_node_is_null(classBody))
    {
        uint32_t cnt = ts_node_child_count(classNode);
        for (uint32_t i = 0; i < cnt; ++i)
        {
            TSNode ch = ts_node_child(classNode, i);
            if (std::string_view(ts_node_type(ch)) == "class_body")
            {
                classBody = ch;
                break;
            }
        }
    }
    if (ts_node_is_null(classBody))
    {
        return false;
    }
    TSNode nameNode = parser::GetChildByField(classNode, parser::fields::Name);
    className = GetNodeText(nameNode, sourceCode);
    return true;
}

/**
 * @brief Collects candidate member fields for accessor generation.
 * @param[in] leaf AST node under cursor.
 * @param[in] classBody Class body AST node.
 * @param[in] request Code action request.
 * @param[in] className Name of enclosing class.
 * @return Vector of {fieldName, fieldType} pairs.
 */
std::vector<std::pair<std::string, std::string>> CollectFieldsForGetterSetter(TSNode leaf, TSNode classBody,
                                                                              const CodeActionRequest& request,
                                                                              const std::string& className)
{
    TSNode varDecl = leaf;
    while (!ts_node_is_null(varDecl) && std::string_view(ts_node_type(varDecl)) != "variable_declaration" &&
           varDecl.id != classBody.id)
    {
        varDecl = ts_node_parent(varDecl);
    }

    std::vector<std::pair<std::string, std::string>> fields;
    if (!ts_node_is_null(varDecl) && std::string_view(ts_node_type(varDecl)) == "variable_declaration")
    {
        TSNode typeNode = parser::GetChildByField(varDecl, parser::fields::VarType);
        if (ts_node_is_null(typeNode))
        {
            typeNode = parser::GetChildByField(varDecl, parser::fields::Type);
        }
        std::string fieldType = GetNodeText(typeNode, request.sourceCode);
        if (fieldType.empty())
        {
            fieldType = "int";
        }

        uint32_t dCnt = ts_node_child_count(varDecl);
        for (uint32_t i = 0; i < dCnt; ++i)
        {
            TSNode ch = ts_node_child(varDecl, i);
            if (std::string_view(ts_node_type(ch)) == "variable_declarator")
            {
                TSNode vNameNode = parser::GetChildByField(ch, parser::fields::Name);
                std::string fName = GetNodeText(vNameNode, request.sourceCode);
                if (!fName.empty())
                {
                    fields.push_back({fName, fieldType});
                }
            }
        }
    }
    else
    {
        request.symbolTable.ForEachSymbol(
            [&]([[maybe_unused]] const std::string& qualifiedName, const std::vector<analysis::Symbol>& symList)
            {
                for (const auto& sym : symList)
                {
                    if (sym.containerName == className &&
                        (sym.type == analysis::SymbolType::Variable || sym.type == analysis::SymbolType::Property))
                    {
                        fields.push_back({sym.name, sym.GetVariable().typeName});
                    }
                }
            });
    }
    return fields;
}

/**
 * @brief Finds the insertion point before the closing brace of a class body.
 * @param[in] classBody Class body node.
 * @return AST point preceding class closing brace.
 */
TSPoint FindClassClosingBracePoint(TSNode classBody)
{
    uint32_t cnt = ts_node_child_count(classBody);
    for (int i = static_cast<int>(cnt) - 1; i >= 0; --i)
    {
        TSNode ch = ts_node_child(classBody, static_cast<uint32_t>(i));
        if (std::string_view(ts_node_type(ch)) == "}")
        {
            return ts_node_start_point(ch);
        }
    }
    return TSPoint{0, 0};
}

/**
 * @brief Context for generating getter and setter accessors.
 */
struct GetterSetterContext
{
    const std::string& className;
    const std::pair<std::string, std::string>& field;
    TSPoint insertPt;
};

/**
 * @brief Creates a CodeAction inserting an accessor text edit into the class body.
 * @param[in] uri Document URI.
 * @param[in] title Action title.
 * @param[in] text Method code to insert.
 * @param[in] insertPt Class body insertion point.
 * @return Constructed CodeAction.
 */
lsp::CodeAction CreateAccessorAction(const std::string& uri, const std::string& title, const std::string& text,
                                     TSPoint insertPt)
{
    lsp::CodeAction action;
    action.title = title;
    action.kind = lsp::CodeActionKindEnum(lsp::CodeActionKind::QuickFix);
    lsp::TextEdit edit;
    edit.range = lsp::Range{{insertPt.row, insertPt.column}, {insertPt.row, insertPt.column}};
    edit.newText = "\n" + text;

    lsp::WorkspaceEdit wsEdit;
    lsp::Map<lsp::DocumentUri, std::vector<lsp::TextEdit>> changes;
    changes[lsp::DocumentUri::parse(uri)] = {std::move(edit)};
    wsEdit.changes = std::move(changes);
    action.edit = std::move(wsEdit);
    return action;
}

/**
 * @brief Checks if a class field already has getter and setter declarations.
 * @param[in] table Symbol table.
 * @param[in] className Name of enclosing class.
 * @param[in] propName Clean property name.
 * @return Pair of booleans {hasGetter, hasSetter}.
 */
std::pair<bool, bool> CheckFieldAccessors(const analysis::SymbolTable& table, const std::string& className,
                                          const std::string& propName)
{
    std::string getterName = "get_" + propName;
    std::string setterName = "set_" + propName;
    bool hasGetter = false;
    bool hasSetter = false;
    table.ForEachSymbol(
        [&]([[maybe_unused]] const std::string& qualifiedName, const std::vector<analysis::Symbol>& symList)
        {
            for (const auto& sym : symList)
            {
                if (sym.containerName == className && sym.type == analysis::SymbolType::Function)
                {
                    if (sym.name == getterName)
                    {
                        hasGetter = true;
                    }
                    if (sym.name == setterName)
                    {
                        hasSetter = true;
                    }
                }
            }
        });
    return {hasGetter, hasSetter};
}

/**
 * @brief Emits Getter, Setter, or combined Getter/Setter actions for a class field.
 * @param[in] request Code action request.
 * @param[in] ctx Getter and setter context.
 * @param[out] actions Destination code actions vector.
 */
void EmitGetterSetterActionsForField(const CodeActionRequest& request, const GetterSetterContext& ctx,
                                     std::vector<lsp::CodeAction>& actions)
{
    const auto& [fName, fType] = ctx.field;
    std::string propName = CleanPropertyName(fName);
    if (propName.empty())
    {
        return;
    }

    auto [hasGetter, hasSetter] = CheckFieldAccessors(request.symbolTable, ctx.className, propName);
    if (hasGetter && hasSetter)
    {
        return;
    }

    std::string cleanType = fType.empty() ? "int" : fType;
    bool isPassByValue = analysis::IsPrimitiveTypeName(cleanType) || cleanType.ends_with("@");
    std::string setterParamType = isPassByValue ? cleanType : ("const " + cleanType + " &in");

    std::string getterCode =
        "    " + cleanType + " get_" + propName + "() const\n    {\n        return " + fName + ";\n    }\n";
    std::string setterCode =
        "    void set_" + propName + "(" + setterParamType + " value)\n    {\n        " + fName + " = value;\n    }\n";

    if (!hasGetter)
    {
        actions.push_back(CreateAccessorAction(request.uri, "Generate Getter", getterCode, ctx.insertPt));
    }
    if (!hasSetter)
    {
        actions.push_back(CreateAccessorAction(request.uri, "Generate Setter", setterCode, ctx.insertPt));
    }
    if (!hasGetter && !hasSetter)
    {
        actions.push_back(CreateAccessorAction(request.uri, "Generate Getter and Setter",
                                               getterCode + "\n" + setterCode, ctx.insertPt));
    }
}

/**
 * @brief Tries to generate Getters and Setters code actions for class fields.
 * @param[in] request Code action request.
 * @param[in] rootNode AST root node.
 * @param[out] actions Destination actions vector.
 */
void TryAddGetterSetterActions(const CodeActionRequest& request, TSNode rootNode, std::vector<lsp::CodeAction>& actions)
{
    if (ts_node_is_null(rootNode) || request.sourceCode.empty())
    {
        return;
    }
    TSPoint pt = {request.range.start.line, request.range.start.character};
    TSNode leaf = ts_node_descendant_for_point_range(rootNode, pt, pt);
    if (ts_node_is_null(leaf))
    {
        return;
    }
    std::string className;
    TSNode classBody;
    if (!FindEnclosingClassAndBody(leaf, request.sourceCode, className, classBody))
    {
        return;
    }
    auto fields = CollectFieldsForGetterSetter(leaf, classBody, request, className);
    if (fields.empty())
    {
        return;
    }
    TSPoint insertPt = FindClassClosingBracePoint(classBody);
    for (const auto& field : fields)
    {
        EmitGetterSetterActionsForField(request, GetterSetterContext{className, field, insertPt}, actions);
    }
}

/**
 * @brief Checks if a diagnostic matches an expected error/warning code string.
 * @param[in] diag Diagnostic object.
 * @param[in] expectedCode Code string to match.
 * @return True if diagnostic code matches.
 */
bool MatchDiagnosticCode(const lsp::Diagnostic& diag, std::string_view expectedCode)
{
    if (!diag.code.has_value())
    {
        return false;
    }
    if (std::holds_alternative<lsp::String>(diag.code.value()))
    {
        return std::get<lsp::String>(diag.code.value()) == expectedCode;
    }
    return false;
}

/**
 * @brief Bounded Levenshtein edit distance between two strings.
 * @param[in] a First string.
 * @param[in] b Second string.
 * @param[in] limit Maximum search distance threshold.
 * @return Computed distance or limit + 1 if exceeded.
 */
size_t BoundedEditDistance(std::string_view a, std::string_view b, size_t limit)
{
    if (a.size() > b.size())
    {
        std::swap(a, b);
    }
    if (b.size() - a.size() > limit)
    {
        return limit + 1;
    }

    std::vector<size_t> previous(a.size() + 1);
    std::vector<size_t> current(a.size() + 1);
    for (size_t i = 0; i <= a.size(); ++i)
    {
        previous[i] = i;
    }

    for (size_t j = 1; j <= b.size(); ++j)
    {
        current[0] = j;
        size_t rowBest = current[0];
        for (size_t i = 1; i <= a.size(); ++i)
        {
            const size_t substitution = previous[i - 1] + (a[i - 1] == b[j - 1] ? 0 : 1);
            current[i] = std::min({substitution, previous[i] + 1, current[i - 1] + 1});
            rowBest = std::min(rowBest, current[i]);
        }

        if (rowBest > limit)
        {
            return limit + 1;
        }
        previous.swap(current);
    }

    return previous[a.size()];
}

/**
 * @brief Determines maximum allowed typo distance by identifier length.
 * @param[in] nameLength Length of identifier.
 * @return Allowed edit distance.
 */
size_t SuggestionLimit(size_t nameLength)
{
    if (nameLength < 3)
    {
        return 0;
    }
    if (nameLength < 5)
    {
        return 1;
    }
    if (nameLength < 8)
    {
        return 2;
    }
    return 3;
}

/**
 * @brief Converts ASCII string to lowercase for case-insensitive matching.
 * @param[in] text Input string slice.
 * @return Folded lowercase string.
 */
std::string FoldCase(std::string_view text)
{
    std::string folded(text);
    std::transform(folded.begin(), folded.end(), folded.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return folded;
}

/**
 * @brief Collects all local definition names visible at the given scope chain.
 * @param[in] scope Starting lexical scope.
 * @param[out] names Destination names vector.
 */
void CollectVisibleLocalNames(const analysis::Scope* scope, std::vector<std::string>& names)
{
    for (const analysis::Scope* walk = scope; walk != nullptr; walk = walk->parent)
    {
        for (const auto& def : walk->definitions)
        {
            names.push_back(def.name);
        }
    }
}

/**
 * @brief Tests if symbol type can appear where a type name is expected.
 * @param[in] type Symbol type.
 * @return True if symbol is class, interface, enum, typedef, or funcdef.
 */
bool IsTypeLikeSymbol(analysis::SymbolType type)
{
    return type == analysis::SymbolType::Class || type == analysis::SymbolType::Interface ||
           type == analysis::SymbolType::Enum || type == analysis::SymbolType::Typedef ||
           type == analysis::SymbolType::Funcdef;
}

struct TypoCandidateSuggestion
{
    std::string name;
    size_t distance = 0;
};

/**
 * @brief Collects candidate names for typo suggestions from local scopes and symbols.
 * @param[in] request Code action request.
 * @param[in] point Cursor position.
 * @param[in] isIdentifier True if unresolved target is an identifier.
 * @param[in] isType True if unresolved target is a type.
 * @return Vector of candidate names.
 */
std::vector<std::string> CollectTypoCandidates(const CodeActionRequest& request, TSPoint point, bool isIdentifier,
                                               bool isType)
{
    std::vector<std::string> candidates;
    if (isIdentifier)
    {
        auto rootScope = request.scopeIndex.GetRoot(request.uri);
        if (rootScope)
        {
            CollectVisibleLocalNames(FindScopeByLineOrRoot(rootScope.get(), point.row, point.column), candidates);
        }
    }
    request.symbolTable.ForEachSymbol(
        [&candidates, isType](const std::string& name, const std::vector<analysis::Symbol>& symbols)
        {
            if (isType && std::none_of(symbols.begin(), symbols.end(),
                                       [](const analysis::Symbol& sym) { return IsTypeLikeSymbol(sym.type); }))
            {
                return;
            }
            candidates.push_back(name);
        });
    return candidates;
}

/**
 * @brief Ranks typo candidate suggestions by edit distance.
 * @param[in] typed Typed identifier text.
 * @param[in] candidates Available candidate names.
 * @return Sorted vector of suggestions within distance limit.
 */
std::vector<TypoCandidateSuggestion> RankTypoSuggestions(const std::string& typed,
                                                         const std::vector<std::string>& candidates)
{
    const std::string typedFolded = FoldCase(typed);
    const size_t limit = SuggestionLimit(typed.size());
    std::vector<TypoCandidateSuggestion> ranked;
    ankerl::unordered_dense::set<std::string> seen;

    for (const auto& candidate : candidates)
    {
        if (candidate.empty() || candidate == typed || !seen.insert(candidate).second)
        {
            continue;
        }
        const size_t distance = BoundedEditDistance(typedFolded, FoldCase(candidate), limit);
        if (distance <= limit)
        {
            ranked.push_back({candidate, distance});
        }
    }

    std::sort(ranked.begin(), ranked.end(),
              [](const TypoCandidateSuggestion& a, const TypoCandidateSuggestion& b)
              {
                  if (a.distance != b.distance)
                  {
                      return a.distance < b.distance;
                  }
                  return a.name < b.name;
              });
    return ranked;
}

/**
 * @brief Context for emitting typo candidate quick-fix code actions.
 */
struct TypoFixContext
{
    const CodeActionRequest& request;
    const lsp::Diagnostic& diag;
    TSNode identifier;
};

/**
 * @brief Emits quick-fix code actions for typo suggestions.
 * @param[in] ctx Typo fix context.
 * @param[in] ranked Ranked suggestions list.
 * @param[out] actions Destination actions vector.
 */
void EmitTypoCodeActions(const TypoFixContext& ctx, const std::vector<TypoCandidateSuggestion>& ranked,
                         std::vector<lsp::CodeAction>& actions)
{
    const size_t offered = std::min<size_t>(ranked.size(), 3);
    const bool hasClearWinner = ranked.size() == 1 || ranked[0].distance < ranked[1].distance;

    for (size_t i = 0; i < offered; ++i)
    {
        lsp::CodeAction action;
        action.title = "Did you mean '" + ranked[i].name + "'?";
        action.kind = lsp::CodeActionKind::QuickFix;
        action.diagnostics = std::vector<lsp::Diagnostic>{ctx.diag};

        if (i == 0 && hasClearWinner)
        {
            action.isPreferred = true;
        }

        lsp::TextEdit edit;
        edit.range.start.line = ts_node_start_point(ctx.identifier).row;
        edit.range.start.character = ts_node_start_point(ctx.identifier).column;
        edit.range.end.line = ts_node_end_point(ctx.identifier).row;
        edit.range.end.character = ts_node_end_point(ctx.identifier).column;
        edit.newText = ranked[i].name;

        lsp::WorkspaceEdit wsEdit;
        lsp::Map<lsp::DocumentUri, std::vector<lsp::TextEdit>> changes;
        changes[lsp::DocumentUri::parse(ctx.request.uri)] = {std::move(edit)};
        wsEdit.changes = std::move(changes);
        action.edit = std::move(wsEdit);

        actions.push_back(std::move(action));
    }
}

/**
 * @brief Tries to generate typo fix suggestions for undefined identifiers or unknown types.
 * @param[in] request Code action request.
 * @param[in] rootNode Root AST node.
 * @param[out] actions Destination actions vector.
 */
void TryAddUndefinedIdentifierSuggestions(const CodeActionRequest& request, TSNode rootNode,
                                          std::vector<lsp::CodeAction>& actions)
{
    if (ts_node_is_null(rootNode) || request.sourceCode.empty())
    {
        return;
    }

    for (const auto& diag : request.context.diagnostics)
    {
        const bool isIdentifier = MatchDiagnosticCode(diag, diagnostics::codes::UndefinedIdentifier);
        const bool isType = MatchDiagnosticCode(diag, diagnostics::codes::UnknownType);
        if (!isIdentifier && !isType)
        {
            continue;
        }

        const TSPoint point = {diag.range.start.line, diag.range.start.character};
        TSNode identifier = ts_node_descendant_for_point_range(rootNode, point, point);
        if (ts_node_is_null(identifier))
        {
            continue;
        }

        const std::string typed = GetNodeText(identifier, request.sourceCode);
        if (typed.empty())
        {
            continue;
        }

        auto candidates = CollectTypoCandidates(request, point, isIdentifier, isType);
        auto ranked = RankTypoSuggestions(typed, candidates);
        if (!ranked.empty())
        {
            EmitTypoCodeActions(TypoFixContext{request, diag, identifier}, ranked, actions);
        }
    }
}

/**
 * @brief Removes the `@` modifier from a handle declared on a primitive type.
 * @param[in] request Code action request.
 * @param[in] rootNode Root AST node.
 * @param[out] actions Destination actions vector.
 */
void TryAddHandleOnPrimitiveFix(const CodeActionRequest& request, TSNode rootNode,
                                std::vector<lsp::CodeAction>& actions)
{
    if (ts_node_is_null(rootNode) || request.sourceCode.empty())
    {
        return;
    }

    for (const auto& diag : request.context.diagnostics)
    {
        if (!MatchDiagnosticCode(diag, diagnostics::codes::HandleOnPrimitive))
        {
            continue;
        }

        const std::string_view line = angel_lsp::utils::GetLine(request.sourceCode, diag.range.start.line);
        const size_t at = line.find('@', diag.range.start.character);
        if (at == std::string_view::npos || at >= diag.range.end.character)
        {
            continue;
        }

        lsp::TextEdit edit;
        edit.range.start.line = diag.range.start.line;
        edit.range.start.character = static_cast<uint32_t>(at);
        edit.range.end.line = diag.range.start.line;
        edit.range.end.character = static_cast<uint32_t>(at + 1);
        edit.newText = "";

        lsp::CodeAction action;
        action.title = "Remove '@' - a primitive has no handle type";
        action.kind = lsp::CodeActionKindEnum(lsp::CodeActionKind::QuickFix);
        action.isPreferred = true;
        action.diagnostics = std::vector<lsp::Diagnostic>{diag};

        lsp::WorkspaceEdit wsEdit;
        lsp::Map<lsp::DocumentUri, std::vector<lsp::TextEdit>> changes;
        changes[lsp::DocumentUri::parse(request.uri)] = {std::move(edit)};
        wsEdit.changes = std::move(changes);
        action.edit = std::move(wsEdit);

        actions.push_back(std::move(action));
    }
}

struct IncludeCandidate
{
    std::string spelling;
    size_t distance = 0;
};

/**
 * @brief Collects all indexed file URIs from the workspace symbol table.
 * @param[in] table Symbol table.
 * @return Set of known document URIs.
 */
ankerl::unordered_dense::set<std::string> CollectIndexedFileUris(const analysis::SymbolTable& table)
{
    ankerl::unordered_dense::set<std::string> indexedUris;
    table.ForEachSymbol(
        [&indexedUris]([[maybe_unused]] const std::string& qualifiedName, const std::vector<analysis::Symbol>& symbols)
        {
            for (const auto& sym : symbols)
            {
                if (!sym.fileUri.empty())
                {
                    indexedUris.insert(sym.fileUri);
                }
            }
        });
    return indexedUris;
}

/**
 * @brief Searches indexed document URIs for filenames similar to the unresolved include.
 * @param[in] includerPath Absolute filesystem path of current document.
 * @param[in] rawPath Unresolved include directive raw path.
 * @param[in] indexedUris Set of indexed file URIs.
 * @param[in] currentUri Current document URI.
 * @return Ranked candidates list sorted by edit distance.
 */
std::vector<IncludeCandidate> RankIncludeCandidates(const std::string& includerPath, const std::string& rawPath,
                                                    const ankerl::unordered_dense::set<std::string>& indexedUris,
                                                    const std::string& currentUri)
{
    const std::string typedName = std::filesystem::path(rawPath).filename().generic_string();
    if (typedName.empty())
    {
        return {};
    }

    const std::string typedFolded = FoldCase(typedName);
    const size_t limit = SuggestionLimit(typedName.size());
    std::vector<IncludeCandidate> ranked;
    ankerl::unordered_dense::set<std::string> seen;

    for (const std::string& candidateUri : indexedUris)
    {
        if (candidateUri == currentUri)
        {
            continue;
        }

        const std::string candidatePath = angel_lsp::utils::UriToPath(candidateUri);
        if (candidatePath.empty())
        {
            continue;
        }

        const std::string candidateName = std::filesystem::path(candidatePath).filename().generic_string();
        const size_t distance = BoundedEditDistance(typedFolded, FoldCase(candidateName), limit);
        if (distance > limit)
        {
            continue;
        }

        std::error_code relativeError;
        const std::filesystem::path relative = std::filesystem::relative(
            std::filesystem::path(candidatePath), std::filesystem::path(includerPath).parent_path(), relativeError);
        if (relativeError || relative.empty())
        {
            continue;
        }

        const std::string spelling = relative.generic_string();
        if (spelling == rawPath || !seen.insert(spelling).second)
        {
            continue;
        }

        ranked.push_back({spelling, distance});
    }

    std::sort(ranked.begin(), ranked.end(),
              [](const IncludeCandidate& a, const IncludeCandidate& b)
              {
                  if (a.distance != b.distance)
                  {
                      return a.distance < b.distance;
                  }
                  return a.spelling < b.spelling;
              });
    return ranked;
}

/**
 * @brief Context for unresolved include suggestions.
 */
struct IncludeFixContext
{
    const CodeActionRequest& request;
    const lsp::Diagnostic& diag;
    const angel_lsp::utils::IncludeDirective& directive;
};

/**
 * @brief Emits quick-fix code actions for unresolved include suggestions.
 * @param[in] ctx Include fix context.
 * @param[in] ranked Ranked include candidates.
 * @param[out] actions Destination actions vector.
 */
void EmitIncludeSuggestions(const IncludeFixContext& ctx, const std::vector<IncludeCandidate>& ranked,
                            std::vector<lsp::CodeAction>& actions)
{
    const std::string_view line =
        angel_lsp::utils::GetLine(ctx.request.sourceCode, static_cast<uint32_t>(ctx.directive.line));
    const char open = ctx.directive.isAngled ? '<' : '"';
    const char close = ctx.directive.isAngled ? '>' : '"';
    const size_t openPos = line.find(open);
    if (openPos == std::string_view::npos)
    {
        return;
    }
    const size_t closePos = line.find(close, openPos + 1);
    if (closePos == std::string_view::npos)
    {
        return;
    }

    const size_t offered = std::min<size_t>(ranked.size(), 3);
    const bool hasClearWinner = ranked.size() == 1 || ranked[0].distance < ranked[1].distance;

    for (size_t i = 0; i < offered; ++i)
    {
        lsp::CodeAction action;
        action.title = "Did you mean '" + ranked[i].spelling + "'?";
        action.kind = lsp::CodeActionKindEnum(lsp::CodeActionKind::QuickFix);
        action.diagnostics = std::vector<lsp::Diagnostic>{ctx.diag};
        if (i == 0 && hasClearWinner)
        {
            action.isPreferred = true;
        }

        lsp::TextEdit edit;
        edit.range.start.line = static_cast<uint32_t>(ctx.directive.line);
        edit.range.start.character = static_cast<uint32_t>(openPos + 1);
        edit.range.end.line = static_cast<uint32_t>(ctx.directive.line);
        edit.range.end.character = static_cast<uint32_t>(closePos);
        edit.newText = ranked[i].spelling;

        lsp::WorkspaceEdit wsEdit;
        lsp::Map<lsp::DocumentUri, std::vector<lsp::TextEdit>> changes;
        changes[lsp::DocumentUri::parse(ctx.request.uri)] = {std::move(edit)};
        wsEdit.changes = std::move(changes);
        action.edit = std::move(wsEdit);

        actions.push_back(std::move(action));
    }
}

/**
 * @brief Suggests existing workspace files for unresolved `#include` paths.
 * @param[in] request Code action request.
 * @param[out] actions Destination actions vector.
 */
void TryAddUnresolvedIncludeSuggestions(const CodeActionRequest& request, std::vector<lsp::CodeAction>& actions)
{
    if (request.sourceCode.empty())
    {
        return;
    }
    const bool anyUnresolvedInclude =
        std::any_of(request.context.diagnostics.begin(), request.context.diagnostics.end(),
                    [](const lsp::Diagnostic& diag) { return MatchDiagnosticCode(diag, "as-warn-include-not-found"); });
    if (!anyUnresolvedInclude)
    {
        return;
    }
    const std::string includerPath = angel_lsp::utils::UriToPath(request.uri);
    if (includerPath.empty())
    {
        return;
    }

    auto indexedUris = CollectIndexedFileUris(request.symbolTable);
    const auto directives = angel_lsp::utils::IncludeResolver::ExtractIncludes(request.sourceCode);

    for (const auto& diag : request.context.diagnostics)
    {
        if (!MatchDiagnosticCode(diag, "as-warn-include-not-found"))
        {
            continue;
        }
        const auto directive = std::find_if(directives.begin(), directives.end(),
                                            [&diag](const angel_lsp::utils::IncludeDirective& candidate)
                                            { return candidate.line == diag.range.start.line; });
        if (directive == directives.end() || directive->rawPath.empty())
        {
            continue;
        }

        auto ranked = RankIncludeCandidates(includerPath, directive->rawPath, indexedUris, request.uri);
        if (!ranked.empty())
        {
            EmitIncludeSuggestions(IncludeFixContext{request, diag, *directive}, ranked, actions);
        }
    }
}

/**
 * @brief Locates a unique global function symbol by name.
 * @param[in] table Symbol table.
 * @param[in] functionName Name of global function.
 * @return Unique function symbol pointer or nullptr if ambiguous/missing.
 */
const analysis::Symbol* FindUniqueGlobalFunction(const analysis::SymbolTable& table, const std::string& functionName)
{
    const analysis::Symbol* target = nullptr;
    size_t globalOverloads = 0;
    table.ForEachSymbol(
        [&](const std::string& name, const std::vector<analysis::Symbol>& symbols)
        {
            if (name != functionName)
            {
                return;
            }
            for (const auto& sym : symbols)
            {
                if (sym.type == analysis::SymbolType::Function && sym.containerName.empty() &&
                    std::holds_alternative<analysis::FunctionSignature>(sym.signature))
                {
                    ++globalOverloads;
                    target = &sym;
                }
            }
        });
    return (target != nullptr && globalOverloads == 1) ? target : nullptr;
}

/**
 * @brief Formats a `funcdef` declaration string matching a function signature.
 * @param[in] signature Function signature.
 * @param[in] funcdefName Target funcdef type name.
 * @return Formatted funcdef declaration line.
 */
std::string FormatFuncdefDeclarationText(const analysis::FunctionSignature& signature, const std::string& funcdefName)
{
    std::string declaration = "funcdef ";
    declaration += signature.returnType.empty() ? "void" : signature.returnType;
    declaration += " " + funcdefName + "(";
    for (size_t i = 0; i < signature.parameters.size(); ++i)
    {
        if (i > 0)
        {
            declaration += ", ";
        }
        const auto& parameter = signature.parameters[i];
        declaration += parameter.rawText.empty() ? parameter.typeName : parameter.rawText;
    }
    declaration += ");\n";
    return declaration;
}

/**
 * @brief Item information for generating a funcdef quick-fix action.
 */
struct FuncdefFixItem
{
    TSNode typeNode;
    std::string functionName;
    std::string funcdefName;
    std::string declaration;
};

/**
 * @brief Emits a quick fix that prepends a funcdef declaration and renames the type reference.
 * @param[in] request Code action request.
 * @param[in] diag Triggering diagnostic.
 * @param[in] item Funcdef fix metadata item.
 * @param[out] actions Destination actions vector.
 */
void EmitFuncdefCodeAction(const CodeActionRequest& request, const lsp::Diagnostic& diag, const FuncdefFixItem& item,
                           std::vector<lsp::CodeAction>& actions)
{
    std::vector<lsp::TextEdit> edits;
    lsp::TextEdit insertion;
    insertion.range.start.line = 0;
    insertion.range.start.character = 0;
    insertion.range.end.line = 0;
    insertion.range.end.character = 0;
    insertion.newText = item.declaration;
    edits.push_back(std::move(insertion));

    lsp::TextEdit rename;
    rename.range.start.line = ts_node_start_point(item.typeNode).row;
    rename.range.start.character = ts_node_start_point(item.typeNode).column;
    rename.range.end.line = ts_node_end_point(item.typeNode).row;
    rename.range.end.character = ts_node_end_point(item.typeNode).column;
    rename.newText = item.funcdefName;
    edits.push_back(std::move(rename));

    lsp::CodeAction action;
    action.title = "Declare funcdef '" + item.funcdefName + "' for '" + item.functionName + "'";
    action.kind = lsp::CodeActionKindEnum(lsp::CodeActionKind::QuickFix);
    action.isPreferred = true;
    action.diagnostics = std::vector<lsp::Diagnostic>{diag};

    lsp::WorkspaceEdit wsEdit;
    lsp::Map<lsp::DocumentUri, std::vector<lsp::TextEdit>> changes;
    changes[lsp::DocumentUri::parse(request.uri)] = std::move(edits);
    wsEdit.changes = std::move(changes);
    action.edit = std::move(wsEdit);

    actions.push_back(std::move(action));
}

/**
 * @brief Declares the funcdef a function handle needs, derived from the function itself.
 * @param[in] request Code action request.
 * @param[in] rootNode Root AST node.
 * @param[out] actions Destination actions vector.
 */
void TryAddGenerateFuncdefFix(const CodeActionRequest& request, TSNode rootNode, std::vector<lsp::CodeAction>& actions)
{
    if (ts_node_is_null(rootNode) || request.sourceCode.empty())
    {
        return;
    }

    for (const auto& diag : request.context.diagnostics)
    {
        if (!MatchDiagnosticCode(diag, "as-hint-funcdef-missing"))
        {
            continue;
        }

        const TSPoint point = {diag.range.start.line, diag.range.start.character};
        TSNode typeNode = ts_node_descendant_for_point_range(rootNode, point, point);
        if (ts_node_is_null(typeNode))
        {
            continue;
        }

        const std::string functionName = GetNodeText(typeNode, request.sourceCode);
        if (functionName.empty())
        {
            continue;
        }

        const analysis::Symbol* target = FindUniqueGlobalFunction(request.symbolTable, functionName);
        if (!target)
        {
            continue;
        }

        const auto& signature = target->GetFunction();
        const std::string funcdefName = functionName + "Func";
        std::string declaration = FormatFuncdefDeclarationText(signature, funcdefName);
        EmitFuncdefCodeAction(request, diag,
                              FuncdefFixItem{typeNode, functionName, funcdefName, std::move(declaration)}, actions);
    }
}

/**
 * @brief Calls the bool conversion operator explicitly, turning `if (h)` into `if (h.opImplConv())`.
 * @param[in] request Code action request.
 * @param[in] rootNode Root AST node.
 * @param[out] actions Destination actions vector.
 */
void TryAddBoolConversionFix(const CodeActionRequest& request, TSNode rootNode, std::vector<lsp::CodeAction>& actions)
{
    if (ts_node_is_null(rootNode) || request.sourceCode.empty())
    {
        return;
    }

    for (const auto& diag : request.context.diagnostics)
    {
        if (!MatchDiagnosticCode(diag, "as-hint-bool-conversion"))
        {
            continue;
        }

        if (!std::holds_alternative<lsp::String>(diag.message))
        {
            continue;
        }
        const std::string& message = std::get<lsp::String>(diag.message);

        const size_t firstQuote = message.find('\'');
        const size_t firstClose =
            firstQuote == std::string::npos ? std::string::npos : message.find('\'', firstQuote + 1);
        const size_t secondQuote =
            firstClose == std::string::npos ? std::string::npos : message.find('\'', firstClose + 1);
        const size_t secondClose =
            secondQuote == std::string::npos ? std::string::npos : message.find('\'', secondQuote + 1);
        if (secondClose == std::string::npos)
        {
            continue;
        }

        const std::string oper = message.substr(secondQuote + 1, secondClose - secondQuote - 1);
        if (oper != "opImplConv" && oper != "opConv")
        {
            continue;
        }

        lsp::TextEdit edit;
        edit.range.start = diag.range.end;
        edit.range.end = diag.range.end;
        edit.newText = "." + oper + "()";

        lsp::CodeAction action;
        action.title = "Call " + oper + "() explicitly";
        action.kind = lsp::CodeActionKindEnum(lsp::CodeActionKind::QuickFix);
        action.isPreferred = true;
        action.diagnostics = std::vector<lsp::Diagnostic>{diag};

        lsp::WorkspaceEdit wsEdit;
        lsp::Map<lsp::DocumentUri, std::vector<lsp::TextEdit>> changes;
        changes[lsp::DocumentUri::parse(request.uri)] = {std::move(edit)};
        wsEdit.changes = std::move(changes);
        action.edit = std::move(wsEdit);

        actions.push_back(std::move(action));
    }
}

/**
 * @brief Adds the `property` keyword to an accessor that carries none.
 * @param[in] request Code action request.
 * @param[in] rootNode Root AST node.
 * @param[out] actions Destination actions vector.
 */
void TryAddAccessorPropertyKeywordFix(const CodeActionRequest& request, TSNode rootNode,
                                      std::vector<lsp::CodeAction>& actions)
{
    if (ts_node_is_null(rootNode) || request.sourceCode.empty())
    {
        return;
    }

    for (const auto& diag : request.context.diagnostics)
    {
        if (!MatchDiagnosticCode(diag, "as-hint-accessor-portability"))
        {
            continue;
        }

        const TSPoint point = {diag.range.start.line, diag.range.start.character};
        TSNode node = ts_node_descendant_for_point_range(rootNode, point, point);
        while (!ts_node_is_null(node) && std::string_view(ts_node_type(node)) != "func_declaration")
        {
            node = ts_node_parent(node);
        }
        if (ts_node_is_null(node))
        {
            continue;
        }

        const TSNode parameters = parser::GetChildByField(node, parser::fields::Parameters);
        if (ts_node_is_null(parameters))
        {
            continue;
        }

        const TSPoint insertAt = ts_node_end_point(parameters);

        lsp::TextEdit edit;
        edit.range.start.line = insertAt.row;
        edit.range.start.character = insertAt.column;
        edit.range.end.line = insertAt.row;
        edit.range.end.character = insertAt.column;
        edit.newText = " property";

        lsp::CodeAction action;
        action.title = "Add the 'property' keyword";
        action.kind = lsp::CodeActionKindEnum(lsp::CodeActionKind::QuickFix);
        action.isPreferred = true;
        action.diagnostics = std::vector<lsp::Diagnostic>{diag};

        lsp::WorkspaceEdit wsEdit;
        lsp::Map<lsp::DocumentUri, std::vector<lsp::TextEdit>> changes;
        changes[lsp::DocumentUri::parse(request.uri)] = {std::move(edit)};
        wsEdit.changes = std::move(changes);
        action.edit = std::move(wsEdit);

        actions.push_back(std::move(action));
    }
}

/**
 * @brief Finds the parameter list node of a function declaration.
 * @param[in] fnNode Function declaration node.
 * @return Parameter list node or null node.
 */
TSNode FindFunctionParametersNode(TSNode fnNode)
{
    TSNode paramList = parser::GetChildByField(fnNode, parser::fields::Parameters);
    if (!ts_node_is_null(paramList))
    {
        return paramList;
    }
    uint32_t cnt = ts_node_child_count(fnNode);
    for (uint32_t c = 0; c < cnt; ++c)
    {
        TSNode ch = ts_node_child(fnNode, c);
        if (std::string_view(ts_node_type(ch)) == "parameter_list")
        {
            return ch;
        }
    }
    return paramList;
}

/**
 * @brief Attempts to construct a missing-const quick-fix action for a matching function symbol.
 * @param[in] request Code action request.
 * @param[in] rootNode Root AST node.
 * @param[in] diag Triggering diagnostic.
 * @param[in] sym Target function symbol.
 * @return Constructed CodeAction, or std::nullopt.
 */
std::optional<lsp::CodeAction> TryBuildMissingConstFixForSymbol(const CodeActionRequest& request, TSNode rootNode,
                                                                const lsp::Diagnostic& diag,
                                                                const analysis::Symbol& sym)
{
    TSPoint fnPt = {sym.startLine, sym.startCharacter};
    TSNode fnNode = ts_node_descendant_for_point_range(rootNode, fnPt, fnPt);
    while (!ts_node_is_null(fnNode) && std::string_view(ts_node_type(fnNode)) != "func_declaration")
    {
        fnNode = ts_node_parent(fnNode);
    }
    if (ts_node_is_null(fnNode))
    {
        return std::nullopt;
    }
    TSNode paramList = FindFunctionParametersNode(fnNode);
    if (ts_node_is_null(paramList))
    {
        return std::nullopt;
    }

    TSPoint insertPt = ts_node_end_point(paramList);
    lsp::TextEdit edit;
    edit.range = lsp::Range{{insertPt.row, insertPt.column}, {insertPt.row, insertPt.column}};
    edit.newText = " const";

    lsp::CodeAction action;
    action.title = "Add 'const' qualifier to method";
    action.kind = lsp::CodeActionKindEnum(lsp::CodeActionKind::QuickFix);
    action.isPreferred = true;
    action.diagnostics = {diag};

    lsp::WorkspaceEdit wsEdit;
    lsp::Map<lsp::DocumentUri, std::vector<lsp::TextEdit>> changes;
    changes[lsp::DocumentUri::parse(request.uri)] = {std::move(edit)};
    wsEdit.changes = std::move(changes);
    action.edit = std::move(wsEdit);
    return action;
}

/**
 * @brief Generates quick-fixes to append `const` to methods requested by diagnostics.
 * @param[in] request Code action request.
 * @param[in] rootNode Root AST node.
 * @param[in] diag Target diagnostic.
 * @param[out] actions Destination actions vector.
 */
void TryAddMissingConstDiagnosticFix(const CodeActionRequest& request, TSNode rootNode, const lsp::Diagnostic& diag,
                                     std::vector<lsp::CodeAction>& actions)
{
    TSPoint dPt = {diag.range.start.line, diag.range.start.character};
    TSNode memberNode = ts_node_descendant_for_point_range(rootNode, dPt, dPt);
    std::string methodName = GetNodeText(memberNode, request.sourceCode);

    TSNode callee = ts_node_parent(memberNode);
    TSNode objNode = parser::GetChildByField(callee, parser::fields::Object);
    auto rootScope = request.scopeIndex.GetRoot(request.uri);
    const analysis::Scope* scope = FindScopeByLineOrRoot(rootScope.get(), dPt.row, dPt.column);
    std::string objType = analysis::CleanBaseType(
        analysis::ResolveExpressionType(objNode, {scope, request.symbolTable, request.sourceCode, request.uri}));

    request.symbolTable.ForEachSymbol(
        [&]([[maybe_unused]] const std::string& qualifiedName, const std::vector<analysis::Symbol>& symList)
        {
            for (const auto& sym : symList)
            {
                if (sym.type == analysis::SymbolType::Function && sym.name == methodName &&
                    (objType.empty() || sym.containerName == objType) && sym.fileUri == request.uri)
                {
                    if (auto action = TryBuildMissingConstFixForSymbol(request, rootNode, diag, sym))
                    {
                        actions.push_back(std::move(*action));
                    }
                }
            }
        });
}

/**
 * @brief Checks if a `const` qualifier already exists between parameter list and method body.
 * @param[in] sourceCode Source text.
 * @param[in] paramList Parameter list node.
 * @param[in] bodyNode Method body node.
 * @param[in] fnNode Function declaration node.
 * @return True if const keyword is present in header suffix.
 */
bool HasConstQualifierBetween(std::string_view sourceCode, TSNode paramList, TSNode bodyNode, TSNode fnNode)
{
    uint32_t pEndByte = ts_node_end_byte(paramList);
    uint32_t bStartByte = !ts_node_is_null(bodyNode) ? ts_node_start_byte(bodyNode) : ts_node_end_byte(fnNode);
    if (pEndByte < bStartByte && bStartByte <= sourceCode.size())
    {
        std::string between = std::string(sourceCode.substr(pEndByte, bStartByte - pEndByte));
        return between.find("const") != std::string::npos;
    }
    return false;
}

/**
 * @brief Checks method under cursor and suggests adding `const` if method body doesn't mutate class state.
 * @param[in] request Code action request.
 * @param[in] rootNode Root AST node.
 * @param[out] actions Destination actions vector.
 */
void TryAddIntentionalConstAction(const CodeActionRequest& request, TSNode rootNode,
                                  std::vector<lsp::CodeAction>& actions)
{
    TSPoint pt = {request.range.start.line, request.range.start.character};
    TSNode leaf = ts_node_descendant_for_point_range(rootNode, pt, pt);
    if (ts_node_is_null(leaf))
    {
        return;
    }

    TSNode fnNode = leaf;
    while (!ts_node_is_null(fnNode) && std::string_view(ts_node_type(fnNode)) != "func_declaration")
    {
        fnNode = ts_node_parent(fnNode);
    }
    TSNode classNode = fnNode;
    while (!ts_node_is_null(classNode) && std::string_view(ts_node_type(classNode)) != "class_declaration")
    {
        classNode = ts_node_parent(classNode);
    }
    if (ts_node_is_null(fnNode) || ts_node_is_null(classNode))
    {
        return;
    }

    TSNode paramList = FindFunctionParametersNode(fnNode);
    TSNode bodyNode = FindFunctionBodyBlock(fnNode);
    if (ts_node_is_null(paramList) || ts_node_is_null(bodyNode))
    {
        return;
    }
    if (HasConstQualifierBetween(request.sourceCode, paramList, bodyNode, fnNode))
    {
        return;
    }

    TSNode classNameNode = parser::GetChildByField(classNode, parser::fields::Name);
    std::string className = GetNodeText(classNameNode, request.sourceCode);

    auto rootScope = request.scopeIndex.GetRoot(request.uri);
    const analysis::Scope* scope =
        FindScopeByLineOrRoot(rootScope.get(), ts_node_start_point(fnNode).row, ts_node_start_point(fnNode).column);

    ClassMutationContext mutCtx{bodyNode, classNode, request.sourceCode, request.symbolTable, className, scope};
    if (!MethodBodyMutatesClassState(mutCtx))
    {
        TSPoint insertPt = ts_node_end_point(paramList);
        lsp::TextEdit edit;
        edit.range = lsp::Range{{insertPt.row, insertPt.column}, {insertPt.row, insertPt.column}};
        edit.newText = " const";

        lsp::CodeAction action;
        action.title = "Add 'const' qualifier to method";
        action.kind = lsp::CodeActionKindEnum(lsp::CodeActionKind::QuickFix);

        lsp::WorkspaceEdit wsEdit;
        lsp::Map<lsp::DocumentUri, std::vector<lsp::TextEdit>> changes;
        changes[lsp::DocumentUri::parse(request.uri)] = {std::move(edit)};
        wsEdit.changes = std::move(changes);
        action.edit = std::move(wsEdit);

        actions.push_back(std::move(action));
    }
}

/**
 * @brief Tries to generate Missing const Qualifier quick fixes and intention actions.
 * @param[in] request Code action request context.
 * @param[in] rootNode Root AST node.
 * @param[out] actions Destination actions vector.
 */
void TryAddConstQualifierActions(const CodeActionRequest& request, TSNode rootNode,
                                 std::vector<lsp::CodeAction>& actions)
{
    if (ts_node_is_null(rootNode) || request.sourceCode.empty())
    {
        return;
    }
    for (const auto& diag : request.context.diagnostics)
    {
        if (MatchDiagnosticCode(diag, "as-err-const-method-required"))
        {
            TryAddMissingConstDiagnosticFix(request, rootNode, diag, actions);
        }
    }
    TryAddIntentionalConstAction(request, rootNode, actions);
}

struct IncludeRefCheckContext
{
    const std::string& currentUri;
    const analysis::SymbolTable& table;
    const ankerl::unordered_dense::set<std::string>& docReferences;
    const std::vector<std::string>& allowedRoots;
};

/**
 * @brief Checks whether an include directive is referenced by symbols used in the file.
 * @param[in] inc Include directive.
 * @param[in] ctx Include reference checking context.
 * @return True if include is referenced or has unknown symbols.
 */
bool IsIncludeDirectiveReferenced(const angel_lsp::utils::IncludeDirective& inc, const IncludeRefCheckContext& ctx)
{
    std::string resolved =
        utils::IncludeResolver::ResolveIncludePath(inc.rawPath, ctx.currentUri, {}, ctx.allowedRoots);
    std::vector<std::string> symbolsInFile;
    ctx.table.ForEachSymbol(
        [&]([[maybe_unused]] const std::string& qualifiedName, const std::vector<analysis::Symbol>& symList)
        {
            for (const auto& s : symList)
            {
                if ((!resolved.empty() && s.fileUri == resolved) ||
                    (!inc.rawPath.empty() && s.fileUri.find(inc.rawPath) != std::string::npos))
                {
                    symbolsInFile.push_back(s.name);
                }
            }
        });

    if (symbolsInFile.empty())
    {
        return true;
    }
    for (const auto& symName : symbolsInFile)
    {
        if (ctx.docReferences.contains(symName))
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief Formats sorted angled and quoted include directive blocks.
 * @param[in] angledIncludes Sorted vector of angled include paths.
 * @param[in] quotedIncludes Sorted vector of quoted include paths.
 * @return Formatted include header block text.
 */
std::string FormatSortedIncludesBlock(const std::vector<std::string>& angledIncludes,
                                      const std::vector<std::string>& quotedIncludes)
{
    std::string newHeaderBlock;
    for (const auto& p : angledIncludes)
    {
        newHeaderBlock += "#include <" + p + ">\n";
    }
    if (!angledIncludes.empty() && !quotedIncludes.empty())
    {
        newHeaderBlock += "\n";
    }
    for (const auto& p : quotedIncludes)
    {
        newHeaderBlock += "#include \"" + p + "\"\n";
    }
    return newHeaderBlock;
}

/**
 * @brief Tries to generate a Sort and Clean #include Directives code action.
 * @param[in] request Code action request context.
 * @param[out] actions Destination actions vector.
 */
void TryAddSortAndCleanIncludesAction(const CodeActionRequest& request, std::vector<lsp::CodeAction>& actions)
{
    auto includes = utils::IncludeResolver::ExtractIncludes(request.sourceCode);
    if (includes.empty())
    {
        return;
    }

    size_t firstLine = includes.front().line;
    size_t lastLine = includes.back().line;

    ankerl::unordered_dense::set<std::string> docReferences;
    auto rootScope = request.scopeIndex.GetRoot(request.uri);
    CollectAllReferences(rootScope.get(), docReferences);

    std::vector<std::string> angledIncludes;
    std::vector<std::string> quotedIncludes;
    ankerl::unordered_dense::set<std::string> seen;
    const IncludeRefCheckContext refCtx{request.uri, request.symbolTable, docReferences, request.allowedRoots};

    for (const auto& inc : includes)
    {
        if (seen.contains(inc.rawPath))
        {
            continue;
        }
        seen.insert(inc.rawPath);

        if (!IsIncludeDirectiveReferenced(inc, refCtx))
        {
            continue;
        }

        if (inc.isAngled)
        {
            angledIncludes.push_back(inc.rawPath);
        }
        else
        {
            quotedIncludes.push_back(inc.rawPath);
        }
    }

    std::sort(angledIncludes.begin(), angledIncludes.end());
    std::sort(quotedIncludes.begin(), quotedIncludes.end());

    lsp::TextEdit edit;
    edit.range.start = lsp::Position{static_cast<uint32_t>(firstLine), 0};
    edit.range.end = lsp::Position{static_cast<uint32_t>(lastLine + 1), 0};
    edit.newText = FormatSortedIncludesBlock(angledIncludes, quotedIncludes);

    lsp::CodeAction action;
    action.title = "Sort and Clean #include Directives";
    action.kind = lsp::CodeActionKindEnum(lsp::CodeActionKind::SourceOrganizeImports);

    lsp::WorkspaceEdit wsEdit;
    lsp::Map<lsp::DocumentUri, std::vector<lsp::TextEdit>> changes;
    changes[lsp::DocumentUri::parse(request.uri)] = {std::move(edit)};
    wsEdit.changes = std::move(changes);
    action.edit = std::move(wsEdit);

    actions.push_back(std::move(action));
}

/**
 * @brief Constructs a TextEdit to delete an entire declaration line.
 * @param[in] def Local variable definition.
 * @param[in] sourceCode Source text.
 * @return TextEdit deleting the line range.
 */
lsp::TextEdit BuildRemoveSingleDeclaratorLine(const analysis::LocalDefinition* def, std::string_view sourceCode)
{
    lsp::TextEdit edit;
    uint32_t lineCount = 1;
    for (char c : sourceCode)
    {
        if (c == '\n')
        {
            lineCount++;
        }
    }
    if (def->endLine + 1 < lineCount)
    {
        edit.range.start = lsp::Position{def->startLine, 0};
        edit.range.end = lsp::Position{def->endLine + 1, 0};
    }
    else
    {
        edit.range.start = lsp::Position{def->startLine, 0};
        size_t lastNewline = sourceCode.rfind('\n');
        size_t lastLineLen =
            (lastNewline != std::string::npos) ? (sourceCode.size() - (lastNewline + 1)) : sourceCode.size();
        edit.range.end = lsp::Position{def->endLine, static_cast<uint32_t>(lastLineLen)};
    }
    edit.newText = "";
    return edit;
}

/**
 * @brief Constructs a TextEdit to delete one declarator from a comma-separated list.
 * @param[in] declarators List of all declarators in declaration.
 * @param[in] declarator Target declarator node.
 * @return TextEdit deleting the declarator.
 */
lsp::TextEdit BuildRemoveMultiDeclaratorItem(const std::vector<TSNode>& declarators, TSNode declarator)
{
    lsp::TextEdit edit;
    size_t targetIdx = 0;
    for (size_t k = 0; k < declarators.size(); ++k)
    {
        if (ts_node_start_byte(declarators[k]) == ts_node_start_byte(declarator))
        {
            targetIdx = k;
            break;
        }
    }

    if (targetIdx == 0 && declarators.size() > 1)
    {
        TSPoint startPt = ts_node_start_point(declarators[0]);
        TSPoint endPt = ts_node_start_point(declarators[1]);
        edit.range.start = lsp::Position{startPt.row, startPt.column};
        edit.range.end = lsp::Position{endPt.row, endPt.column};
    }
    else
    {
        TSPoint startPt = ts_node_end_point(declarators[targetIdx - 1]);
        TSPoint endPt = ts_node_end_point(declarators[targetIdx]);
        edit.range.start = lsp::Position{startPt.row, startPt.column};
        edit.range.end = lsp::Position{endPt.row, endPt.column};
    }
    edit.newText = "";
    return edit;
}

/**
 * @brief Constructs a TextEdit to delete an unused variable declaration or declarator.
 * @param[in] rootNode Root AST node.
 * @param[in] sourceCode Source text.
 * @param[in] def Target local definition.
 * @return TextEdit deleting the unused variable.
 */
lsp::TextEdit BuildRemoveUnusedVariableEdit(TSNode rootNode, std::string_view sourceCode,
                                            const analysis::LocalDefinition* def)
{
    TSPoint pt = {def->startLine, def->startCharacter};
    TSNode leaf = ts_node_descendant_for_point_range(rootNode, pt, pt);

    TSNode declarator = leaf;
    while (!ts_node_is_null(declarator) && std::string_view(ts_node_type(declarator)) != "variable_declarator")
    {
        declarator = ts_node_parent(declarator);
    }

    TSNode decl = declarator;
    while (!ts_node_is_null(decl) && std::string_view(ts_node_type(decl)) != "variable_declaration")
    {
        decl = ts_node_parent(decl);
    }

    if (ts_node_is_null(decl))
    {
        lsp::TextEdit edit;
        edit.range.start = lsp::Position{def->startLine, 0};
        edit.range.end = lsp::Position{def->endLine + 1, 0};
        edit.newText = "";
        return edit;
    }

    std::vector<TSNode> declarators;
    uint32_t dCount = ts_node_child_count(decl);
    for (uint32_t i = 0; i < dCount; ++i)
    {
        TSNode child = ts_node_child(decl, i);
        if (std::string_view(ts_node_type(child)) == "variable_declarator")
        {
            declarators.push_back(child);
        }
    }

    if (declarators.size() <= 1)
    {
        return BuildRemoveSingleDeclaratorLine(def, sourceCode);
    }
    return BuildRemoveMultiDeclaratorItem(declarators, declarator);
}

/**
 * @brief Generates quick-fixes to delete unused local variables in the active scope.
 * @param[in] request Code action request.
 * @param[in] rootNode AST root node.
 * @param[out] actions Destination actions vector.
 */
void TryAddRemoveUnusedVariableFixes(const CodeActionRequest& request, TSNode rootNode,
                                     std::vector<lsp::CodeAction>& actions)
{
    auto rootScope = request.scopeIndex.GetRoot(request.uri);
    if (!rootScope)
    {
        return;
    }

    ankerl::unordered_dense::set<const analysis::LocalDefinition*> usedDefs;
    CollectUsedDefinitions(rootScope.get(), usedDefs);

    std::vector<const analysis::LocalDefinition*> unusedVars;
    CollectUnusedVariables(rootScope.get(), usedDefs, unusedVars);

    for (const auto* def : unusedVars)
    {
        bool matchesRange = (request.range.start.line <= def->endLine && request.range.end.line >= def->startLine);
        bool matchesDiag = false;

        for (const auto& diag : request.context.diagnostics)
        {
            if (diag.range.start.line <= def->endLine && diag.range.end.line >= def->startLine)
            {
                matchesDiag = true;
                break;
            }
        }

        if (!matchesRange && !matchesDiag)
        {
            continue;
        }

        lsp::TextEdit edit = BuildRemoveUnusedVariableEdit(rootNode, request.sourceCode, def);
        lsp::CodeAction action;
        action.title = "Remove unused variable '" + def->name + "'";
        action.kind = lsp::CodeActionKindEnum(lsp::CodeActionKind::QuickFix);
        action.isPreferred = true;

        lsp::WorkspaceEdit wsEdit;
        lsp::Map<lsp::DocumentUri, std::vector<lsp::TextEdit>> changes;
        changes[lsp::DocumentUri::parse(request.uri)] = {std::move(edit)};
        wsEdit.changes = std::move(changes);
        action.edit = std::move(wsEdit);

        std::vector<lsp::Diagnostic> matchingDiags;
        for (const auto& diag : request.context.diagnostics)
        {
            if (diag.range.start.line <= def->endLine && diag.range.end.line >= def->startLine)
            {
                matchingDiags.push_back(diag);
            }
        }
        if (!matchingDiags.empty())
        {
            action.diagnostics = std::move(matchingDiags);
        }

        actions.push_back(std::move(action));
    }
}

/**
 * @brief Formats stub declarations for missing interface methods.
 * @param[in] missingMethods List of missing method symbols.
 * @return Formatted stub code string.
 */
std::string FormatMissingInterfaceMethodStubs(const std::vector<analysis::Symbol>& missingMethods)
{
    std::string stubs;
    for (const auto& m : missingMethods)
    {
        const auto& fn = m.GetFunction();
        std::string ret = fn.returnType.empty() ? "void" : fn.returnType;
        stubs += "\n    " + ret + " " + m.name + "(";
        for (size_t p = 0; p < fn.parameters.size(); ++p)
        {
            if (p > 0)
            {
                stubs += ", ";
            }
            const auto& param = fn.parameters[p];
            stubs += param.typeName;
            if (!param.name.empty())
            {
                stubs += " " + param.name;
            }
            if (!param.defaultValue.empty())
            {
                stubs += " = " + param.defaultValue;
            }
        }
        stubs += ")\n    {\n";
        if (ret != "void")
        {
            std::string defaultVal = GetDefaultReturnValue(ret);
            stubs += "        return " + defaultVal + ";\n";
        }
        stubs += "    }\n";
    }
    return stubs;
}

/**
 * @brief Identifies which interface methods are not implemented by a class.
 * @param[in] className Name of implementing class.
 * @param[in] cleanIface Cleaned interface type name.
 * @param[in] table Symbol table.
 * @return Vector of missing interface method symbols.
 */
std::vector<analysis::Symbol> CollectMissingInterfaceMethods(const std::string& className,
                                                             const std::string& cleanIface,
                                                             const analysis::SymbolTable& table)
{
    auto ifaceHierarchy = analysis::GetInheritedTypeHierarchy(cleanIface, table);
    if (ifaceHierarchy.empty())
    {
        return {};
    }

    std::vector<analysis::Symbol> ifaceMethods;
    for (const auto& ifaceName : ifaceHierarchy)
    {
        table.ForEachSymbol(
            [&]([[maybe_unused]] const std::string& qualifiedName, const std::vector<analysis::Symbol>& mSyms)
            {
                for (const auto& m : mSyms)
                {
                    if (m.type == analysis::SymbolType::Function && m.containerName == ifaceName)
                    {
                        ifaceMethods.push_back(m);
                    }
                }
            });
    }

    std::vector<analysis::Symbol> classMethods;
    table.ForEachSymbol(
        [&]([[maybe_unused]] const std::string& qualifiedName, const std::vector<analysis::Symbol>& mSyms)
        {
            for (const auto& m : mSyms)
            {
                if (m.type == analysis::SymbolType::Function && m.containerName == className)
                {
                    classMethods.push_back(m);
                }
            }
        });

    std::vector<analysis::Symbol> missingMethods;
    for (const auto& ifMethod : ifaceMethods)
    {
        bool implemented = false;
        for (const auto& cMethod : classMethods)
        {
            if (cMethod.name == ifMethod.name &&
                cMethod.GetFunction().parameters.size() == ifMethod.GetFunction().parameters.size())
            {
                implemented = true;
                break;
            }
        }
        if (!implemented)
        {
            missingMethods.push_back(ifMethod);
        }
    }
    return missingMethods;
}

/**
 * @brief Locates the insertion position before the closing brace of a class declaration.
 * @param[in] rootNode Root AST node.
 * @param[in] clsSym Class symbol.
 * @return LSP position for method stub insertion.
 */
lsp::Position FindClassInterfaceInsertionPosition(TSNode rootNode, const analysis::Symbol& clsSym)
{
    lsp::Position insertPos{clsSym.endLine, clsSym.endCharacter};
    TSPoint cPt = {clsSym.startLine, clsSym.startCharacter};
    TSNode cNode = ts_node_descendant_for_point_range(rootNode, cPt, cPt);
    while (!ts_node_is_null(cNode) && std::string_view(ts_node_type(cNode)) != "class_declaration")
    {
        cNode = ts_node_parent(cNode);
    }
    if (ts_node_is_null(cNode))
    {
        return insertPos;
    }

    TSNode bodyNode = parser::GetChildByField(cNode, parser::fields::Body);
    if (ts_node_is_null(bodyNode))
    {
        uint32_t cnt = ts_node_child_count(cNode);
        for (uint32_t i = 0; i < cnt; ++i)
        {
            TSNode ch = ts_node_child(cNode, i);
            if (std::string_view(ts_node_type(ch)) == "class_body")
            {
                bodyNode = ch;
                break;
            }
        }
    }

    if (!ts_node_is_null(bodyNode))
    {
        uint32_t bCount = ts_node_child_count(bodyNode);
        for (int i = static_cast<int>(bCount) - 1; i >= 0; --i)
        {
            TSNode bChild = ts_node_child(bodyNode, static_cast<uint32_t>(i));
            if (std::string_view(ts_node_type(bChild)) == "}")
            {
                TSPoint pt = ts_node_start_point(bChild);
                insertPos = lsp::Position{pt.row, pt.column};
                break;
            }
        }
    }
    return insertPos;
}

/**
 * @brief Generates quick-fixes to implement missing interface methods for implementing classes.
 * @param[in] request Code action request.
 * @param[in] rootNode AST root node.
 * @param[out] actions Destination actions vector.
 */
void TryAddImplementInterfaceFixes(const CodeActionRequest& request, TSNode rootNode,
                                   std::vector<lsp::CodeAction>& actions)
{
    request.symbolTable.ForEachSymbol(
        [&]([[maybe_unused]] const std::string& qualifiedName, const std::vector<analysis::Symbol>& symbols)
        {
            for (const auto& clsSym : symbols)
            {
                if (clsSym.type != analysis::SymbolType::Class || clsSym.fileUri != request.uri)
                {
                    continue;
                }
                if (request.range.start.line > clsSym.endLine || request.range.end.line < clsSym.startLine)
                {
                    continue;
                }

                const auto& cls = clsSym.GetClass();
                for (const auto& baseName : cls.bases)
                {
                    std::string cleanIface = analysis::CleanBaseType(baseName);
                    if (cleanIface.empty())
                    {
                        continue;
                    }

                    auto missingMethods = CollectMissingInterfaceMethods(clsSym.name, cleanIface, request.symbolTable);
                    if (missingMethods.empty())
                    {
                        continue;
                    }

                    std::string stubs = FormatMissingInterfaceMethodStubs(missingMethods);
                    lsp::Position insertPos = FindClassInterfaceInsertionPosition(rootNode, clsSym);

                    lsp::TextEdit edit;
                    edit.range = lsp::Range{insertPos, insertPos};
                    edit.newText = stubs;

                    lsp::CodeAction action;
                    action.title = "Implement missing interface methods for '" + cleanIface + "'";
                    action.kind = lsp::CodeActionKindEnum(lsp::CodeActionKind::QuickFix);
                    action.isPreferred = true;

                    std::vector<lsp::Diagnostic> matchingDiags;
                    for (const auto& diag : request.context.diagnostics)
                    {
                        if (MatchDiagnosticCode(diag, "as-err-interface-impl-missing") &&
                            diag.range.start.line <= clsSym.endLine && diag.range.end.line >= clsSym.startLine)
                        {
                            matchingDiags.push_back(diag);
                        }
                    }
                    if (!matchingDiags.empty())
                    {
                        action.diagnostics = std::move(matchingDiags);
                    }

                    lsp::WorkspaceEdit wsEdit;
                    lsp::Map<lsp::DocumentUri, std::vector<lsp::TextEdit>> changes;
                    changes[lsp::DocumentUri::parse(request.uri)].push_back(std::move(edit));
                    wsEdit.changes = std::move(changes);
                    action.edit = std::move(wsEdit);

                    actions.push_back(std::move(action));
                }
            }
        });
}
} // namespace

std::optional<std::vector<lsp::CodeAction>> GetCodeActions(const CodeActionRequest& request)
{
    if (!request.tree || request.sourceCode.empty())
    {
        return std::nullopt;
    }

    TSNode rootNode = ts_tree_root_node(request.tree);
    if (ts_node_is_null(rootNode))
    {
        return std::nullopt;
    }

    std::vector<lsp::CodeAction> actions;
    TryAddRemoveUnusedVariableFixes(request, rootNode, actions);
    TryAddImplementInterfaceFixes(request, rootNode, actions);
    TryAddExtractVariableAction(request, rootNode, actions);
    TryAddExtractMethodAction(request, rootNode, actions);
    TryAddGetterSetterActions(request, rootNode, actions);
    TryAddConstQualifierActions(request, rootNode, actions);
    TryAddUndefinedIdentifierSuggestions(request, rootNode, actions);
    TryAddHandleOnPrimitiveFix(request, rootNode, actions);
    TryAddUnresolvedIncludeSuggestions(request, actions);
    TryAddAccessorPropertyKeywordFix(request, rootNode, actions);
    TryAddBoolConversionFix(request, rootNode, actions);
    TryAddGenerateFuncdefFix(request, rootNode, actions);
    TryAddSortAndCleanIncludesAction(request, actions);

    if (actions.empty())
    {
        return std::nullopt;
    }

    return actions;
}

std::optional<lsp::CodeAction> ResolveCodeAction(const CodeActionResolveRequest& request)
{
    return request.action;
}
} // namespace angel_lsp::features
