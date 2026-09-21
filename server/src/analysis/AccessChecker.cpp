#include "analysis/AccessChecker.h"
#include "analysis/ASTUtils.h"
#include "analysis/NodeIndex.h"
#include "analysis/ScopeTree.h"
#include "analysis/SemanticHelpers.h"
#include "utils/Utils.h"

#include "parser/GrammarNames.h"
#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{
namespace
{
/**
 * @brief Node text as an owning string.
 *
 * Kept per translation unit rather than shared with ASTUtils::NodeText, which returns a
 * string_view. The two are not interchangeable: callers here store the result, concatenate
 * it, and use it after the node has gone out of scope, so handing them a view would trade a
 * duplicated three-line function for a lifetime question at several dozen call sites.
 * Deduplicating it was attempted and reverted for exactly that reason.
 */
std::string NodeText(TSNode node, std::string_view sourceCode)
{
    if (ts_node_is_null(node))
    {
        return "";
    }

    const uint32_t start = ts_node_start_byte(node);
    const uint32_t end = ts_node_end_byte(node);
    if (start >= end || end > sourceCode.size())
    {
        return "";
    }
    return std::string(sourceCode.substr(start, end - start));
}

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
    const std::string& typeName;
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
    auto candidatesPtr = ctx.table.FindSymbolsPtr(owner + "::" + memberName);
    if (candidatesPtr && !candidatesPtr->empty())
    {
        candidates.insert(candidates.end(), candidatesPtr->begin(), candidatesPtr->end());
        return candidates;
    }

