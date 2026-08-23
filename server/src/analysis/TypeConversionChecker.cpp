#include "analysis/TypeConversionChecker.h"
#include "analysis/SemanticHelpers.h"

#include <algorithm>
#include <functional>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{
    namespace
    {
        constexpr uint32_t k_typeFieldLength = 4;      ///< "type"
        constexpr uint32_t k_valueFieldLength = 5;     ///< "value"
        constexpr uint32_t k_argsFieldLength = 9;      ///< "arguments"
        constexpr uint32_t k_functionFieldLength = 8;  ///< "function"
        constexpr uint32_t k_operandFieldLength = 7;   ///< "operand"
        constexpr uint32_t k_operatorFieldLength = 8;  ///< "operator"
        constexpr uint32_t k_varTypeFieldLength = 8;   ///< "var_type"

        /** @brief What a resolved expression is worth to this pass. */
        struct ExpressionType
        {
            std::string baseName;    ///< Cleaned base type name, e.g. "Money" or "int".
            bool known = false;      ///< False means "give up" - never a reason to diagnose.
            bool isLiteral = false;  ///< A literal's type is exact, so it can be judged strictly.
        };

        /** @brief The little a rule needs to know about a type declaration.
         *  @note Deliberately not the Symbol itself: this is looked up for practically every call
         *        expression in a document, and copying a Symbol (strings, parameter vectors, a
         *        variant) that many times dominated the pass. */
        struct TypeDeclarationInfo
        {
            bool found = false;
            bool isClass = false;
            bool isTemplate = false;
        };

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

        std::string_view NodeType(TSNode node)
        {
            return ts_node_is_null(node) ? std::string_view{} : std::string_view(ts_node_type(node));
        }

        /** @brief Strips a namespace qualification, leaving the last segment ("NS::Foo" -> "Foo"). */
        std::string LastScopeSegment(const std::string &name)
        {
            const size_t pos = name.rfind("::");
            return pos == std::string::npos ? name : name.substr(pos + 2);
        }

        bool IsSameType(const std::string &a, const std::string &b)
        {
            return a == b || LastScopeSegment(a) == LastScopeSegment(b);
        }

        /** @brief Visits every symbol registered under a qualified name without copying the bucket.
         *  @param visitor Returns true to stop the walk. */
        void ForEachSymbolNamed(const std::string &qualifiedName,
                                const SymbolTable &table,
                                const std::function<bool(const Symbol &)> &visitor)
        {
            const auto bucket = table.FindSymbolsPtr(qualifiedName);
            if (!bucket)
            {
                return;
            }
            for (const auto &sym : *bucket)
            {
                if (visitor(sym))
                {
                    return;
                }
            }
        }

        /** @brief Looks up what a type name denotes.
         *  @return found == false when the name resolves to nothing this analyzer can see - which
         *          is the signal to stay silent about anything involving it. */
        TypeDeclarationInfo FindTypeDeclaration(const std::string &typeName, const SymbolTable &table)
        {
            TypeDeclarationInfo info;
            if (typeName.empty())
            {
                return info;
            }

            const std::string bare = LastScopeSegment(typeName);
            for (const auto &candidate : { std::cref(typeName), std::cref(bare) })
            {
                ForEachSymbolNamed(candidate.get(), table, [&info](const Symbol &sym)
                {
                    if (sym.type != SymbolType::Class && sym.type != SymbolType::Interface)
                    {
                        return false;
                    }
                    info.found = true;
                    info.isClass = sym.type == SymbolType::Class;
                    info.isTemplate = info.isClass && sym.GetClass().isTemplate;
                    return true;
                });

                if (info.found || bare == typeName)
                {
                    break;
                }
            }
            return info;
        }

        /** @brief True when a name resolves to an enum, typedef, funcdef or namespace.
         *  @note Keeps those out of the class-shaped rules below rather than letting them fall
         *        through to "not a class, therefore suspicious". */
        bool ResolvesToNonClassDeclaration(const std::string &typeName, const SymbolTable &table)
        {
            bool found = false;
            const std::string bare = LastScopeSegment(typeName);

            for (const auto &candidate : { std::cref(typeName), std::cref(bare) })
            {
                ForEachSymbolNamed(candidate.get(), table, [&found](const Symbol &sym)
                {
                    if (sym.type == SymbolType::Enum || sym.type == SymbolType::Typedef ||
                        sym.type == SymbolType::Funcdef || sym.type == SymbolType::Namespace)
                    {
                        found = true;
                        return true;
                    }
                    return false;
                });

                if (found || bare == typeName)
                {
                    break;
                }
            }
            return found;
        }

        /** @brief Visits every overload of a named method visible on a type, base types included.
         *  @param visitor Returns true to stop the walk. */
        void ForEachMethod(const std::string &typeName,
                           const std::string &memberName,
                           const SymbolTable &table,
                           const std::function<bool(const Symbol &)> &visitor)
        {
            bool stopped = false;
            for (const auto &cls : GetInheritedTypeHierarchy(typeName, table))
            {
                ForEachSymbolNamed(cls + "::" + memberName, table, [&](const Symbol &sym)
                {
                    if (sym.type != SymbolType::Function)
                    {
                        return false;
                    }
                    stopped = visitor(sym);
                    return stopped;
                });

                if (stopped)
                {
                    return;
                }
            }
        }

        /** @brief Visits the constructors declared directly on a type.
         *  @note Deliberately not hierarchy-wide: AngelScript does not inherit constructors. */
        void ForEachConstructor(const std::string &typeName,
                                const SymbolTable &table,
                                const std::function<bool(const Symbol &)> &visitor)
        {
            const std::string bare = LastScopeSegment(typeName);
            bool sawAny = false;
            bool stopped = false;

            const auto visit = [&](const Symbol &sym)
            {
                if (sym.type != SymbolType::Function)
                {
                    return false;
                }
                sawAny = true;
                stopped = visitor(sym);
                return stopped;
            };

            ForEachSymbolNamed(typeName + "::" + bare, table, visit);
            if (!stopped && !sawAny && bare != typeName)
            {
                ForEachSymbolNamed(bare + "::" + bare, table, visit);
            }
        }

        /**
         * @brief True when an overload can be called with exactly one argument.
         * @note Parameter count alone is not the test: `CLogger(const string &in name, bool
         *       isStatic = false)` is a one-argument converting constructor, and treating it as a
         *       two-argument one is what made the first corpus run flag a legitimate conversion.
         */
        bool AcceptsSingleArgument(const std::vector<ParameterInformation> &parameters)
        {
            if (parameters.empty())
            {
                return false;
            }
            return std::all_of(parameters.begin() + 1, parameters.end(),
                               [](const ParameterInformation &param) { return !param.defaultValue.empty(); });
        }

        /** @brief The declared type of the single argument such an overload converts from. */
        std::string SingleArgumentType(const std::vector<ParameterInformation> &parameters)
        {
            return parameters.empty() ? "" : CleanBaseType(parameters[0].typeName);
        }

        /** @brief True when a type declares any cast operator overload.
         *  @note Deliberately coarse. Matching a cast operator to its result type means resolving
         *        the engine's template-ish opCast, which no declaration in the source states
         *        precisely enough to bet a diagnostic on. */
        bool DeclaresAnyCastOperator(const std::string &typeName, const SymbolTable &table)
        {
            bool found = false;
            const auto mark = [&found](const Symbol &) { found = true; return true; };

            ForEachMethod(typeName, "opCast", table, mark);
            if (!found)
            {
                ForEachMethod(typeName, "opImplCast", table, mark);
            }
            return found;
        }

        /** @brief True when a type declares a conversion operator producing the target type. */
        bool DeclaresConversionTo(const std::string &fromType,
                                  const std::string &toType,
                                  const SymbolTable &table,
                                  bool implicitOnly)
        {
            bool found = false;
            const auto matches = [&](const Symbol &sym)
            {
                if (IsSameType(CleanBaseType(sym.GetFunction().returnType), toType))
                {
                    found = true;
                    return true;
                }
                return false;
            };

            static const char *k_implicit[] = { "opImplConv", "opImplCast" };
            static const char *k_explicit[] = { "opConv", "opCast" };

            for (const char *opName : k_implicit)
            {
                ForEachMethod(fromType, opName, table, matches);
                if (found)
                {
                    return true;
                }
            }
            if (implicitOnly)
            {
                return false;
            }
            for (const char *opName : k_explicit)
            {
                ForEachMethod(fromType, opName, table, matches);
                if (found)
                {
                    return true;
                }
            }
            return false;
        }

        /** @brief True when one type appears in the other's inheritance chain, either direction.
         *  @note Both directions count: an upcast is implicit, and a downcast is what cast<> is
         *        for, so neither is worth a diagnostic here. */
        bool AreHierarchyRelated(const std::string &a, const std::string &b, const SymbolTable &table)
        {
            for (const auto &base : GetInheritedTypeHierarchy(a, table))
            {
                if (IsSameType(base, b))
                {
                    return true;
                }
            }
            for (const auto &base : GetInheritedTypeHierarchy(b, table))
            {
                if (IsSameType(base, a))
                {
                    return true;
                }
            }
            return false;
        }

        /** @brief True for the built-in scalar types plus the configured string type.
         *  @note Conversions among these are the engine's business, not a declaration's, so the
         *        rules below let every primitive-to-primitive pair through. */
        bool IsBuiltInValueType(const std::string &typeName, const DiagnosticContext &ctx)
        {
            return IsPrimitiveTypeName(typeName) || typeName == ctx.request.GetStringTypeName();
        }

        /**
         * @brief Decides whether a value of type 'from' can reach type 'to'.
         * @param depth Recursion guard. At depth > 0 only the direct relations are consulted, so
         *              matching a constructor parameter cannot recurse into another constructor.
         * @return True whenever a route exists - and also whenever this analyzer cannot see enough
         *         to rule one out.
         */
        bool IsConvertible(const std::string &from,
                           const std::string &to,
                           const DiagnosticContext &ctx,
                           int depth = 0)
        {
            if (from.empty() || to.empty() || IsSameType(from, to))
            {
                return true;
            }

            const SymbolTable &table = ctx.request.symbolTable;

            if (ctx.request.IsRegisteredSymbol(from) || ctx.request.IsRegisteredSymbol(to))
            {
                return true;
            }

            const bool fromBuiltIn = IsBuiltInValueType(from, ctx);
            const bool toBuiltIn = IsBuiltInValueType(to, ctx);
            if (fromBuiltIn && toBuiltIn)
            {
                return true;
            }

            const TypeDeclarationInfo fromDecl = FindTypeDeclaration(from, table);
            const TypeDeclarationInfo toDecl = FindTypeDeclaration(to, table);

            // An unresolved name is an engine-registered type as far as this analyzer knows, and
            // engine types carry conversions that appear nowhere in the source.
            if ((!fromBuiltIn && !fromDecl.found) || (!toBuiltIn && !toDecl.found))
            {
                return true;
            }
            if (ResolvesToNonClassDeclaration(from, table) || ResolvesToNonClassDeclaration(to, table))
            {
                return true;
            }

            if (!fromBuiltIn && !toBuiltIn && AreHierarchyRelated(from, to, table))
            {
                return true;
            }
            if (!fromBuiltIn && DeclaresConversionTo(from, to, table, /*implicitOnly=*/false))
            {
                return true;
            }

            if (depth > 0)
            {
                return false;
            }

            // A template class is instantiated per element type; its declared parameter types are
            // written in terms of the template parameter, which says nothing about this call site.
            //
            // Currently unreachable, and kept anyway: ClassSignature::isTemplate is never set
            // because the grammar has no production for a template class declaration, so one can
            // never reach this checker to be judged. The guard is the right answer for the day the
            // grammar gains it, and it is one comparison.
            if (toDecl.isTemplate)
            {
                return true;
            }

            bool convertible = false;
            const auto acceptsFrom = [&](const Symbol &sym)
            {
                const auto &parameters = sym.GetFunction().parameters;
                if (AcceptsSingleArgument(parameters) &&
                    IsConvertible(from, SingleArgumentType(parameters), ctx, depth + 1))
                {
                    convertible = true;
                    return true;
                }
                return false;
            };

            ForEachConstructor(to, table, acceptsFrom);
            if (!convertible)
            {
                ForEachMethod(to, "opAssign", table, acceptsFrom);
            }
            return convertible;
        }

        /** @brief Classifies a numeric literal as integral or floating point. */
        std::string ClassifyNumberLiteral(const std::string &text)
        {
            const bool isHex = text.size() > 1 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X');
            if (!isHex)
            {
                for (const char c : text)
                {
                    if (c == '.' || c == 'e' || c == 'E' || c == 'f' || c == 'F')
                    {
                        return "float";
                    }
                }
            }
            return "int";
        }

        /** @brief Finds the class a 'this' expression refers to at a given node. */
        std::string EnclosingClassName(TSNode node, std::string_view sourceCode)
        {
            for (const auto &container : GetEnclosingContainers(node, sourceCode))
            {
                if (container.kind == ContainerKind::Class || container.kind == ContainerKind::Interface)
                {
                    return container.name;
                }
            }
            return "";
        }

        ExpressionType ResolveValueType(TSNode node,
                                        const Scope *scope,
                                        const DiagnosticContext &ctx,
                                        std::string_view sourceCode);

        /** @brief Resolves the type a bare (possibly scope-qualified) name denotes as a value. */
        ExpressionType ResolveIdentifierValueType(TSNode node,
                                                  const std::string &name,
                                                  const Scope *scope,
                                                  const DiagnosticContext &ctx,
                                                  std::string_view sourceCode)
        {
            if (name == "this")
            {
                const std::string className = EnclosingClassName(node, sourceCode);
                return className.empty() ? ExpressionType{} : ExpressionType{ className, true, false };
            }

            if (scope)
            {
                if (const LocalDefinition *def = ResolveInScope(scope, name); def && !def->typeName.empty())
                {
                    return ExpressionType{ CleanBaseType(def->typeName), true, false };
                }
            }

            ExpressionType result;
            ForEachSymbolNamed(name, ctx.request.symbolTable, [&result](const Symbol &sym)
            {
                // A bare function name is a function pointer, and a bare type name is not a value
                // at all. Neither has a value type worth judging, so both stay unknown.
                if (sym.type != SymbolType::Variable && sym.type != SymbolType::Property)
                {
                    return true;
                }
                if (!sym.GetVariable().typeName.empty())
                {
                    result = ExpressionType{ CleanBaseType(sym.GetVariable().typeName), true, false };
                    return true;
                }
                return false;
            });
            return result;
        }

        /** @brief Resolves what a call expression evaluates to: a constructed type, or a return type. */
        ExpressionType ResolveCallValueType(TSNode node,
                                            const DiagnosticContext &ctx,
                                            std::string_view sourceCode)
        {
            TSNode callee = ts_node_child_by_field_name(node, "function", k_functionFieldLength);
            if (ts_node_is_null(callee) && ts_node_child_count(node) > 0)
            {
                callee = ts_node_child(node, 0);
            }
            if (ts_node_is_null(callee))
            {
                return ExpressionType{};
            }

            const std::string calleeName = CleanBaseType(NodeText(callee, sourceCode));
            if (calleeName.empty())
            {
                return ExpressionType{};
            }

            // Type(args) constructs a value of that type; anything else is an ordinary call.
            if (FindTypeDeclaration(calleeName, ctx.request.symbolTable).found)
            {
                return ExpressionType{ LastScopeSegment(calleeName), true, false };
            }
            if (IsBuiltInValueType(calleeName, ctx))
            {
                return ExpressionType{ calleeName, true, false };
            }

            ExpressionType result;
            ForEachSymbolNamed(LastScopeSegment(calleeName), ctx.request.symbolTable, [&result](const Symbol &sym)
            {
                if (sym.type == SymbolType::Function && !sym.GetFunction().returnType.empty())
                {
                    result = ExpressionType{ CleanBaseType(sym.GetFunction().returnType), true, false };
                    return true;
                }
                return false;
            });
            return result;
        }

        ExpressionType ResolveValueType(TSNode node,
                                        const Scope *scope,
                                        const DiagnosticContext &ctx,
                                        std::string_view sourceCode)
        {
            if (ts_node_is_null(node))
            {
                return ExpressionType{};
            }

            const std::string_view nodeType = NodeType(node);

            if (nodeType == node_types::NumberLiteral)
            {
                return ExpressionType{ ClassifyNumberLiteral(NodeText(node, sourceCode)), true, true };
            }
            if (nodeType == node_types::StringLiteral)
            {
                return ExpressionType{ std::string(ctx.request.GetStringTypeName()), true, true };
            }
            if (nodeType == node_types::BooleanLiteral)
            {
                return ExpressionType{ "bool", true, true };
            }
            if (nodeType == node_types::NullLiteral)
            {
                // 'null' has its own rule (CheckNullAssignedToNonHandle) and no type of its own.
                return ExpressionType{};
            }

            if (nodeType == "parenthesized_expression")
            {
                return ts_node_named_child_count(node) > 0
                           ? ResolveValueType(ts_node_named_child(node, 0), scope, ctx, sourceCode)
                           : ExpressionType{};
            }

            if (nodeType == "unary_expression")
            {
                const std::string op = NodeText(
                    ts_node_child_by_field_name(node, "operator", k_operatorFieldLength), sourceCode);
                if (op == "!" || op == "not")
                {
                    return ExpressionType{ "bool", true, false };
                }
                return ResolveValueType(
                    ts_node_child_by_field_name(node, "operand", k_operandFieldLength), scope, ctx, sourceCode);
            }

            if (nodeType == "identifier" || nodeType == "scoped_identifier")
            {
                return ResolveIdentifierValueType(node, NodeText(node, sourceCode), scope, ctx, sourceCode);
            }

            if (nodeType == node_types::CallExpression || nodeType == "construct_call_expression")
            {
                return ResolveCallValueType(node, ctx, sourceCode);
            }

            if (nodeType == "cast_expression" || nodeType == "functional_cast_expression")
            {
                const std::string typeText = CleanBaseType(
                    NodeText(ts_node_child_by_field_name(node, "type", k_typeFieldLength), sourceCode));
                return typeText.empty() ? ExpressionType{} : ExpressionType{ typeText, true, false };
            }

            if (nodeType == "member_expression")
            {
                const std::string resolved = ResolveExpressionType(node, scope, ctx.request.symbolTable, sourceCode);
                return resolved.empty() ? ExpressionType{} : ExpressionType{ CleanBaseType(resolved), true, false };
            }

            // Binary, conditional and assignment expressions need operator resolution this pass
            // does not do, so their result type stays unknown rather than being guessed.
            return ExpressionType{};
        }

        /** @brief Emits at the exact source range of a node. */
        void EmitAtNode(TSNode node,
                        DiagnosticContext &ctx,
                        std::string_view code,
                        const std::string &from,
                        const std::string &to)
        {
            const TSPoint start = ts_node_start_point(node);
            const TSPoint end = ts_node_end_point(node);
            ctx.EmitAtRange(start.row, start.column, end.row, end.column, code, from, to,
                            DiagnosticSeverity::Error);
        }

        /** @brief Everything one declared type text says that the rules below need to know. */
        struct DeclaredType
        {
            std::string baseName;
            bool isHandle = false;
            bool usable = false;  ///< False when the shape is out of scope (array, template, unknown).
        };

        /** @brief Reads a 'type' node into the shape the conversion rules can act on.
         *  @note Arrays, templates and anything that does not resolve to a plain class are marked
         *        unusable: their conversion rules depend on element types this pass does not track. */
        DeclaredType ReadDeclaredType(TSNode typeNode, const DiagnosticContext &ctx, std::string_view sourceCode)
        {
            DeclaredType result;
            if (ts_node_is_null(typeNode))
            {
                return result;
            }

            const std::string raw = NodeText(typeNode, sourceCode);
            if (raw.empty() || raw.find('<') != std::string::npos || raw.find('[') != std::string::npos)
            {
                return result;
            }

            result.isHandle = raw.find('@') != std::string::npos;
            result.baseName = CleanBaseType(raw);
            if (result.baseName.empty() || IsBuiltInValueType(result.baseName, ctx))
            {
                return result;
            }
            if (ctx.request.IsRegisteredSymbol(result.baseName))
            {
                return result;
            }

            const TypeDeclarationInfo declaration = FindTypeDeclaration(result.baseName, ctx.request.symbolTable);
            if (!declaration.found || !declaration.isClass || declaration.isTemplate)
            {
                return result;
            }
            if (ResolvesToNonClassDeclaration(result.baseName, ctx.request.symbolTable))
            {
                return result;
            }

            result.usable = true;
            return result;
        }

        /** @brief Rule for `T v = expr;` - the implicit conversion route. */
        void CheckInitializer(TSNode declaratorNode,
                              const DeclaredType &declared,
                              const Scope *scope,
                              DiagnosticContext &ctx,
                              std::string_view sourceCode)
        {
            TSNode valueNode = ts_node_child_by_field_name(declaratorNode, "value", k_valueFieldLength);
            if (ts_node_is_null(valueNode) || NodeType(valueNode) == "initializer_list")
            {
                return;
            }

            const ExpressionType source = ResolveValueType(valueNode, scope, ctx, sourceCode);
            if (!source.known || source.baseName.empty())
            {
                return;
            }

            // A handle binds to objects, and which objects is a question of the whole interface
            // graph - much of which can be engine-side. A literal on the right-hand side needs
            // none of that graph to be wrong, so handles are judged on literals only.
            if (declared.isHandle && !source.isLiteral)
            {
                return;
            }

            if (IsConvertible(source.baseName, declared.baseName, ctx))
            {
                return;
            }

            // NOT IMPLEMENTED: as-err-implicit-conversion, the older code for this same condition.
            // Its message names only the source type; as-err-no-implicit-conversion names both and
            // says which declaration would fix it, so it is the one emitted.
            EmitAtNode(valueNode, ctx, "as-err-no-implicit-conversion", source.baseName, declared.baseName);
        }

        /** @brief Rule for a one-argument construction: `T(expr)` or `T v(expr);`. */
        void CheckConstruction(TSNode argumentListNode,
                               const std::string &targetType,
                               const Scope *scope,
                               DiagnosticContext &ctx,
                               std::string_view sourceCode)
        {
            // Only single-argument constructions are conversions. Anything else is overload
            // resolution over a full argument list, which this pass does not attempt.
            if (ts_node_is_null(argumentListNode) || ts_node_named_child_count(argumentListNode) != 1)
            {
                return;
            }

            TSNode argument = ts_node_named_child(argumentListNode, 0);
            const ExpressionType source = ResolveValueType(argument, scope, ctx, sourceCode);
            if (!source.known || source.baseName.empty() || IsSameType(source.baseName, targetType))
            {
                return;
            }

            const SymbolTable &table = ctx.request.symbolTable;

            if (AreHierarchyRelated(source.baseName, targetType, table))
            {
                return;
            }
            if (DeclaresConversionTo(source.baseName, targetType, table, /*implicitOnly=*/false))
            {
                return;
            }

            bool constructible = false;
            ForEachConstructor(targetType, table, [&](const Symbol &sym)
            {
                const auto &parameters = sym.GetFunction().parameters;
                if (AcceptsSingleArgument(parameters) &&
                    IsConvertible(source.baseName, SingleArgumentType(parameters), ctx, 1))
                {
                    constructible = true;
                    return true;
                }
                return false;
            });

            if (!constructible)
            {
                EmitAtNode(argument, ctx, "as-err-no-explicit-conversion", source.baseName, targetType);
            }
        }

        /** @brief Rule for `cast<T>(expr)` - the reinterpreting route. */
        void CheckCast(TSNode castNode,
                       const Scope *scope,
                       DiagnosticContext &ctx,
                       std::string_view sourceCode)
        {
            const std::string targetName = CleanBaseType(
                NodeText(ts_node_child_by_field_name(castNode, "type", k_typeFieldLength), sourceCode));

            TSNode valueNode = ts_node_child_by_field_name(castNode, "value", k_valueFieldLength);
            if (ts_node_is_null(valueNode) || targetName.empty())
            {
                return;
            }

            const SymbolTable &table = ctx.request.symbolTable;

            // Unlike the other two rules the target may legitimately be an interface here, so the
            // class-only ReadDeclaredType is not what decides visibility.
            if (!FindTypeDeclaration(targetName, table).found)
            {
                return;
            }

            const ExpressionType source = ResolveValueType(valueNode, scope, ctx, sourceCode);
            if (!source.known || source.baseName.empty() || IsSameType(source.baseName, targetName))
            {
                return;
            }
            if (!FindTypeDeclaration(source.baseName, table).found)
            {
                return;
            }
            if (ctx.request.IsRegisteredSymbol(source.baseName) || ctx.request.IsRegisteredSymbol(targetName))
            {
                return;
            }

            if (AreHierarchyRelated(source.baseName, targetName, table))
            {
                return;
            }
            if (DeclaresAnyCastOperator(source.baseName, table) || DeclaresAnyCastOperator(targetName, table))
            {
                return;
            }

            EmitAtNode(castNode, ctx, "as-err-invalid-cast", source.baseName, targetName);
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

        void VisitNode(TSNode node, const TypeConversionCheckRequest &request, DiagnosticContext &ctx)
        {
            const std::string_view nodeType = NodeType(node);

            // Resolved lazily: only the three rules below need the scope walk, and paying for it on
            // every node of the tree costs more than the rules themselves.
            const Scope *scope = nullptr;
            bool scopeResolved = false;
            const auto scopeAt = [&]() -> const Scope *
            {
                if (!scopeResolved)
                {
                    const TSPoint start = ts_node_start_point(node);
                    scope = FindInnermostScope(request.scopeRoot, start.row, start.column);
                    scopeResolved = true;
                }
                return scope;
            };

            if (nodeType == "variable_declaration")
            {
                const DeclaredType declared = ReadDeclaredType(
                    ts_node_child_by_field_name(node, "var_type", k_varTypeFieldLength), ctx, request.sourceCode);

                if (declared.usable)
                {
                    for (uint32_t i = 0; i < ts_node_named_child_count(node); ++i)
                    {
                        TSNode child = ts_node_named_child(node, i);
                        if (NodeType(child) != "variable_declarator")
                        {
                            continue;
                        }

                        CheckInitializer(child, declared, scopeAt(), ctx, request.sourceCode);

                        if (!declared.isHandle)
                        {
                            CheckConstruction(ts_node_child_by_field_name(child, "arguments", k_argsFieldLength),
                                              declared.baseName, scopeAt(), ctx, request.sourceCode);
                        }
                    }
                }
            }
            else if (nodeType == "cast_expression")
            {
                CheckCast(node, scopeAt(), ctx, request.sourceCode);
            }
            else if (nodeType == node_types::CallExpression)
            {
                TSNode callee = ts_node_child_by_field_name(node, "function", k_functionFieldLength);
                if (ts_node_is_null(callee) && ts_node_child_count(node) > 0)
                {
                    callee = ts_node_child(node, 0);
                }

                const std::string calleeName = CleanBaseType(NodeText(callee, request.sourceCode));
                const DeclaredType target = ReadDeclaredType(callee, ctx, request.sourceCode);
                if (target.usable && !target.isHandle && IsSameType(calleeName, target.baseName))
                {
                    CheckConstruction(ts_node_child_by_field_name(node, "arguments", k_argsFieldLength),
                                      target.baseName, scopeAt(), ctx, request.sourceCode);
                }
            }

            // Named children only: every node these rules match is a named one, and anonymous
            // token nodes ('(', '=', ';') are leaves with nothing underneath them to find.
            const uint32_t childCount = ts_node_named_child_count(node);
            for (uint32_t i = 0; i < childCount; ++i)
            {
                VisitNode(ts_node_named_child(node, i), request, ctx);
            }
        }
    }

    void CheckTypeConversions(const TypeConversionCheckRequest &request, DiagnosticContext &ctx)
    {
        if (ts_node_is_null(request.root) || request.sourceCode.empty())
        {
            return;
        }

        VisitNode(request.root, request, ctx);
    }
}
