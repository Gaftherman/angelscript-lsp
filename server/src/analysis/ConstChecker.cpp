#include "analysis/ConstChecker.h"
#include "analysis/NodeIndex.h"
#include "analysis/SemanticHelpers.h"
#include "analysis/TypeSanitization.h"
#include "parser/ASTUtils.h"
#include "utils/Utils.h"

#include "parser/GrammarNames.h"
#include "parser/Keywords.h"
#include "utils/ScopedNameUtils.h"
#include "utils/StopFlag.h"
#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{
/**
 * @brief True when a declared type's written form begins with `const`.
 *
 * The grammar puts `const` first in a type, and both the scope tree and the symbol table
 * keep the type's source text, so this is what a declaration's constness looks like from
 * here. `const Entity@` counts: in AngelScript that is a handle to a read-only object,
 * which is exactly the case the engine refuses a mutation through.
 */
static bool TypeTextIsConst(std::string_view typeText)
{
    return HasLeadingConst(typeText);
}

/**
 * @brief True when a handle type is declared const itself (e.g. `Type@ const`).
 */
static bool TypeTextIsHandleConst(std::string_view typeText)
{
    return HasTrailingConst(typeText);
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

namespace const_checker_detail
{
/** @brief What one expression is, as far as constness goes. */
enum class Constness
{
    Unknown, ///< Nothing could be established. Stay silent.
    Mutable,
    Const
};
} // namespace const_checker_detail

using Constness = const_checker_detail::Constness;

namespace const_checker_detail
{
/**
 * @brief Context bundled for constness resolution queries.
 */
struct ConstContext
{
    const Scope* scope = nullptr;
    const SymbolTable& table;
    std::string_view sourceCode;
    ExpressionTypeCache* cache = nullptr;
};
} // namespace const_checker_detail

using ConstContext = const_checker_detail::ConstContext;

static Constness ResolveConstness(TSNode node, const ConstContext& ctx);

/** @brief Constness of a plain name, whether it is a local, a parameter or a global. */
static Constness ResolveNameConstness(std::string_view name, TSNode node, const ConstContext& ctx)
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

    const auto symbols = ctx.table.FindSymbolsPtr(name);
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

namespace const_checker_detail
{
/**
 * @brief Class and const qualification for enclosing method or accessor.
 * @note [REFACTOR-CANDIDATE] Local helper structure candidate for structural deduplication.
 */
struct EnclosingMethodInfo
{
    bool inClass = false;
    bool isConstMethod = false;
    std::string className;
};
} // namespace const_checker_detail

using EnclosingMethodInfo = const_checker_detail::EnclosingMethodInfo;

/**
 * @brief Identifies enclosing class and whether the enclosing method or accessor is const.
 *
 * @param[in] node AST node inside the method.
 * @param[in] sourceCode Source text.
 * @return EnclosingMethodInfo containing class membership and const qualification.
 * @note [REFACTOR-CANDIDATE] Manually re-evaluates AST for isConst; candidate for direct Pass 1 flag read via
 * TypeExtraction::ExtractTypeInfo / Symbol::modifiers.isConst.
 */
static EnclosingMethodInfo FindEnclosingMethod(TSNode node, std::string_view sourceCode)
{
    EnclosingMethodInfo info;
    const auto& gs = parser::GetGrammarSymbols();
    const std::array<TSSymbol, 2> funcSyms = {gs.symFuncDeclaration, gs.symAccessor};
    TSNode funcNode = parser::FindAncestorWithAnySymbol(node, funcSyms);
    if (ts_node_is_null(funcNode))
    {
        return info;
    }

    TSNode classNode = parser::FindAncestorWithSymbol(funcNode, gs.symClassDeclaration);
    if (!ts_node_is_null(classNode))
    {
        info.inClass = true;
        TSNode nameNode = parser::GetChildByField(classNode, gs.fieldName);
        if (!ts_node_is_null(nameNode))
        {
            info.className = parser::NodeText(nameNode, sourceCode);
        }
    }

    if (!info.inClass)
    {
        return info;
    }

    TSTreeCursor cursor = ts_tree_cursor_new(funcNode);
    if (ts_tree_cursor_goto_first_child(&cursor))
    {
        do
        {
            TSNode child = ts_tree_cursor_current_node(&cursor);
            const TSSymbol sym = ts_node_symbol(child);
            if (sym == gs.tokConst ||
                (sym == gs.symFuncAttributes && parser::NodeText(child, sourceCode).find("const") != std::string::npos))
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
static const VariableSignature* FindClassMemberVariable(const std::string& className, const std::string& memberName,
                                                        const SymbolTable& table, ExpressionTypeCache* cache = nullptr)
{
    if (className.empty() || memberName.empty())
    {
        return nullptr;
    }
    for (const auto& owner : GetInheritedTypeHierarchy(className, table, cache))
    {
        if (const auto candidates = table.FindMemberSymbolPtr(owner, memberName))
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

static bool IsMemberOfThis(TSNode target, const EnclosingMethodInfo& encInfo, const ConstContext& ctx,
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
static bool IsIdentifierMemberOfThis(TSNode target, const EnclosingMethodInfo& encInfo, const ConstContext& ctx,
                                     std::string& outMemberName)
{
    const std::string text = parser::NodeText(target, ctx.sourceCode);
    const auto [prefix, baseName] = utils::SplitScopedName(text);
    if (!prefix.empty() && prefix != encInfo.className)
    {
        return false;
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
    if (FindClassMemberVariable(encInfo.className, name, ctx.table, ctx.cache))
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
 * @note [REFACTOR-CANDIDATE] Manually re-evaluates AST for isHandle; candidate for direct Pass 1 flag read via
 * Symbol::modifiers.isHandle / LocalDefinition::isHandleType.
 */
static bool IsMemberExprOfThis(TSNode target, const EnclosingMethodInfo& encInfo, const ConstContext& ctx,
                               std::string& outMemberName)
{
    TSNode obj = parser::GetChildByField(target, parser::GetGrammarSymbols().fieldObject);
    TSNode member = parser::GetChildByField(target, parser::GetGrammarSymbols().fieldMember);
    if (ts_node_is_null(obj) || ts_node_is_null(member))
    {
        return false;
    }

    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol objSym = ts_node_symbol(obj);
    if (objSym == gs.symThisExpression ||
        (objSym == gs.symIdentifier && parser::NodeText(obj, ctx.sourceCode) == parser::keywords::This))
    {
        outMemberName = parser::NodeText(member, ctx.sourceCode);
        return true;
    }

    std::string parentMember;
    if (IsMemberOfThis(obj, encInfo, ctx, parentMember))
    {
        bool isHandle = false;
        if (const auto* var = FindClassMemberVariable(encInfo.className, parentMember, ctx.table, ctx.cache))
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
            outMemberName = parser::NodeText(member, ctx.sourceCode);
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
static bool IsMemberOfThis(TSNode target, const EnclosingMethodInfo& encInfo, const ConstContext& ctx,
                           std::string& outMemberName)
{
    if (!encInfo.inClass || !encInfo.isConstMethod || ts_node_is_null(target))
    {
        return false;
    }

    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol targetSym = ts_node_symbol(target);
    if (targetSym == gs.symThisExpression ||
        (targetSym == gs.symIdentifier && parser::NodeText(target, ctx.sourceCode) == parser::keywords::This))
    {
        outMemberName = parser::keywords::This;
        return true;
    }

    if (targetSym == gs.symIdentifier || targetSym == gs.symScopedIdentifier)
    {
        return IsIdentifierMemberOfThis(target, encInfo, ctx, outMemberName);
    }

    if (targetSym == gs.symMemberExpression)
    {
        return IsMemberExprOfThis(target, encInfo, ctx, outMemberName);
    }

    return false;
}

static Constness ResolveLeafConstness(TSNode node, const ConstContext& ctx)
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
        const std::string whole = parser::NodeText(node, ctx.sourceCode);
        const Constness qualified = ResolveNameConstness(whole, node, ctx);
        if (qualified != Constness::Unknown)
        {
            return qualified;
        }
        return ResolveNameConstness(LastScopeSegment(whole), node, ctx);
    }

    return Constness::Unknown;
}

static Constness ResolveConstness(TSNode node, const ConstContext& ctx)
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

namespace const_checker_detail
{
/**
 * @brief What the pass concluded about a method name looked up on a type.
 */
struct MethodLookup
{
    bool found = false; ///< False means no declaration was visible. Stay silent.
    bool hasConstOverload = false;
    bool returnsSelfReference =
        false; ///< True if a declaration returns a reference to its own type (e.g. Type& Method()).
    std::string declaringClass;
};
} // namespace const_checker_detail

using MethodLookup = const_checker_detail::MethodLookup;

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
 * @note [REFACTOR-CANDIDATE] Manually re-evaluates AST for isRef; candidate for direct Pass 1 flag read via
 * TypeExtraction::ExtractTypeInfo / ParameterInformation::isReference / isReturnReference.
 */
static bool IsSelfReferenceReturn(const FunctionSignature& fn, const std::string& owner)
{
    const bool isRef = fn.modifiers.isReturnReference || IsReferenceTypeView(fn.returnType);
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
static MethodLookup FindMethod(const std::string& typeName, const std::string& methodName, const SymbolTable& table,
                               ExpressionTypeCache* cache = nullptr)
{
    MethodLookup result;
    if (typeName.empty() || methodName.empty())
    {
        return result;
    }

    for (const auto& owner : GetInheritedTypeHierarchy(typeName, table, cache))
    {
        const auto candidates = table.FindMemberSymbolPtr(owner, methodName);
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

static Constness ResolveHandleConstness(TSNode node, const ConstContext& ctx)
{
    if (ts_node_is_null(node))
    {
        return Constness::Unknown;
    }

    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol nodeSym = ts_node_symbol(node);
    if (nodeSym == gs.symIdentifier || nodeSym == gs.symScopedIdentifier)
    {
        const std::string name = parser::NodeText(node, ctx.sourceCode);
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

static void CheckAssignment(TSNode node, const ConstCheckRequest& request, const Scope* scope, DiagnosticContext& ctx)
{
    TSNode target = parser::GetChildByField(node, parser::GetGrammarSymbols().fieldLeft);
    if (ts_node_is_null(target))
    {
        return;
    }

    bool isHandleAssignment = false;
    TSNode actualTarget = target;
    const auto& gs = parser::GetGrammarSymbols();
    if (ts_node_symbol(target) == gs.symUnaryExpression)
    {
        TSNode opNode = parser::GetChildByField(target, gs.fieldOperator);
        if (!ts_node_is_null(opNode) && ts_node_symbol(opNode) == gs.tokAt)
        {
            isHandleAssignment = true;
            TSNode operand = parser::GetChildByField(target, gs.fieldOperand);
            if (!ts_node_is_null(operand))
            {
                actualTarget = operand;
            }
        }
    }

    const ConstContext constCtx{scope, ctx.request.symbolTable, request.sourceCode, ctx.request.exprCache};
    const auto encInfo = FindEnclosingMethod(node, request.sourceCode);

    std::string memberName;
    if (IsMemberOfThis(actualTarget, encInfo, constCtx, memberName))
    {
        ctx.EmitAtNode(target, "as-err-readonly-reference", memberName);
        return;
    }

    if (isHandleAssignment)
    {
        if (ResolveHandleConstness(actualTarget, constCtx) == Constness::Const)
        {
            ctx.EmitAtNode(target, "as-err-const-assignment", parser::NodeTextString(target, request.sourceCode));
        }
        return;
    }

    if (ResolveConstness(target, constCtx) != Constness::Const)
    {
        return;
    }

    ctx.EmitAtNode(target, "as-err-const-assignment", parser::NodeTextString(target, request.sourceCode));
}

static void CheckMethodCall(TSNode node, const ConstCheckRequest& request, const Scope* scope, DiagnosticContext& ctx)
{
    const auto& gs = parser::GetGrammarSymbols();
    TSNode callee = parser::GetChildByField(node, gs.fieldFunction);
    if (ts_node_is_null(callee) || ts_node_symbol(callee) != gs.symMemberExpression)
    {
        return;
    }

    TSNode objectNode = parser::GetChildByField(callee, gs.fieldObject);
    TSNode memberNode = parser::GetChildByField(callee, gs.fieldMember);
    if (ts_node_is_null(objectNode) || ts_node_is_null(memberNode))
    {
        return;
    }

    const SymbolTable& table = ctx.request.symbolTable;
    const ConstContext constCtx{scope, table, request.sourceCode, ctx.request.exprCache};
    if (ResolveConstness(objectNode, constCtx) != Constness::Const)
    {
        return;
    }

    const std::string objectType = CleanBaseType(ResolveExpressionType(objectNode, ExpressionTypeContext(scope, ctx)));
    if (objectType.empty() || !HierarchyIsFullyVisible(objectType, table))
    {
        return;
    }

    const std::string methodName = parser::NodeText(memberNode, request.sourceCode);
    const MethodLookup method = FindMethod(objectType, methodName, table, ctx.request.exprCache);
    if (!method.found || method.hasConstOverload || method.returnsSelfReference)
    {
        return;
    }

    ctx.EmitAtNode(memberNode, "as-err-const-method-required", std::string(LastScopeSegment(method.declaringClass)),
                   methodName);
}

static void CheckConstCorrectnessIndexed(const ConstCheckRequest& request, DiagnosticContext& ctx)
{
    if (ctx.request.stopFlag && ctx.request.stopFlag->stop_requested())
    {
        return;
    }

    const auto& gs = parser::GetGrammarSymbols();
    const std::array<TSSymbol, 2> kSymbols = {gs.symAssignmentExpression, gs.symCallExpression};
    request.nodeIndex->ForEachNodeOrdered(std::span<const TSSymbol>(kSymbols.data(), kSymbols.size()),
                                          [&](TSNode node)
                                          {
                                              if (ctx.request.stopFlag && ctx.request.stopFlag->stop_requested())
                                              {
                                                  return;
                                              }
                                              const TSPoint start = ts_node_start_point(node);
                                              const Scope* scope =
                                                  FindInnermostScope(request.scopeRoot, start.row, start.column);
                                              if (ts_node_symbol(node) == gs.symAssignmentExpression)
                                              {
                                                  CheckAssignment(node, request, scope, ctx);
                                              }
                                              else
                                              {
                                                  CheckMethodCall(node, request, scope, ctx);
                                              }
                                          });
}

/**
 * @brief Check const correctness.
 * @param[in] request Contextual request bundle containing AST root, source text, and parameters.
 * @param[in,out] ctx Diagnostic collection and analysis context.
 */
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
        CheckConstCorrectnessIndexed(request, ctx);
        return;
    }

    NodeIndex localIndex(request.root);
    ConstCheckRequest indexedRequest = request;
    indexedRequest.nodeIndex = &localIndex;
    CheckConstCorrectnessIndexed(indexedRequest, ctx);
}
} // namespace angel_lsp::analysis
