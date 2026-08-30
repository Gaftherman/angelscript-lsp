#include "analysis/AccessChecker.h"
#include "analysis/ASTUtils.h"
#include "analysis/SemanticHelpers.h"
#include "utils/Utils.h"

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

        constexpr uint32_t k_objectFieldLength = 6; ///< "object"
        constexpr uint32_t k_memberFieldLength = 6; ///< "member"

        /** @brief Strips a namespace qualification, leaving the last segment ("G::A" -> "A"). */
        std::string LastScopeSegment(const std::string &name)
        {
            const size_t pos = name.rfind("::");
            return pos == std::string::npos ? name : name.substr(pos + 2);
        }

        /** @brief Compares two type names by their last segment, so a qualification cannot hide a match. */
        bool IsSameType(const std::string &a, const std::string &b)
        {
            return !a.empty() && (a == b || LastScopeSegment(a) == LastScopeSegment(b));
        }

        /** @brief True when derived is base, or reaches it through its declared base chain. */
        bool DerivesFrom(const std::string &derived, const std::string &base, const SymbolTable &table)
        {
            if (IsSameType(derived, base))
            {
                return true;
            }
            for (const auto &ancestor : GetInheritedTypeHierarchy(derived, table))
            {
                if (IsSameType(ancestor, base))
                {
                    return true;
                }
            }
            return false;
        }

        /** @brief The access modifier a declaration carries, whatever kind of declaration it is. */
        bool TryReadAccess(const Symbol &sym, AccessModifier &access)
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
            bool found = false;     ///< True if any member of this name exists in the hierarchy.
            bool decided = false;   ///< True if an access restriction (private/protected) was found.
            AccessModifier access = AccessModifier::Public;
            std::string declaringClass;
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
         * `c.V` is answered with "'V' is not a member of 'C'". Both halves measured directly:
         *
         *     angelscript_oracle doc_r07_accessor_without_kw.as --property-accessor-mode=3   rejects
         *     angelscript_oracle doc_r07_accessor_without_kw.as --property-accessor-mode=2   accepts
         *
         * The `--property-accessor-mode` flag was added to the oracle for this, because a setting
         * whose second half cannot be asked about is a setting recorded on faith.
         */
        bool AccessorStandsForProperty(const Symbol &sym, bool keywordRequired)
        {
            if (!keywordRequired)
            {
                return true;
            }
            return std::holds_alternative<FunctionSignature>(sym.signature) &&
                   sym.GetFunction().modifiers.isProperty;
        }

        MemberAccess FindMember(const std::string &typeName,
                                const std::string &memberName,
                                const SymbolTable &table,
                                bool accessorKeywordRequired)
        {
            MemberAccess result;
            if (typeName.empty() || memberName.empty())
            {
                return result;
            }

            for (const auto &owner : GetInheritedTypeHierarchy(typeName, table))
            {
                auto candidates = table.FindSymbols(owner + "::" + memberName);
                if (candidates.empty())
                {
                    for (const auto &accessor : { owner + "::get_" + memberName,
                                                  owner + "::set_" + memberName })
                    {
                        for (const auto &sym : table.FindSymbols(accessor))
                        {
                            if (AccessorStandsForProperty(sym, accessorKeywordRequired))
                            {
                                candidates.push_back(sym);
                            }
                        }
                    }
                }

                if (candidates.empty())
                {
                    // A member declared inside a namespaced class is registered under a qualified
                    // name the concatenation above does not reproduce, so fall back to matching on
                    // the container the collector recorded.
                    for (const auto &sym : table.FindSymbols(memberName))
                    {
                        if (IsSameType(sym.containerName, owner))
                        {
                            candidates.push_back(sym);
                        }
                    }
                    if (candidates.empty())
                    {
                        for (const auto &sym : table.FindSymbols("get_" + memberName))
                        {
                            if (IsSameType(sym.containerName, owner) &&
                                AccessorStandsForProperty(sym, accessorKeywordRequired))
                            {
                                candidates.push_back(sym);
                            }
                        }
                        for (const auto &sym : table.FindSymbols("set_" + memberName))
                        {
                            if (IsSameType(sym.containerName, owner) &&
                                AccessorStandsForProperty(sym, accessorKeywordRequired))
                            {
                                candidates.push_back(sym);
                            }
                        }
                    }
                }

                for (const auto &sym : candidates)
                {
                    AccessModifier access = AccessModifier::Public;
                    if (!TryReadAccess(sym, access))
                    {
                        continue;
                    }

                    result.found = true;

                    if (access == AccessModifier::Public)
                    {
                        // Reachable, so there is nothing to report whatever else shares the name.
                        result.decided = false;
                        result.access = AccessModifier::Public;
                        result.declaringClass = IsMixinClass(owner, table) ? typeName : owner;
                        return result;
                    }

                    // Protected outranks private: the more permissive of two same-named
                    // declarations is the one an access has to fail against to be an error.
                    if (!result.decided || (result.access == AccessModifier::Private &&
                                            access == AccessModifier::Protected))
                    {
                        result.decided = true;
                        result.access = access;
                        result.declaringClass = IsMixinClass(owner, table) ? typeName : owner;
                    }
                }
            }

            return result;
        }

        /** @brief Innermost class body enclosing a node, or empty when the node sits outside one. */
        std::string EnclosingClass(TSNode node, std::string_view sourceCode, bool &insideMixin,
                                   const SymbolTable &table)
        {
            insideMixin = false;
            for (const auto &container : GetEnclosingContainers(node, sourceCode))
            {
                if (container.kind != ContainerKind::Class)
                {
                    continue;
                }

                const auto symbols = table.FindSymbolsPtr(container.name);
                if (symbols)
                {
                    insideMixin = std::any_of(symbols->begin(), symbols->end(),
                                              [](const Symbol &sym)
                                              {
                                                  return sym.type == SymbolType::Class &&
                                                         sym.GetClass().modifiers.isMixin;
                                              });
                }
                return container.name;
            }
            return "";
        }

        void CheckMemberExpression(TSNode node, const AccessCheckRequest &request,
                                   const Scope *scope, DiagnosticContext &ctx)
        {
            TSNode objectNode = ts_node_child_by_field_name(node, "object", k_objectFieldLength);
            TSNode memberNode = ts_node_child_by_field_name(node, "member", k_memberFieldLength);
            if (ts_node_is_null(objectNode) || ts_node_is_null(memberNode))
            {
                return;
            }

            const SymbolTable &table = ctx.request.symbolTable;

            const std::string objectType = CleanBaseType(ResolveExpressionType(
                objectNode, scope, table, request.sourceCode, ctx.request.fileUri));
            if (objectType.empty() || !HierarchyIsFullyVisible(objectType, table))
            {
                return;
            }

            const std::string memberName = NodeText(memberNode, request.sourceCode);
            const MemberAccess member = FindMember(objectType, memberName, table,
                                                   ctx.request.RequiresAccessorKeyword());
            if (!member.found)
            {
                const TSPoint start = ts_node_start_point(memberNode);
                const TSPoint end = ts_node_end_point(memberNode);
                ctx.EmitAtRange(start.row, start.column, end.row, end.column,
                                "as-err-member-not-found", objectType, memberName);
                return;
            }

            if (!member.decided)
            {
                return;
            }

            bool insideMixin = false;
            const std::string accessingClass = EnclosingClass(node, request.sourceCode, insideMixin, table);
            (void)insideMixin;

            // A mixin body is judged like any other. It used to be skipped wholesale, on the
            // grounds that its methods are compiled into each including class - but that only
            // clouds an access to the mixin's *own* members, which is handled where a member is
            // attributed to the including class rather than to the mixin. An access to some other
            // class's private member is an error from every includer, and a real engine says so:
            // "Illegal access to private property", once per instantiation.
            //
            // The guard was also dead until now: GetEnclosingContainers did not recognise a mixin
            // body as a class at all, so insideMixin was never true.

            // asEP_PRIVATE_PROP_AS_PROTECTED makes a private member follow the protected rule
            // instead, so a derived class may reach it. The declaration still says `private` and
            // so does the message when the access is wrong anyway - what the option changes is
            // which rule decides that, not what the member was written as.
            const bool declaredPrivate = member.access == AccessModifier::Private;
            if (declaredPrivate && !ctx.request.TreatsPrivateAsProtected())
            {
                // Per class, not per instance: inside the declaring class every object of that
                // class is open, and outside it none is.
                if (IsSameType(accessingClass, member.declaringClass))
                {
                    return;
                }
            }
            else
            {
                // Reachable from a derived class, and only through an object of that class's own
                // type - reaching a base-typed object's protected member is an error even from a
                // class that inherits it.
                if (!accessingClass.empty() &&
                    DerivesFrom(accessingClass, member.declaringClass, table) &&
                    DerivesFrom(objectType, accessingClass, table))
                {
                    return;
                }
            }

            const TSPoint start = ts_node_start_point(memberNode);
            const TSPoint end = ts_node_end_point(memberNode);
            ctx.EmitAtRange(start.row, start.column, end.row, end.column,
                            declaredPrivate ? "as-err-private-member-access" : "as-err-protected-member-access",
                            memberName, member.declaringClass);
        }

        void CheckIdentifierNode(TSNode node, const AccessCheckRequest &request,
                                 const Scope *scope, DiagnosticContext &ctx)
        {
            TSNode parent = ts_node_parent(node);
            if (ts_node_is_null(parent))
            {
                return;
            }

            const std::string_view parentType = ts_node_type(parent);

            // Skip if this is the member field of a member_expression (e.g. the 'b' in 'a.b')
            if (parentType == "member_expression")
            {
                TSNode memberField = ts_node_child_by_field_name(parent, "member", k_memberFieldLength);
                if (ts_node_eq(node, memberField))
                {
                    return;
                }
            }

            // Skip declarations (where the identifier defines a name)
            if (parentType == "variable_declarator" || parentType == "parameter" ||
                parentType == "func_declaration" || parentType == "class_declaration" ||
                parentType == "interface_declaration" || parentType == "enum_declaration" ||
                parentType == "enum_member" || parentType == "virtual_property" ||
                parentType == "typedef_declaration" || parentType == "funcdef_declaration" ||
                parentType == "import_declaration" || parentType == "mixin_declaration")
            {
                TSNode nameField = ts_node_child_by_field_name(parent, "name", 4);
                if (ts_node_eq(node, nameField))
                {
                    return;
                }
            }

            // Skip type references, base class list, comments, etc.
            if (parentType == "datatype" || parentType == "primitive_type" ||
                parentType == "base_class_list" || parentType == "comment")
            {
                return;
            }

            // If node is an identifier inside a scoped_identifier, let the scoped_identifier be checked instead
            if (std::string_view(ts_node_type(node)) == "identifier" && parentType == "scoped_identifier")
            {
                return;
            }

            std::string idText = NodeText(node, request.sourceCode);
            while (!idText.empty() && isspace(static_cast<unsigned char>(idText.front()))) idText.erase(idText.begin());
            while (!idText.empty() && isspace(static_cast<unsigned char>(idText.back()))) idText.pop_back();

            if (idText.empty() || idText == "this" || idText == "super" || idText == "value" ||
                idText == "true" || idText == "false" || idText == "null")
            {
                return;
            }

            // Check if identifier is inside a lambda_expression and accessing outer local variables/parameters (no closures)
            TSNode p = parent;
            TSNode lambdaNode{};
            bool hasLambda = false;
            while (!ts_node_is_null(p))
            {
                std::string_view pt = ts_node_type(p);
                if (pt == "lambda_expression" || pt == "anonymous_function")
                {
                    lambdaNode = p;
                    hasLambda = true;
                    break;
                }
                if (pt == "func_declaration" || pt == "function_definition")
                {
                    break;
                }
                p = ts_node_parent(p);
            }

            if (hasLambda)
            {
                const Scope *owner = nullptr;
                const LocalDefinition *localDef = ResolveInScope(scope, idText, &owner);

                // A lambda captures nothing, but a *global* is not a capture - it is still in
                // scope, and the compiler accepts it:
                //
                //     int g = 5;
                //     void Init() { CB@ cb = function() { g = 100; }; }   // compiles
                //     void Init() { int l = 1; CB@ cb = function() { l = 2; }; }
                //                                                    ^ No matching symbol 'l'
                //
                // LOCALS_QUERY records a module-level declaration under the same
                // LocalDefinitionKind::Variable as a function-body local (its own comment says
                // "Variables (locals and globals)"), so the definition alone cannot tell the two
                // apart. Only the scope that holds it can. Without this test every legal global
                // read inside a lambda was reported.
                bool ownerIsFunctionNested = false;
                for (const Scope *ancestor = owner; ancestor != nullptr; ancestor = ancestor->parent)
                {
                    if (ancestor->isFunctionScope)
                    {
                        ownerIsFunctionNested = true;
                        break;
                    }
                }

                if (localDef && ownerIsFunctionNested &&
                    (localDef->kind == LocalDefinitionKind::Variable || localDef->kind == LocalDefinitionKind::Parameter))
                {
                    TSPoint lStart = ts_node_start_point(lambdaNode);
                    TSPoint lEnd = ts_node_end_point(lambdaNode);
                    bool isInsideLambda = false;
                    if (localDef->startLine > lStart.row && localDef->startLine < lEnd.row)
                    {
                        isInsideLambda = true;
                    }
                    else if (localDef->startLine == lStart.row && localDef->startCharacter >= lStart.column)
                    {
                        isInsideLambda = true;
                    }

                    if (!isInsideLambda)
                    {
                        const TSPoint start = ts_node_start_point(node);
                        const TSPoint end = ts_node_end_point(node);
                        ctx.EmitAtRange(start.row, start.column, end.row, end.column,
                                        "as-err-lambda-closure-disallowed");
                        return;
                    }
                }
            }

            const SymbolTable &table = ctx.request.symbolTable;

            bool insideMixin = false;
            const std::string accessingClass = EnclosingClass(node, request.sourceCode, insideMixin, table);
            if (accessingClass.empty())
            {
                return;
            }

            // If it resolves to a local variable or parameter in the current function scope, it's not an implicit member access
            const LocalDefinition *localDef = ResolveInScope(scope, idText);
            if (localDef && (localDef->kind == LocalDefinitionKind::Variable || localDef->kind == LocalDefinitionKind::Parameter))
            {
                return;
            }

            const MemberAccess member = FindMember(accessingClass, idText, table,
                                                   ctx.request.RequiresAccessorKeyword());
            if (!member.decided)
            {
                return;
            }

            const bool declaredPrivate = member.access == AccessModifier::Private;
            if (declaredPrivate && !ctx.request.TreatsPrivateAsProtected())
            {
                // In derived class, accessing base class's private member implicitly is an error
                if (!IsSameType(accessingClass, member.declaringClass))
                {
                    const TSPoint start = ts_node_start_point(node);
                    const TSPoint end = ts_node_end_point(node);
                    ctx.EmitAtRange(start.row, start.column, end.row, end.column,
                                    "as-err-private-member-access", idText, member.declaringClass);
                }
            }
        }

        void VisitNode(TSNode node, const AccessCheckRequest &request, DiagnosticContext &ctx, int depth = 0)
                {
            // Pathologically nested source would otherwise recurse until the stack gives out; see
            // k_maxAstDepth in ASTUtils.h.
            if (depth > k_maxAstDepth)
                return;

            const std::string_view nodeType = ts_node_type(node);
            if (nodeType == "member_expression")
            {
                const TSPoint start = ts_node_start_point(node);
                CheckMemberExpression(node, request,
                                      FindInnermostScope(request.scopeRoot, start.row, start.column), ctx);
            }
            else if (nodeType == "scoped_identifier" || nodeType == "identifier")
            {
                const TSPoint start = ts_node_start_point(node);
                CheckIdentifierNode(node, request,
                                    FindInnermostScope(request.scopeRoot, start.row, start.column), ctx);
            }

            const uint32_t childCount = ts_node_named_child_count(node);
            for (uint32_t i = 0; i < childCount; ++i)
            {
                VisitNode(ts_node_named_child(node, i), request, ctx, depth + 1);
            }
        }
    }

    void CheckMemberAccess(const AccessCheckRequest &request, DiagnosticContext &ctx)
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

        VisitNode(request.root, request, ctx);
    }
}
