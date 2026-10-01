#include "analysis/ConstChecker.h"
#include "analysis/ASTUtils.h"
#include "analysis/NodeIndex.h"
#include "analysis/SemanticHelpers.h"
#include "utils/Utils.h"

#include "parser/GrammarNames.h"
#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{
namespace
{
/**
 * @brief True when a declared type's written form begins with `const`.
 *
 * The grammar puts `const` first in a type, and both the scope tree and the symbol table
 * keep the type's source text, so this is what a declaration's constness looks like from
 * here. `const Entity@` counts: in AngelScript that is a handle to a read-only object,
 * which is exactly the case the engine refuses a mutation through.
 */
bool TypeTextIsConst(std::string_view typeText)
{
    while (!typeText.empty() && (typeText.front() == ' ' || typeText.front() == '\t'))
    {
        typeText.remove_prefix(1);
    }
    return typeText.starts_with("const ") || typeText == "const";
}

/**
 * @brief True when a handle type is declared const itself (e.g. `Type@ const`).
 */
bool TypeTextIsHandleConst(std::string_view typeText)
{
    while (!typeText.empty() && (typeText.front() == ' ' || typeText.front() == '\t'))
    {
        typeText.remove_prefix(1);
    }
    while (!typeText.empty() && (typeText.back() == ' ' || typeText.back() == '\t'))
    {
        typeText.remove_suffix(1);
    }
    return typeText.ends_with(" const");
}

/**
 * @brief Constness of a parameter, read off the enclosing declaration's parameter list.
 *
 * The scope tree records a parameter as a definition but not its written type - it fills
 * that in from a variable_declarator, which a parameter is not - so the answer has to come
 * from the tree. Kept here rather than added to LocalDefinition on purpose: typeName there
 * is documented as meaningful only for a variable, and populating it for parameters would
 * quietly change what isHandleType and typeKind mean for every other reader of the scope
 * tree, including the null-assignment rule.
 *
 * @param[in] node AST node inside the function or method.
 * @param[in] name Parameter name to search for.
 * @param[in] sourceCode Document source text.
 * @param[in] checkHandleConst True to check if handle itself is const (Type@ const).
 * @return True/false if parameter found and whether its type is const; std::nullopt if not found.
 */
std::optional<bool> ParameterIsConst(TSNode node, std::string_view name, std::string_view sourceCode,
                                     bool checkHandleConst = false)
{
    TSNode owner = node;
    while (!ts_node_is_null(owner))
    {
        const std::string_view ownerType = ts_node_type(owner);
        if (ownerType == "func_declaration" || ownerType == "lambda_expression" || ownerType == "virtual_property" ||
            ownerType == "accessor")
        {
            break;
        }
        owner = ts_node_parent(owner);
    }
    if (ts_node_is_null(owner))
    {
        return std::nullopt;
    }

    TSNode parameters = parser::GetChildByField(owner, parser::fields::Parameters);
    if (ts_node_is_null(parameters))
    {
        return std::nullopt;
    }

    const uint32_t count = ts_node_named_child_count(parameters);
    for (uint32_t i = 0; i < count; ++i)
    {
        TSNode parameter = ts_node_named_child(parameters, i);
        if (std::string_view(ts_node_type(parameter)) != "parameter")
        {
            continue;
        }

        TSNode nameNode = parser::GetChildByField(parameter, parser::fields::Name);
        if (ts_node_is_null(nameNode) || NodeText(nameNode, sourceCode) != name)
        {
            continue;
        }

        TSNode typeNode = parser::GetChildByField(parameter, parser::fields::ParamType);
        if (ts_node_is_null(typeNode))
        {
            return false;
        }
        const std::string_view typeText = NodeText(typeNode, sourceCode);
        return checkHandleConst ? TypeTextIsHandleConst(typeText) : TypeTextIsConst(typeText);
    }
    return std::nullopt;
}

/** @brief What one expression is, as far as constness goes. */
enum class Constness
{
    Unknown, ///< Nothing could be established. Stay silent.
    Mutable,
    Const
};

/**
 * @brief Context bundled for constness resolution queries.
 */
struct ConstContext
{
    const Scope* scope = nullptr;
    const SymbolTable& table;
    std::string_view sourceCode;
};

Constness ResolveConstness(TSNode node, const ConstContext& ctx, int depth = 0);

/** @brief Constness of a plain name, whether it is a local, a parameter or a global. */
Constness ResolveNameConstness(std::string_view name, TSNode node, const ConstContext& ctx)
{
    if (name.empty())
    {
        return Constness::Unknown;
    }

    // A local or a parameter first, since either shadows a global of the same name.
    if (ctx.scope)
    {
        if (const LocalDefinition* def = ResolveInScope(ctx.scope, name))
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

    const auto symbols = ctx.table.FindSymbolsPtr(std::string(name));
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

/** @brief Class and const qualification for enclosing method or accessor. */
struct EnclosingMethodInfo
{
    bool inClass = false;
    bool isConstMethod = false;
    std::string className;
};

/**
 * @brief Identifies enclosing class and whether the enclosing method or accessor is const.
 *
 * @param[in] node AST node inside the method.
 * @param[in] sourceCode Source text.
 * @return EnclosingMethodInfo containing class membership and const qualification.
 */
EnclosingMethodInfo FindEnclosingMethod(TSNode node, std::string_view sourceCode)
{
    EnclosingMethodInfo info;
    TSNode curr = node;
    TSNode funcNode{};

    while (!ts_node_is_null(curr))
    {
        const std::string_view type = ts_node_type(curr);
        if (ts_node_is_null(funcNode) && (type == parser::nodes::FuncDeclaration || type == parser::nodes::Accessor))
        {
            funcNode = curr;
        }
        else if (type == parser::nodes::ClassDeclaration)
        {
            info.inClass = true;
            TSNode nameNode = parser::GetChildByField(curr, parser::fields::Name);
            if (!ts_node_is_null(nameNode))
            {
                info.className = NodeText(nameNode, sourceCode);
            }
            break;
        }
        curr = ts_node_parent(curr);
    }

    if (!info.inClass || ts_node_is_null(funcNode))
    {
        return info;
    }

    TSTreeCursor cursor = ts_tree_cursor_new(funcNode);
    if (ts_tree_cursor_goto_first_child(&cursor))
    {
        do
        {
            TSNode child = ts_tree_cursor_current_node(&cursor);
            const std::string_view ctype = ts_node_type(child);
            if (ctype == "const" || (ctype == parser::nodes::FuncAttributes &&
                                     NodeText(child, sourceCode).find("const") != std::string::npos))
            {
                info.isConstMethod = true;
                break;
            }
        } while (ts_tree_cursor_goto_next_sibling(&cursor));
    }
    ts_tree_cursor_delete(&cursor);
    return info;
}

/**
 * @brief Finds a member variable or property in a class's inheritance hierarchy.
 *
 * @param[in] className Name of the class to search.
 * @param[in] memberName Member name to look for.
 * @param[in] table Workspace symbol table.
 * @return Pointer to VariableSignature if found, nullptr otherwise.
 */
const VariableSignature* FindClassMemberVariable(const std::string& className, const std::string& memberName,
                                                 const SymbolTable& table)
{
    if (className.empty() || memberName.empty())
    {
        return nullptr;
    }
    for (const auto& owner : GetInheritedTypeHierarchy(className, table))
    {
        if (const auto candidates = table.FindSymbolsPtr(owner + "::" + memberName))
        {
            for (const auto& sym : *candidates)
            {
                if (sym.type == SymbolType::Variable || sym.type == SymbolType::Property)
                {
                    return &sym.GetVariable();
                }
            }
        }
    }
    return nullptr;
}

bool IsMemberOfThis(TSNode target, const EnclosingMethodInfo& encInfo, const ConstContext& ctx,
                    std::string& outMemberName);

/**
 * @brief Checks if an identifier or scoped identifier represents a member of 'this'.
 *
 * @param[in] target AST node representing the identifier.
 * @param[in] encInfo Enclosing method information.
 * @param[in] ctx Const context.
 * @param[out] outMemberName Resolved member name if matched.
 * @return True if target represents a member of 'this'.
 */
bool IsIdentifierMemberOfThis(TSNode target, const EnclosingMethodInfo& encInfo, const ConstContext& ctx,
                              std::string& outMemberName)
{
    const std::string text = NodeText(target, ctx.sourceCode);
    const std::string_view baseName = LastScopeSegment(text);
    if (text != baseName)
    {
        const size_t sepPos = text.rfind("::");
        if (sepPos != std::string::npos && text.substr(0, sepPos) != encInfo.className)
        {
            return false;
        }
    }

    const std::string name(baseName);
    if (ctx.scope)
    {
        const Scope* owner = nullptr;
        if (const LocalDefinition* def = ResolveInScope(ctx.scope, name, &owner))
        {
            if (def->kind == LocalDefinitionKind::Field || (owner && owner->kind == ScopeKind::Class))
            {
                outMemberName = name;
                return true;
            }
            return false;
        }
    }
    if (FindClassMemberVariable(encInfo.className, name, ctx.table))
    {
        outMemberName = name;
        return true;
    }
    return false;
}

/**
 * @brief Checks if a member expression represents a mutation of 'this'.
 *
 * @param[in] target AST node representing the member expression.
 * @param[in] encInfo Enclosing method information.
 * @param[in] ctx Const context.
 * @param[out] outMemberName Resolved member name if matched.
 * @return True if target represents a member of 'this'.
 */
bool IsMemberExprOfThis(TSNode target, const EnclosingMethodInfo& encInfo, const ConstContext& ctx,
                        std::string& outMemberName)
{
    TSNode obj = parser::GetChildByField(target, parser::fields::Object);
    TSNode member = parser::GetChildByField(target, parser::fields::Member);
    if (ts_node_is_null(obj) || ts_node_is_null(member))
    {
        return false;
    }

    const std::string_view objType = ts_node_type(obj);
    if (objType == "this_expression" || (objType == "identifier" && NodeText(obj, ctx.sourceCode) == "this"))
    {
        outMemberName = NodeText(member, ctx.sourceCode);
        return true;
    }

    std::string parentMember;
    if (IsMemberOfThis(obj, encInfo, ctx, parentMember))
    {
        bool isHandle = false;
        if (const auto* var = FindClassMemberVariable(encInfo.className, parentMember, ctx.table))
        {
            isHandle = var->modifiers.isHandle;
        }
        else if (ctx.scope)
        {
            if (const auto* def = ResolveInScope(ctx.scope, parentMember))
            {
                isHandle = def->isHandleType;
            }
        }
        if (!isHandle)
        {
            outMemberName = NodeText(member, ctx.sourceCode);
            return true;
        }
    }
    return false;
}

/**
 * @brief Determines if an assignment target refers to a member of 'this' in a const method.
 *
 * @param[in] target Target AST node of the assignment.
 * @param[in] encInfo Enclosing method information.
 * @param[in] ctx Const context.
 * @param[out] outMemberName Name of the modified member.
 * @return True if target modifies 'this' or a member of 'this' in a const context.
 */
bool IsMemberOfThis(TSNode target, const EnclosingMethodInfo& encInfo, const ConstContext& ctx,
                    std::string& outMemberName)
{
    if (!encInfo.inClass || !encInfo.isConstMethod || ts_node_is_null(target))
    {
        return false;
    }

    const std::string_view targetType = ts_node_type(target);
    if (targetType == "this_expression" || (targetType == "identifier" && NodeText(target, ctx.sourceCode) == "this"))
    {
        outMemberName = "this";
        return true;
    }

    if (targetType == "identifier" || targetType == "scoped_identifier")
    {
        return IsIdentifierMemberOfThis(target, encInfo, ctx, outMemberName);
    }

    if (targetType == "member_expression")
    {
        return IsMemberExprOfThis(target, encInfo, ctx, outMemberName);
    }

    return false;
}

Constness ResolveConstness(TSNode node, const ConstContext& ctx, int depth)
{
    // See k_maxAstDepth in ASTUtils.h.
    if (depth > k_maxAstDepth || ts_node_is_null(node))
    {
        return Constness::Unknown;
    }

    const std::string_view nodeType = ts_node_type(node);

    if (nodeType == "this_expression" || (nodeType == "identifier" && NodeText(node, ctx.sourceCode) == "this"))
    {
        const auto encInfo = FindEnclosingMethod(node, ctx.sourceCode);
        return (encInfo.inClass && encInfo.isConstMethod) ? Constness::Const : Constness::Mutable;
    }

    if (nodeType == "identifier")
    {
        return ResolveNameConstness(NodeText(node, ctx.sourceCode), node, ctx);
    }

    if (nodeType == "scoped_identifier")
    {
        // Asked for whole first, the way ResolveExpressionType does, since a qualified name
        // is the key the collector stored it under.
        const std::string whole = NodeText(node, ctx.sourceCode);
        const Constness qualified = ResolveNameConstness(whole, node, ctx);
        if (qualified != Constness::Unknown)
        {
            return qualified;
        }
        return ResolveNameConstness(LastScopeSegment(whole), node, ctx);
    }

    // A member of a const object is const, which is what makes `e.field = 1` through a
    // `const Entity &in` an error. A member of a mutable object is judged on its own
    // declaration - and a const member is already reported where it is declared, so there
    // is nothing to conclude from one here.
    if (nodeType == "member_expression")
    {
        const Constness object =
            ResolveConstness(parser::GetChildByField(node, parser::fields::Object), ctx, depth + 1);
        return object == Constness::Const ? Constness::Const : Constness::Unknown;
    }

    if (nodeType == "parenthesized_expression")
    {
        return ts_node_named_child_count(node) > 0 ? ResolveConstness(ts_node_named_child(node, 0), ctx, depth + 1)
                                                   : Constness::Unknown;
    }

    // An index into a const container, a cast, a call result: each has an answer, and none
    // of them is one this pass can reach without more of the engine's rules than it has.
    // Unknown keeps the caller quiet, which is the only safe direction.
    return Constness::Unknown;
}

/** @brief What the pass concluded about a method name looked up on a type. */
struct MethodLookup
{
    bool found = false; ///< False means no declaration was visible. Stay silent.
    bool hasConstOverload = false;
    bool returnsSelfReference =
        false; ///< True if a declaration returns a reference to its own type (e.g. Type& Method()).
    std::string declaringClass;
};

/**
 * @brief Checks whether a method signature returns a reference to its owning type.
 *
 * Methods returning `Type&` (e.g., `string& ToLowercase()`) are fluent / chaining operations.
 * In Sven Co-op stubs and native AngelScript ecosystems, these signatures are registered
 * returning references and can be invoked on const/read-only parameters for value extraction.
 *
 * @param[in] fn Function signature being inspected.
 * @param[in] owner Name of the owning class.
 * @return True if the method returns a reference to the owning class.
 */
bool IsSelfReferenceReturn(const FunctionSignature& fn, const std::string& owner)
{
    const bool isRef = fn.modifiers.isReturnReference || (!fn.returnType.empty() && fn.returnType.back() == '&');
    return isRef && (CleanBaseType(fn.returnType) == owner || CleanBaseType(fn.returnBaseTypeName) == owner);
}

/**
 * @brief Looks for a method by name across a type's whole visible hierarchy.
 *
 * Answers whether *any* declaration of that name is const, because that is the question the
 * engine asks. Its message - "No matching signatures to 'Entity::Mutate() const'" - is a
 * failed overload lookup, not a refusal of the call, so one const member in the set makes
 * the call legal and this pass silent.
 */
MethodLookup FindMethod(const std::string& typeName, const std::string& methodName, const SymbolTable& table)
{
    MethodLookup result;
    if (typeName.empty() || methodName.empty())
    {
        return result;
    }

    for (const auto& owner : GetInheritedTypeHierarchy(typeName, table))
    {
        const auto candidates = table.FindSymbolsPtr(owner + "::" + methodName);
        if (!candidates)
        {
            continue;
        }

        for (const auto& sym : *candidates)
        {
            if (sym.type != SymbolType::Function || !std::holds_alternative<FunctionSignature>(sym.signature))
            {
                continue;
            }

            if (!result.found)
            {
                result.found = true;
                result.declaringClass = owner;
            }
            const auto& fn = sym.GetFunction();
            result.hasConstOverload = result.hasConstOverload || fn.modifiers.isConst;
            result.returnsSelfReference = result.returnsSelfReference || IsSelfReferenceReturn(fn, owner);
        }
    }
    return result;
}

/**
 * @brief Arguments for constness diagnostic emission.
 */
struct DiagArgs
{
    std::string_view code;
    std::string arg1 = "";
    std::string arg2 = "";
};

void EmitAtNode(TSNode node, DiagnosticContext& ctx, const DiagArgs& diag)
{
    const TSPoint start = ts_node_start_point(node);
    const TSPoint end = ts_node_end_point(node);
    ctx.EmitAtRange({start.row, start.column, end.row, end.column}, diag.code, diag.arg1, diag.arg2);
}

Constness ResolveHandleConstness(TSNode node, const ConstContext& ctx)
{
    if (ts_node_is_null(node))
    {
        return Constness::Unknown;
    }

    const std::string_view nodeType = ts_node_type(node);
    if (nodeType == "identifier" || nodeType == "scoped_identifier")
    {
        const std::string name = NodeText(node, ctx.sourceCode);
        if (ctx.scope)
        {
            if (const LocalDefinition* def = ResolveInScope(ctx.scope, LastScopeSegment(name)))
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
        if (const auto symbols = ctx.table.FindSymbolsPtr(name))
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

void CheckAssignment(TSNode node, const ConstCheckRequest& request, const Scope* scope, DiagnosticContext& ctx)
{
    TSNode target = parser::GetChildByField(node, parser::fields::Left);
    if (ts_node_is_null(target))
    {
        return;
    }

    bool isHandleAssignment = false;
    TSNode actualTarget = target;
    if (std::string_view(ts_node_type(target)) == "unary_expression")
    {
        TSNode opNode = parser::GetChildByField(target, parser::fields::Operator);
        if (!ts_node_is_null(opNode) && NodeText(opNode, request.sourceCode) == "@")
        {
            isHandleAssignment = true;
            TSNode operand = parser::GetChildByField(target, parser::fields::Operand);
            if (!ts_node_is_null(operand))
            {
                actualTarget = operand;
            }
        }
    }

    const ConstContext constCtx{scope, ctx.request.symbolTable, request.sourceCode};
    const auto encInfo = FindEnclosingMethod(node, request.sourceCode);

    std::string memberName;
    if (IsMemberOfThis(actualTarget, encInfo, constCtx, memberName))
    {
        EmitAtNode(target, ctx, {"as-err-readonly-reference", memberName});
        return;
    }

    if (isHandleAssignment)
    {
        if (ResolveHandleConstness(actualTarget, constCtx) == Constness::Const)
        {
            EmitAtNode(target, ctx, {"as-err-const-assignment", NodeTextString(target, request.sourceCode)});
        }
        return;
    }

    if (ResolveConstness(target, constCtx) != Constness::Const)
    {
        return;
    }

    EmitAtNode(target, ctx, {"as-err-const-assignment", NodeTextString(target, request.sourceCode)});
}

void CheckMethodCall(TSNode node, const ConstCheckRequest& request, const Scope* scope, DiagnosticContext& ctx)
{
    TSNode callee = parser::GetChildByField(node, parser::fields::Function);
    if (ts_node_is_null(callee) || std::string_view(ts_node_type(callee)) != "member_expression")
    {
        return;
    }

    TSNode objectNode = parser::GetChildByField(callee, parser::fields::Object);
    TSNode memberNode = parser::GetChildByField(callee, parser::fields::Member);
    if (ts_node_is_null(objectNode) || ts_node_is_null(memberNode))
    {
        return;
    }

    const SymbolTable& table = ctx.request.symbolTable;
    const ConstContext constCtx{scope, table, request.sourceCode};
    if (ResolveConstness(objectNode, constCtx) != Constness::Const)
    {
        return;
    }

    const std::string objectType = CleanBaseType(ResolveExpressionType(objectNode, ExpressionTypeContext(scope, ctx)));
    if (objectType.empty() || !HierarchyIsFullyVisible(objectType, table))
    {
        return;
    }

    const std::string methodName = NodeText(memberNode, request.sourceCode);
    const MethodLookup method = FindMethod(objectType, methodName, table);
    if (!method.found || method.hasConstOverload || method.returnsSelfReference)
    {
        return;
    }

    EmitAtNode(memberNode, ctx,
               {"as-err-const-method-required", std::string(LastScopeSegment(method.declaringClass)), methodName});
}

void VisitNode(TSNode node, const ConstCheckRequest& request, DiagnosticContext& ctx, int depth = 0)
{
    // Pathologically nested source would otherwise recurse until the stack gives out; see
    // k_maxAstDepth in ASTUtils.h.
    if (depth > k_maxAstDepth)
        return;

    const std::string_view nodeType = ts_node_type(node);
    if (nodeType == "assignment_expression" || nodeType == "call_expression")
    {
        const TSPoint start = ts_node_start_point(node);
        const Scope* scope = FindInnermostScope(request.scopeRoot, start.row, start.column);
        if (nodeType == "assignment_expression")
        {
            CheckAssignment(node, request, scope, ctx);
        }
        else
        {
            CheckMethodCall(node, request, scope, ctx);
        }
    }

    const uint32_t childCount = ts_node_named_child_count(node);
    for (uint32_t i = 0; i < childCount; ++i)
    {
        VisitNode(ts_node_named_child(node, i), request, ctx, depth + 1);
    }
}
} // namespace

void CheckConstCorrectness(const ConstCheckRequest& request, DiagnosticContext& ctx)
{
    if (ts_node_is_null(request.root) || request.sourceCode.empty())
    {
        return;
    }

    // A stub describes an API rather than using one, so it has no expressions worth judging -
    // the same exemption every other use-site pass carries.
    if (utils::IsPredefinedFile(ctx.request.fileUri, ctx.request.predefinedFileExtension))
    {
        return;
    }

    if (request.nodeIndex)
    {
        auto assignNodes = request.nodeIndex->Nodes(parser::nodes::AssignmentExpression);
        auto callNodes = request.nodeIndex->Nodes(parser::nodes::CallExpression);
        size_t i = 0;
        size_t j = 0;
        while (i < assignNodes.size() || j < callNodes.size())
        {
            bool takeAssign = false;
            if (i < assignNodes.size() && j < callNodes.size())
            {
                takeAssign = (ts_node_start_byte(assignNodes[i]) <= ts_node_start_byte(callNodes[j]));
            }
            else if (i < assignNodes.size())
            {
                takeAssign = true;
            }

            if (takeAssign)
            {
                TSNode node = assignNodes[i++];
                const TSPoint start = ts_node_start_point(node);
                const Scope* scope = FindInnermostScope(request.scopeRoot, start.row, start.column);
                CheckAssignment(node, request, scope, ctx);
            }
            else
            {
                TSNode node = callNodes[j++];
                const TSPoint start = ts_node_start_point(node);
                const Scope* scope = FindInnermostScope(request.scopeRoot, start.row, start.column);
                CheckMethodCall(node, request, scope, ctx);
            }
        }
        return;
    }

    VisitNode(request.root, request, ctx);
}
} // namespace angel_lsp::analysis
