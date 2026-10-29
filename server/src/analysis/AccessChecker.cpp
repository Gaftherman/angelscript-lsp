#include "analysis/AccessChecker.h"
#include "analysis/ASTUtils.h"
#include "analysis/NodeIndex.h"
#include "analysis/ScopeTree.h"
#include "analysis/SemanticHelpers.h"
#include "analysis/rules/RuleIndex.h"
#include "utils/Constants.h"
#include "utils/Utils.h"

#include "analysis/DiagnosticCodes.h"
#include "parser/GrammarNames.h"
#include "parser/Keywords.h"
#include "utils/StopFlag.h"
#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{
namespace
{
/** @brief Compares two type names by their last segment, so a qualification cannot hide a match. */
bool IsSameType(const std::string& a, const std::string& b)
{
    return !a.empty() && (a == b || LastScopeSegment(a) == LastScopeSegment(b));
}

/** @brief True when derived is base, or reaches it through its declared base chain. */
bool DerivesFrom(const std::string& derived, const std::string& base, const SymbolTable& table)
{
    if (IsSameType(derived, base))
    {
        return true;
    }
    for (const auto& ancestor : GetInheritedTypeHierarchy(derived, table))
    {
        if (IsSameType(ancestor, base))
        {
            return true;
        }
    }
    return false;
}

/** @brief The access modifier a declaration carries, whatever kind of declaration it is. */
bool TryReadAccess(const Symbol& sym, AccessModifier& access)
{
    if (std::holds_alternative<FunctionSignature>(sym.signature))
    {
        access = sym.GetFunction().modifiers.access;
        return true;
    }
    if (std::holds_alternative<VariableSignature>(sym.signature))
    {
        access = sym.GetVariable().modifiers.access;
        return true;
    }
    return false;
}

/** @brief What the pass concluded about one `object.member` pair. */
struct MemberAccess
{
    bool found = false;   ///< True if any member of this name exists in the hierarchy.
    bool decided = false; ///< True if an access restriction (private/protected) was found.
    AccessModifier access = AccessModifier::Public;
    std::string declaringClass;

