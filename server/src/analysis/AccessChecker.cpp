#include "analysis/AccessChecker.h"
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
        constexpr uint32_t k_objectFieldLength = 6; ///< "object"
        constexpr uint32_t k_memberFieldLength = 6; ///< "member"

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

        /**
         * @brief True when every type in a chain resolves to a declaration this analyzer can read.
         *
         * The rule's whole claim is "no declaration permits this access", so it may only speak once
         * every declaration is on the table. One base that resolves to nothing is an
         * engine-registered type as far as this pass knows, and those carry members - and access
         * levels - written down nowhere in the workspace.
         */
        bool HierarchyIsFullyVisible(const std::string &typeName, const SymbolTable &table)
        {
            for (const auto &ancestor : GetInheritedTypeHierarchy(typeName, table))
            {
                const auto symbols = table.FindSymbolsPtr(ancestor);
                if (!symbols)
                {
                    return false;
                }
                for (const auto &sym : *symbols)
                {
                    if (sym.type != SymbolType::Class)
                    {
                        continue;
                    }
                    for (const auto &base : sym.GetClass().bases)
                    {
                        if (!table.FindSymbolsPtr(CleanBaseType(base)))
                        {
                            return false;
                        }
                    }
                }
            }
            return true;
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
            bool decided = false;   ///< False means stay silent, for any of the reasons below.
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
        MemberAccess FindMember(const std::string &typeName,
                                const std::string &memberName,
                                const SymbolTable &table)
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
                }

                for (const auto &sym : candidates)
                {
                    AccessModifier access = AccessModifier::Public;
                    if (!TryReadAccess(sym, access))
                    {
                        continue;
                    }

                    if (access == AccessModifier::Public)
                    {
                        // Reachable, so there is nothing to report whatever else shares the name.
                        return MemberAccess{};
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
            const MemberAccess member = FindMember(objectType, memberName, table);
            if (!member.decided)
            {
                return;
            }

            bool insideMixin = false;
            const std::string accessingClass = EnclosingClass(node, request.sourceCode, insideMixin, table);

            // A mixin's methods are compiled into each class that includes it, not into the mixin,
            // so what its body may reach is decided somewhere this node is not. Left alone.
            if (insideMixin)
            {
                return;
            }

            const bool isPrivate = member.access == AccessModifier::Private;
            if (isPrivate)
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
                            isPrivate ? "as-err-private-member-access" : "as-err-protected-member-access",
                            memberName, member.declaringClass);
        }

        /** @brief Locates the innermost scope containing a point, for local-variable resolution. */
        const Scope *FindInnermostScope(const Scope *root, uint32_t line, uint32_t character)
        {
            if (!root)
            {
                return nullptr;
            }

            const auto contains = [line, character](const Scope &scope)
            {
                if (line < scope.startLine || line > scope.endLine)
                {
                    return false;
                }
                if (line == scope.startLine && character < scope.startCharacter)
                {
                    return false;
                }
                if (line == scope.endLine && character > scope.endCharacter)
                {
                    return false;
                }
                return true;
            };

            if (!contains(*root))
            {
                return nullptr;
            }

            const Scope *current = root;
            for (bool descended = true; descended;)
            {
                descended = false;
                for (const auto &child : current->children)
                {
                    if (child && contains(*child))
                    {
                        current = child.get();
                        descended = true;
                        break;
                    }
                }
            }
            return current;
        }

        void VisitNode(TSNode node, const AccessCheckRequest &request, DiagnosticContext &ctx)
        {
            if (std::string_view(ts_node_type(node)) == "member_expression")
            {
                const TSPoint start = ts_node_start_point(node);
                CheckMemberExpression(node, request,
                                      FindInnermostScope(request.scopeRoot, start.row, start.column), ctx);
            }

            const uint32_t childCount = ts_node_named_child_count(node);
            for (uint32_t i = 0; i < childCount; ++i)
            {
                VisitNode(ts_node_named_child(node, i), request, ctx);
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