    for (const auto& accessor : {owner + "::get_" + memberName, owner + "::set_" + memberName})
    {
        auto accSyms = ctx.table.FindSymbolsPtr(accessor);
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

    const MemberLookupContext ctx{typeName, table, accessorKeywordRequired};
    for (const auto& owner : GetInheritedTypeHierarchy(typeName, table))
    {
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
    for (const auto& container : GetEnclosingContainers(node, sourceCode))
    {
        if (container.kind != ContainerKind::Class)
        {
            continue;
        }

        const auto symbols = table.FindSymbolsPtr(container.name);
        if (symbols)
        {
            insideMixin = std::any_of(symbols->begin(), symbols->end(), [](const Symbol& sym)
                                      { return sym.type == SymbolType::Class && sym.GetClass().modifiers.isMixin; });
        }
        return container.name;
    }
    return "";
}

/**
 * @brief Resolves the owning type of a member access object expression.
 *
 * @param[in] objectNode AST node of the object expression.
 * @param[in] scope Current lexical scope.
 * @param[in] request Access check request.
 * @param[in] ctx Diagnostic context.
 * @return Normalized owner type name.
 */
std::string ResolveObjectOwnerType(TSNode objectNode, const Scope* scope, const AccessCheckRequest& request,
                                   const DiagnosticContext& ctx)
{
    const std::string_view arrayContainer =
        ctx.request.GetArrayTypeName().empty() ? std::string_view("array") : ctx.request.GetArrayTypeName();
    return MemberOwnerType(
        ResolveExpressionType(objectNode, scope, ctx.request.symbolTable, request.sourceCode, ctx.request.fileUri),
        arrayContainer);
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
        ctx.EmitAtRange(hintStart.row, hintStart.column, hintEnd.row, hintEnd.column, "as-hint-accessor-disabled",
                        objectType, NodeText(memberNode, ctx.request.sourceCode), DiagnosticSeverity::Hint);
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

void CheckMemberExpression(TSNode node, const AccessCheckRequest& request, const Scope* scope, DiagnosticContext& ctx)
{
    TSNode objectNode = parser::GetChildByField(node, parser::fields::Object);
    TSNode memberNode = parser::GetChildByField(node, parser::fields::Member);
    if (ts_node_is_null(objectNode) || ts_node_is_null(memberNode))
    {
        return;
    }

    const SymbolTable& table = ctx.request.symbolTable;
    const std::string objectType = ResolveObjectOwnerType(objectNode, scope, request, ctx);
    if (objectType.empty() || !HierarchyIsFullyVisible(objectType, table))
    {
        return;
    }

    const std::string memberName = NodeText(memberNode, request.sourceCode);
    const MemberAccess member = FindMember(objectType, memberName, table, ctx.request.RequiresAccessorKeyword());
    if (!member.found)
    {
        const TSPoint start = ts_node_start_point(memberNode);
        const TSPoint end = ts_node_end_point(memberNode);
        ctx.EmitAtRange(start.row, start.column, end.row, end.column, "as-err-member-not-found", objectType,
                        memberName);
        return;
    }

    MaybeEmitAccessorHint(memberNode, member, objectType, ctx);

    if (!member.decided)
    {
        return;
    }

    bool insideMixin = false;
    const std::string accessingClass = EnclosingClass(node, request.sourceCode, insideMixin, table);

    if (IsMemberAccessAllowed(member, accessingClass, objectType, ctx))
    {
        return;
    }

    const TSPoint start = ts_node_start_point(memberNode);
    const TSPoint end = ts_node_end_point(memberNode);
    const bool declaredPrivate = member.access == AccessModifier::Private;
    ctx.EmitAtRange(start.row, start.column, end.row, end.column,
                    declaredPrivate ? "as-err-private-member-access" : "as-err-protected-member-access", memberName,
                    member.declaringClass);
}

/**
 * @brief Checks if an AST node type corresponds to a declaration name node.
 *
 * @param[in] type AST node type name.
 * @return True if node type defines a declared symbol name.
 */
bool IsDeclarationNodeType(std::string_view type)
{
    static constexpr std::string_view kDeclTypes[] = {
        "variable_declarator",   "parameter",           "func_declaration",   "class_declaration",
        "interface_declaration", "enum_declaration",    "enum_member",        "virtual_property",
        "typedef_declaration",   "funcdef_declaration", "import_declaration", "mixin_declaration"};
    return std::find(std::begin(kDeclTypes), std::end(kDeclTypes), type) != std::end(kDeclTypes);
}

/**
 * @brief Checks if an AST node type is a type reference or comment node.
 *
 * @param[in] type AST node type name.
 * @return True if node type is a type reference or comment.
 */
bool IsTypeRefOrComment(std::string_view type)
{
    static constexpr std::string_view kTypeRefTypes[] = {"datatype", "primitive_type", "base_class_list", "comment"};
    return std::find(std::begin(kTypeRefTypes), std::end(kTypeRefTypes), type) != std::end(kTypeRefTypes);
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

    const std::string_view parentType = ts_node_type(parent);
    if (parentType == "member_expression")
    {
        TSNode memberField = parser::GetChildByField(parent, parser::fields::Member);
        return ts_node_eq(node, memberField);
    }

    if (IsDeclarationNodeType(parentType))
    {
        TSNode nameField = parser::GetChildByField(parent, parser::fields::Name);
        return ts_node_eq(node, nameField);
    }

    if (IsTypeRefOrComment(parentType))
    {
        return true;
    }

    return std::string_view(ts_node_type(node)) == "identifier" && parentType == "scoped_identifier";
}

/**
 * @brief Extracts trimmed text from an identifier AST node.
 *
 * @param[in] node AST node of the identifier.
 * @param[in] sourceCode Document source text.
 * @return Clean trimmed identifier string.
 */
std::string ExtractCleanIdentifier(TSNode node, std::string_view sourceCode)
{
    std::string idText = NodeText(node, sourceCode);
    while (!idText.empty() && isspace(static_cast<unsigned char>(idText.front())))
    {
        idText.erase(idText.begin());
    }
    while (!idText.empty() && isspace(static_cast<unsigned char>(idText.back())))
    {
        idText.pop_back();
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

    if (IsDeclaredInClosure(scope, enclosingClosure, idText))
    {
        return false;
    }

    if (IsOuterLocalOrParameter(enclosingClosure, idText))
    {
        const TSPoint start = ts_node_start_point(node);
        const TSPoint end = ts_node_end_point(node);
        ctx.EmitAtRange(start.row, start.column, end.row, end.column, "as-err-lambda-closure-disallowed");
        return true;
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
    const SymbolTable& table = ctx.request.symbolTable;

    bool insideMixin = false;
    const std::string accessingClass = EnclosingClass(node, ctx.request.sourceCode, insideMixin, table);
    if (accessingClass.empty())
    {
        return;
    }

    const LocalDefinition* localDef = ResolveInScope(scope, idText);
    if (localDef &&
        (localDef->kind == LocalDefinitionKind::Variable || localDef->kind == LocalDefinitionKind::Parameter))
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
            ctx.EmitAtRange(start.row, start.column, end.row, end.column, "as-err-private-member-access", idText,
                            member.declaringClass);
        }
    }
}

void CheckIdentifierNode(TSNode node, const AccessCheckRequest& request, const Scope* scope, DiagnosticContext& ctx)
{
    TSNode parent = ts_node_parent(node);
    if (ShouldSkipIdentifierNode(node, parent))
    {
        return;
    }

    const std::string idText = ExtractCleanIdentifier(node, request.sourceCode);
    if (idText.empty() || IsKeyword(idText) || idText == "value")
    {
        return;
    }

    if (CheckClosureDisallowedAccess(node, scope, idText, ctx))
    {
        return;
    }

    CheckImplicitMemberAccess(node, scope, idText, ctx);
}

void VisitNode(TSNode node, const AccessCheckRequest& request, DiagnosticContext& ctx, int depth = 0)
{
    // Pathologically nested source would otherwise recurse until the stack gives out; see
    // k_maxAstDepth in ASTUtils.h.
    if (depth > k_maxAstDepth)
        return;

    const std::string_view nodeType = ts_node_type(node);
    if (nodeType == "member_expression")
    {
        const TSPoint start = ts_node_start_point(node);
        CheckMemberExpression(node, request, FindInnermostScope(request.scopeRoot, start.row, start.column), ctx);
    }
    else if (nodeType == "scoped_identifier" || nodeType == "identifier")
    {
        const TSPoint start = ts_node_start_point(node);
        CheckIdentifierNode(node, request, FindInnermostScope(request.scopeRoot, start.row, start.column), ctx);
    }

    const uint32_t childCount = ts_node_named_child_count(node);
    for (uint32_t i = 0; i < childCount; ++i)
    {
        VisitNode(ts_node_named_child(node, i), request, ctx, depth + 1);
    }
}
} // namespace

void CheckMemberAccess(const AccessCheckRequest& request, DiagnosticContext& ctx)
{
    if (ts_node_is_null(request.root) || request.sourceCode.empty())
    {
        return;
    }

    // A stub describes an API rather than using one, so it has no expressions worth judging -
    // and reading its declarations as accesses would be the same category error the declaration
    // rules already exempt it from.
    if (utils::IsPredefinedFile(ctx.request.fileUri, ctx.request.predefinedFileExtension))
    {
        return;
    }

    if (request.nodeIndex)
    {
        auto members = request.nodeIndex->Nodes(parser::nodes::MemberExpression);
        auto scopeds = request.nodeIndex->Nodes(parser::nodes::ScopedIdentifier);
        auto idents = request.nodeIndex->Nodes(parser::nodes::Identifier);

        size_t i = 0;
        size_t j = 0;
        size_t k = 0;
        while (i < members.size() || j < scopeds.size() || k < idents.size())
        {
            uint32_t bMembers = (i < members.size()) ? ts_node_start_byte(members[i]) : UINT32_MAX;
            uint32_t bScopeds = (j < scopeds.size()) ? ts_node_start_byte(scopeds[j]) : UINT32_MAX;
            uint32_t bIdents = (k < idents.size()) ? ts_node_start_byte(idents[k]) : UINT32_MAX;

            if (bMembers <= bScopeds && bMembers <= bIdents)
            {
                TSNode node = members[i++];
                const TSPoint start = ts_node_start_point(node);
                CheckMemberExpression(node, request, FindInnermostScope(request.scopeRoot, start.row, start.column),
                                      ctx);
            }
            else if (bScopeds <= bIdents)
            {
                TSNode node = scopeds[j++];
                const TSPoint start = ts_node_start_point(node);
                CheckIdentifierNode(node, request, FindInnermostScope(request.scopeRoot, start.row, start.column), ctx);
            }
            else
            {
                TSNode node = idents[k++];
                const TSPoint start = ts_node_start_point(node);
                CheckIdentifierNode(node, request, FindInnermostScope(request.scopeRoot, start.row, start.column), ctx);
            }
        }
        return;
    }

    VisitNode(request.root, request, ctx);
}
} // namespace angel_lsp::analysis