    /**
     * @brief True when the name resolved through a `get_`/`set_` pair, not a real member.
     *
     * Recorded because whether that pair *is* a property is the host's decision, not the
     * script's: under asEP_PROPERTY_ACCESSOR_MODE 0 and 1 a script accessor is not one, and
     * `c.X` does not compile. Resolution deliberately still succeeds there - see
     * SemanticAnalysisRequest::ScriptAccessorsAreProperties - and this is what lets the
     * disagreement be reported as a hint instead of by making the member disappear.
     */
    bool viaAccessor = false;
};

/**
 * @brief Finds the declaration a member name reaches through a type's inheritance chain.
 *
 * Resolves to the least restrictive candidate rather than the first one. A name can carry
 * overloads with different access - a public `Fire(int)` beside a private `Fire()` is
 * ordinary - and an access rule that picked the private one would report a call the engine
 * accepts. Public anywhere in the chain therefore ends the search.
 *
 * A member reached through a mixin is attributed to the object's own type rather than to
 * the mixin, because that is where it actually ends up: including a mixin copies its
 * members into the including class, so `private string Name` in `mixin class NameGetter`
 * becomes a private member of every class that includes it. Reading it as the mixin's own
 * was this rule's only false positive over the corpus, and it hit `this.Name = name;` in a
 * constructor - as ordinary a line as the corpus contains.
 */
/**
 * @brief True when `get_X`/`set_X` may stand in for the member `X`.
 *
 * asEP_PROPERTY_ACCESSOR_MODE decides it, and the two settings really do accept different
 * programs. Under mode 2 any method named `get_X` is the property `X`; under mode 3 - the
 * SDK's own default - it is an ordinary method until the `property` keyword is written, and
 * `c.V` is answered with "'V' is not a member of 'C'". Under mode 3, accessors require
 * the explicit 'property' keyword decoration, whereas mode 2 accepts implicit accessor methods.
 */
bool AccessorStandsForProperty(const Symbol& sym, bool keywordRequired)
{
    if (!keywordRequired)
    {
        return true;
    }
    return std::holds_alternative<FunctionSignature>(sym.signature) && sym.GetFunction().modifiers.isProperty;
}

/**
 * @brief Context bundled for member lookup across type hierarchies.
 */
struct MemberLookupContext
{
    std::string typeName;
    const SymbolTable& table;
    bool accessorKeywordRequired = false;
};

/**
 * @brief Collects symbols matching the member name directly on the specified owner type.
 *
 * @param[in] owner Type name owning the candidate member.
 * @param[in] memberName Member name to search.
 * @param[in] ctx Lookup context bundling type hierarchy and symbol table.
 * @param[out] viaAccessor Set to true if match was via getter/setter accessor.
 * @return Vector of matched symbols.
 */
std::vector<Symbol> CollectDirectCandidates(const std::string& owner, const std::string& memberName,
                                            const MemberLookupContext& ctx, bool& viaAccessor)
{
    std::vector<Symbol> candidates;
    auto candidatesPtr = ctx.table.FindMemberSymbolPtr(owner, memberName);
    if (candidatesPtr && !candidatesPtr->empty())
    {
        candidates.insert(candidates.end(), candidatesPtr->begin(), candidatesPtr->end());
        const bool hasInstance = std::any_of(candidates.begin(), candidates.end(),
                                             [](const Symbol& s)
                                             {
                                                 return !(s.type == SymbolType::Variable &&
                                                          std::holds_alternative<VariableSignature>(s.signature) &&
                                                          s.GetVariable().isEnumConstant);
                                             });
        if (hasInstance)
        {
            std::stable_partition(candidates.begin(), candidates.end(),
                                  [](const Symbol& s)
                                  {
                                      return !(s.type == SymbolType::Variable &&
                                               std::holds_alternative<VariableSignature>(s.signature) &&
                                               s.GetVariable().isEnumConstant);
                                  });
            return candidates;
        }
    }

    for (const auto& accessorPrefix : {"get_", "set_"})
    {
        auto accSyms = ctx.table.FindMemberSymbolPtr(owner, std::string(accessorPrefix) + memberName);
        if (!accSyms)
        {
            continue;
        }
        for (const auto& sym : *accSyms)
        {
            if (AccessorStandsForProperty(sym, ctx.accessorKeywordRequired))
            {
                viaAccessor = true;
                candidates.push_back(sym);
            }
        }
    }
    std::stable_partition(candidates.begin(), candidates.end(),
                          [](const Symbol& s)
                          {
                              return !(s.type == SymbolType::Variable &&
                                       std::holds_alternative<VariableSignature>(s.signature) &&
                                       s.GetVariable().isEnumConstant);
                          });
    return candidates;
}

/**
 * @brief Collects symbols matching the member name via container matching on the specified owner type.
 *
 * @param[in] owner Type name owning the candidate member.
 * @param[in] memberName Member name to search.
 * @param[in] ctx Lookup context bundling type hierarchy and symbol table.
 * @param[out] viaAccessor Set to true if match was via getter/setter accessor.
 * @return Vector of matched symbols.
 */
std::vector<Symbol> CollectContainerCandidates(const std::string& owner, const std::string& memberName,
                                               const MemberLookupContext& ctx, bool& viaAccessor)
{
    std::vector<Symbol> candidates;
    if (auto memberSyms = ctx.table.FindSymbolsPtr(memberName))
    {
        for (const auto& sym : *memberSyms)
        {
            if (IsSameType(sym.containerName, owner))
            {
                candidates.push_back(sym);
            }
        }
    }
    if (!candidates.empty())
    {
        return candidates;
    }

    for (const auto& accessorPrefix : {"get_", "set_"})
    {
        if (auto syms = ctx.table.FindSymbolsPtr(accessorPrefix + memberName))
        {
            for (const auto& sym : *syms)
            {
                if (IsSameType(sym.containerName, owner) && AccessorStandsForProperty(sym, ctx.accessorKeywordRequired))
                {
                    viaAccessor = true;
                    candidates.push_back(sym);
                }
            }
        }
    }
    return candidates;
}

/**
 * @brief Collects all candidate symbols for a member name on an owner type.
 *
 * @param[in] owner Type name owning the candidate member.
 * @param[in] memberName Member name to search.
 * @param[in] ctx Lookup context bundling type hierarchy and symbol table.
 * @param[out] viaAccessor Set to true if match was via getter/setter accessor.
 * @return Vector of candidate symbols.
 */
std::vector<Symbol> CollectOwnerCandidates(const std::string& owner, const std::string& memberName,
                                           const MemberLookupContext& ctx, bool& viaAccessor)
{
    auto candidates = CollectDirectCandidates(owner, memberName, ctx, viaAccessor);
    if (candidates.empty())
    {
        candidates = CollectContainerCandidates(owner, memberName, ctx, viaAccessor);
    }
    return candidates;
}

/**
 * @brief Evaluates access for candidate symbols, updating the resolved MemberAccess result.
 *
 * @param[in] candidates Candidate symbols found for the owner.
 * @param[in] owner Owner type name.
 * @param[in] ctx Lookup context bundling type hierarchy and symbol table.
 * @param[in,out] result Member access state to update.
 * @return True if resolution concluded (public access reached).
 */
bool ApplyCandidatesAccess(const std::vector<Symbol>& candidates, const std::string& owner,
                           const MemberLookupContext& ctx, MemberAccess& result)
{
    for (const auto& sym : candidates)
    {
        AccessModifier access = AccessModifier::Public;
        if (!TryReadAccess(sym, access))
        {
            continue;
        }

        result.found = true;

        if (access == AccessModifier::Public)
        {
            result.decided = false;
            result.access = AccessModifier::Public;
            result.declaringClass = IsMixinClass(owner, ctx.table) ? ctx.typeName : owner;
            return true;
        }

        if (!result.decided || (result.access == AccessModifier::Private && access == AccessModifier::Protected))
        {
            result.decided = true;
            result.access = access;
            result.declaringClass = IsMixinClass(owner, ctx.table) ? ctx.typeName : owner;
        }
    }
    return false;
}

MemberAccess FindMember(const std::string& typeName, const std::string& memberName, const SymbolTable& table,
                        bool accessorKeywordRequired)
{
    MemberAccess result;
    if (typeName.empty() || memberName.empty())
    {
        return result;
    }

    const auto ruleIndex = table.GetRuleIndex();
    const MemberLookupContext ctx{typeName, table, accessorKeywordRequired};
    for (const auto& owner : GetInheritedTypeHierarchy(typeName, table))
    {
        if (ruleIndex)
        {
            const auto& members = ruleIndex->Members(owner);
            const bool hasPotential = members.allMemberNames.contains(memberName) ||
                                      members.allMemberNames.contains("get_" + memberName) ||
                                      members.allMemberNames.contains("set_" + memberName);
            if (!hasPotential)
            {
                continue;
            }
        }
        auto candidates = CollectOwnerCandidates(owner, memberName, ctx, result.viaAccessor);
        if (ApplyCandidatesAccess(candidates, owner, ctx, result))
        {
            return result;
        }
    }

    return result;
}

/** @brief Innermost class body enclosing a node, or empty when the node sits outside one. */
std::string EnclosingClass(TSNode node, std::string_view sourceCode, bool& insideMixin, const SymbolTable& table)
{
    insideMixin = false;
    TSNode curr = parser::GetParent(node);
    size_t depth = 0;
    const auto& gs = parser::GetGrammarSymbols();
    while (!ts_node_is_null(curr) && ++depth <= constants::k_maxAstClimbDepth)
    {
        const TSSymbol sym = ts_node_symbol(curr);
        if (sym == gs.symLambdaExpression)
        {
            return "";
        }
        if (sym == gs.symClassDeclaration)
        {
            TSNode nameNode = parser::GetChildByField(curr, gs.fieldName);
            if (!ts_node_is_null(nameNode))
            {
                std::string className = NodeText(nameNode, sourceCode);
                const auto symbols = table.FindSymbolsPtr(className);
                if (symbols)
                {
                    insideMixin =
                        std::any_of(symbols->begin(), symbols->end(), [](const Symbol& sym)
                                    { return sym.type == SymbolType::Class && sym.GetClass().modifiers.isMixin; });
                }
                return className;
            }
        }
        curr = parser::GetParent(curr);
    }
    return "";
}

/**
 * @brief Resolves the owning type of a member access object expression.
 *
 * @param[in] objectNode AST node of the object expression.
 * @param[in] scope Current lexical scope.
 * @param[in] ctx Diagnostic context.
 * @return Normalized owner type name.
 */
std::string ResolveObjectOwnerType(TSNode objectNode, const Scope* scope, const DiagnosticContext& ctx)
{
    const std::string_view arrayContainer = ctx.request.GetEffectiveArrayTypeName();
    return MemberOwnerType(ResolveExpressionType(objectNode, ExpressionTypeContext(scope, ctx)), arrayContainer);
}

/**
 * @brief Emits a hint diagnostic if access was made via a disabled accessor.
 *
 * @param[in] memberNode AST node of the member being accessed.
 * @param[in] member Member resolution outcome.
 * @param[in] objectType Object type name.
 * @param[in,out] ctx Diagnostic context.
 */
void MaybeEmitAccessorHint(TSNode memberNode, const MemberAccess& member, const std::string& objectType,
                           DiagnosticContext& ctx)
{
    if (member.viaAccessor && !ctx.request.ScriptAccessorsAreProperties() && ctx.request.diagnostics &&
        ctx.request.diagnostics->reportAccessorDisabled)
    {
        const TSPoint hintStart = ts_node_start_point(memberNode);
        const TSPoint hintEnd = ts_node_end_point(memberNode);
        ctx.EmitAtRange({hintStart.row, hintStart.column, hintEnd.row, hintEnd.column}, "as-hint-accessor-disabled",
                        {objectType, NodeTextString(memberNode, ctx.request.sourceCode)}, DiagnosticSeverity::Hint);
    }
}

/**
 * @brief Determines whether a member access is permitted based on privacy/protection rules.
 *
 * @param[in] member Resolved member access descriptor.
 * @param[in] accessingClass Enclosing class making the access.
 * @param[in] objectType Type of the target object expression.
 * @param[in] ctx Diagnostic context.
 * @return True if member access is allowed, false if access violation.
 */
bool IsMemberAccessAllowed(const MemberAccess& member, const std::string& accessingClass, const std::string& objectType,
                           const DiagnosticContext& ctx)
{
    const bool declaredPrivate = member.access == AccessModifier::Private;
    if (declaredPrivate && !ctx.request.TreatsPrivateAsProtected())
    {
        return IsSameType(accessingClass, member.declaringClass);
    }
    return !accessingClass.empty() && DerivesFrom(accessingClass, member.declaringClass, ctx.request.symbolTable) &&
           DerivesFrom(objectType, accessingClass, ctx.request.symbolTable);
}

/**
 * @brief Emits a private or protected access violation diagnostic for a member access.
 *
 * @param[in] memberNode AST node of the member being accessed.
 * @param[in] member Member resolution outcome.
 * @param[in] memberName Name of the member being accessed.
 * @param[in,out] ctx Diagnostic context.
 */
void EmitPrivacyViolation(TSNode memberNode, const MemberAccess& member, std::string_view memberName,
                          DiagnosticContext& ctx)
{
    const TSPoint start = ts_node_start_point(memberNode);
    const TSPoint end = ts_node_end_point(memberNode);
    const bool declaredPrivate = member.access == AccessModifier::Private;
    ctx.EmitAtRange({start.row, start.column, end.row, end.column},
                    declaredPrivate ? diagnostics::codes::PrivateMemberAccess
                                    : diagnostics::codes::ProtectedMemberAccess,
                    memberName, member.declaringClass);
}

void CheckMemberExpression(TSNode node, const AccessCheckRequest& request, const Scope* scope, DiagnosticContext& ctx)
{
    TSNode objectNode = parser::GetChildByField(node, parser::GetGrammarSymbols().fieldObject);
    TSNode memberNode = parser::GetChildByField(node, parser::GetGrammarSymbols().fieldMember);
    if (ts_node_is_null(objectNode) || ts_node_is_null(memberNode))
    {
        return;
    }

    const SymbolTable& table = ctx.request.symbolTable;
    const std::string objectType = ResolveObjectOwnerType(objectNode, scope, ctx);
    if (objectType.empty() || objectType == "auto")
    {
        return;
    }

    const std::string memberName = NodeText(memberNode, request.sourceCode);
    if (memberName.empty())
    {
        return;
    }

    if (parser::keywords::IsReserved(memberName))
    {
        const TSPoint start = ts_node_start_point(memberNode);
        const TSPoint end = ts_node_end_point(memberNode);
        ctx.EmitAtRange({start.row, start.column, end.row, end.column}, diagnostics::codes::ReservedKeywordName,
                        memberName);
        return;
    }

    if ((IsCorePrimitive(objectType) && objectType != "auto") || objectType == "void")
    {
        const TSPoint start = ts_node_start_point(memberNode);
        const TSPoint end = ts_node_end_point(memberNode);
        ctx.EmitAtRange({start.row, start.column, end.row, end.column}, diagnostics::codes::MemberNotFound, objectType,
                        memberName);
        return;
    }

    if (!HierarchyIsFullyVisible(objectType, table))
    {
        return;
    }

    const MemberAccess member = FindMember(objectType, memberName, table, ctx.request.RequiresAccessorKeyword());
    if (!member.found)
    {
        const TSPoint start = ts_node_start_point(memberNode);
        const TSPoint end = ts_node_end_point(memberNode);
        ctx.EmitAtRange({start.row, start.column, end.row, end.column}, diagnostics::codes::MemberNotFound, objectType,
                        memberName);
        return;
    }

    MaybeEmitAccessorHint(memberNode, member, objectType, ctx);

    if (member.decided)
    {
        bool insideMixin = false;
        const std::string accessingClass = EnclosingClass(node, request.sourceCode, insideMixin, table);
        if (!IsMemberAccessAllowed(member, accessingClass, objectType, ctx))
        {
            EmitPrivacyViolation(memberNode, member, memberName, ctx);
        }
    }
}

/**
 * @brief Checks if an AST node type corresponds to a declaration name node.
 *
 * @param[in] type AST node type name.
 * @return True if node type defines a declared symbol name.
 */
bool IsDeclarationNodeType(TSSymbol sym)
{
    const auto& gs = parser::GetGrammarSymbols();
    return sym == gs.symVariableDeclarator || sym == gs.symParameter || sym == gs.symFuncDeclaration ||
           sym == gs.symClassDeclaration || sym == gs.symInterfaceDeclaration || sym == gs.symEnumDeclaration ||
           sym == gs.symEnumMember || sym == gs.symVirtualProperty || sym == gs.symTypedefDeclaration ||
           sym == gs.symFuncdefDeclaration || sym == gs.symImportDeclaration || sym == gs.symMixinDeclaration;
}

/**
 * @brief Checks if an AST node is a type reference or comment node.
 *
 * @param[in] sym Grammar symbol ID.
 * @return True if node is a type reference or comment.
 */
bool IsTypeRefOrComment(TSSymbol sym)
{
    const auto& gs = parser::GetGrammarSymbols();
    return sym == gs.symDatatype || sym == gs.symPrimitiveType || sym == gs.symBaseClassList || sym == gs.symComment;
}

/**
 * @brief Checks whether an identifier AST node should be skipped during access validation.
 *
 * @param[in] node AST node to check.
 * @param[in] parent Parent AST node.
 * @return True if the node is a declaration, type reference or irrelevant context.
 */
bool ShouldSkipIdentifierNode(TSNode node, TSNode parent)
{
    if (ts_node_is_null(parent))
    {
        return true;
    }

    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol parentSym = ts_node_symbol(parent);
    if (parentSym == gs.symMemberExpression)
    {
        TSNode memberField = parser::GetChildByField(parent, gs.fieldMember);
        return ts_node_eq(node, memberField);
    }

    if (IsDeclarationNodeType(parentSym))
    {
        TSNode nameField = parser::GetChildByField(parent, gs.fieldName);
        return ts_node_eq(node, nameField);
    }

    if (IsTypeRefOrComment(parentSym))
    {
        return true;
    }

    return ts_node_symbol(node) == gs.symIdentifier && parentSym == gs.symScopedIdentifier;
}

/**
 * @brief Extracts trimmed text view from an identifier AST node without heap allocation.
 *
 * @param[in] node AST node of the identifier.
 * @param[in] sourceCode Document source text.
 * @return Clean trimmed identifier string_view.
 */
std::string_view ExtractCleanIdentifier(TSNode node, std::string_view sourceCode)
{
    std::string_view idText = GetNodeTextView(node, sourceCode);
    while (!idText.empty() && isspace(static_cast<unsigned char>(idText.front())))
    {
        idText.remove_prefix(1);
    }
    while (!idText.empty() && isspace(static_cast<unsigned char>(idText.back())))
    {
        idText.remove_suffix(1);
    }
    return idText;
}

/**
 * @brief Checks if a name is declared within the closure's local scope chain.
 *
 * @param[in] scope Current inner scope.
 * @param[in] enclosingClosure Enclosing closure scope.
 * @param[in] idText Name to check.
 * @return True if declared within closure.
 */
bool IsDeclaredInClosure(const Scope* scope, const Scope* enclosingClosure, std::string_view idText)
{
    for (const Scope* s = scope; s != nullptr; s = s->parent)
    {
        for (const auto& def : s->definitions)
        {
            if (def.name == idText)
            {
                return true;
            }
        }
        if (s == enclosingClosure)
        {
            break;
        }
    }
    return false;
}

/**
 * @brief Checks if a name is declared as a local variable or parameter in outer non-class scopes.
 *
 * @param[in] enclosingClosure Enclosing closure scope.
 * @param[in] idText Name to check.
 * @return True if declared in an outer function scope.
 */
bool IsOuterLocalOrParameter(const Scope* enclosingClosure, std::string_view idText)
{
    for (const Scope* s = enclosingClosure->parent; s != nullptr; s = s->parent)
    {
        if (s->kind == ScopeKind::Class || s->kind == ScopeKind::Namespace || s->kind == ScopeKind::Global)
        {
            break;
        }
        for (const auto& def : s->definitions)
        {
            if (def.name == idText &&
                (def.kind == LocalDefinitionKind::Variable || def.kind == LocalDefinitionKind::Parameter))
            {
                return true;
            }
        }
    }
    return false;
}

/**
 * @brief Finds the innermost class enclosing the closure containing node, if any.
 *
 * @param[in] node AST node inside the closure.
 * @param[in] sourceCode Document source text.
 * @return Name of the outer class enclosing the lambda, or empty if none.
 */
std::string EnclosingClassOfLambda(TSNode node, std::string_view sourceCode)
{
    TSNode curr = parser::GetParent(node);
    size_t depth = 0;
    bool foundLambda = false;
    const auto& gs = parser::GetGrammarSymbols();
    while (!ts_node_is_null(curr) && ++depth <= constants::k_maxAstClimbDepth)
    {
        const TSSymbol sym = ts_node_symbol(curr);
        if (sym == gs.symLambdaExpression)
        {
            foundLambda = true;
        }
        else if (foundLambda && sym == gs.symClassDeclaration)
        {
            TSNode nameNode = parser::GetChildByField(curr, gs.fieldName);
            if (!ts_node_is_null(nameNode))
            {
                return NodeText(nameNode, sourceCode);
            }
        }
        curr = parser::GetParent(curr);
    }
    return "";
}

/**
 * @brief Checks whether an identifier attempts forbidden outer variable capture in a closure.
 *
 * @param[in] node AST identifier node.
 * @param[in] scope Current lexical scope.
 * @param[in] idText Identifier name.
 * @param[in,out] ctx Diagnostic context.
 * @return True if an error diagnostic was emitted, false otherwise.
 */
bool CheckClosureDisallowedAccess(TSNode node, const Scope* scope, std::string_view idText, DiagnosticContext& ctx)
{
    const Scope* enclosingClosure = FindEnclosingClosure(scope);
    if (!enclosingClosure)
    {
        return false;
    }

    if (idText == "this" || idText == "super")
    {
        const TSPoint start = ts_node_start_point(node);
        const TSPoint end = ts_node_end_point(node);
        ctx.EmitAtRange({start.row, start.column, end.row, end.column}, diagnostics::codes::LambdaThisDisallowed);
        return true;
    }

    if (IsDeclaredInClosure(scope, enclosingClosure, idText))
    {
        return false;
    }

    if (IsOuterLocalOrParameter(enclosingClosure, idText))
    {
        const TSPoint start = ts_node_start_point(node);
        const TSPoint end = ts_node_end_point(node);
        ctx.EmitAtRange({start.row, start.column, end.row, end.column}, diagnostics::codes::LambdaClosureDisallowed,
                        idText);
        return true;
    }

    const std::string outerClass = EnclosingClassOfLambda(node, ctx.request.sourceCode);
    if (!outerClass.empty())
    {
        const MemberAccess member = FindMember(outerClass, std::string(idText), ctx.request.symbolTable, false);
        if (member.found)
        {
            const TSPoint start = ts_node_start_point(node);
            const TSPoint end = ts_node_end_point(node);
            ctx.EmitAtRange({start.row, start.column, end.row, end.column}, diagnostics::codes::LambdaMemberAccess,
                            idText);
            return true;
        }
    }

    return false;
}

/**
 * @brief Checks if the lexical scope chain includes a class scope.
 *
 * @param[in] scope Lexical scope to inspect.
 * @return True if scope is null (unknown) or has an enclosing ScopeKind::Class.
 */
bool HasEnclosingClassScope(const Scope* scope)
{
    if (!scope)
    {
        return true;
    }
    for (const Scope* s = scope; s != nullptr; s = s->parent)
    {
        if (s->kind == ScopeKind::Class)
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief Validates implicit member access on an identifier within an enclosing class.
 *
 * @param[in] node AST identifier node.
 * @param[in] scope Current lexical scope.
 * @param[in] idText Identifier name.
 * @param[in,out] ctx Diagnostic context.
 */
void CheckImplicitMemberAccess(TSNode node, const Scope* scope, std::string_view idText, DiagnosticContext& ctx)
{
    if (!HasEnclosingClassScope(scope))
    {
        return;
    }

    const LocalDefinition* localDef = ResolveInScopeAt(scope, idText, ts_node_start_byte(node));
    if (localDef &&
        (localDef->kind == LocalDefinitionKind::Variable || localDef->kind == LocalDefinitionKind::Parameter))
    {
        return;
    }

    const SymbolTable& table = ctx.request.symbolTable;

    bool insideMixin = false;
    const std::string accessingClass = EnclosingClass(node, ctx.request.sourceCode, insideMixin, table);
    if (accessingClass.empty())
    {
        return;
    }

    const MemberAccess member =
        FindMember(accessingClass, std::string(idText), table, ctx.request.RequiresAccessorKeyword());
    if (!member.decided)
    {
        return;
    }

    const bool declaredPrivate = member.access == AccessModifier::Private;
    if (declaredPrivate && !ctx.request.TreatsPrivateAsProtected())
    {
        if (!IsSameType(accessingClass, member.declaringClass))
        {
            const TSPoint start = ts_node_start_point(node);
            const TSPoint end = ts_node_end_point(node);
            ctx.EmitAtRange({start.row, start.column, end.row, end.column}, "as-err-private-member-access", idText,
                            member.declaringClass);
        }
    }
}

void CheckIdentifierNode(TSNode node, const AccessCheckRequest& request, DiagnosticContext& ctx)
{
    TSNode parent = parser::GetParent(node);
    if (ShouldSkipIdentifierNode(node, parent))
    {
        return;
    }

    const std::string_view idText = ExtractCleanIdentifier(node, request.sourceCode);
    if (idText == "this" || idText == "super")
    {
        const TSPoint start = ts_node_start_point(node);
        CheckClosureDisallowedAccess(node, FindInnermostScope(request.scopeRoot, start.row, start.column), idText, ctx);
        return;
    }
    if (idText.empty() || IsKeyword(idText) || idText == "value")
    {
        return;
    }

    const TSPoint start = ts_node_start_point(node);
    const Scope* scope = FindInnermostScope(request.scopeRoot, start.row, start.column);
    if (CheckClosureDisallowedAccess(node, scope, idText, ctx))
    {
        return;
    }

    CheckImplicitMemberAccess(node, scope, idText, ctx);
}

void CheckMemberAccessIndexed(const AccessCheckRequest& request, DiagnosticContext& ctx)
{
    if (ctx.request.stopFlag && ctx.request.stopFlag->stop_requested())
    {
        return;
    }

    for (TSNode thisNode : request.nodeIndex->Nodes(parser::nodes::ThisExpression))
    {
        if (ctx.request.stopFlag && ctx.request.stopFlag->stop_requested())
        {
            return;
        }
        const TSPoint start = ts_node_start_point(thisNode);
        CheckClosureDisallowedAccess(thisNode, FindInnermostScope(request.scopeRoot, start.row, start.column), "this",
                                     ctx);
    }

    const auto& gs = parser::GetGrammarSymbols();
    const std::array<TSSymbol, 3> kSymbols = {gs.symMemberExpression, gs.symScopedIdentifier, gs.symIdentifier};
    request.nodeIndex->ForEachNodeOrdered(
        std::span<const TSSymbol>(kSymbols.data(), kSymbols.size()),
        [&](TSNode node)
        {
            if (ctx.request.stopFlag && ctx.request.stopFlag->stop_requested())
            {
                return;
            }
            if (ts_node_symbol(node) == gs.symMemberExpression)
            {
                const TSPoint start = ts_node_start_point(node);
                CheckMemberExpression(node, request, FindInnermostScope(request.scopeRoot, start.row, start.column),
                                      ctx);
            }
            else
            {
                CheckIdentifierNode(node, request, ctx);
            }
        });
}
} // namespace

void CheckMemberAccess(const AccessCheckRequest& request, DiagnosticContext& ctx)
{
    if (ts_node_is_null(request.root) || request.sourceCode.empty())
    {
        return;
    }

    if (utils::IsPredefinedFile(ctx.request.fileUri, ctx.request.predefinedFileExtension))
    {
        return;
    }

    if (request.nodeIndex)
    {
        CheckMemberAccessIndexed(request, ctx);
        return;
    }

    NodeIndex localIndex(request.root);
    AccessCheckRequest indexedRequest = request;
    indexedRequest.nodeIndex = &localIndex;
    CheckMemberAccessIndexed(indexedRequest, ctx);
}
} // namespace angel_lsp::analysis
