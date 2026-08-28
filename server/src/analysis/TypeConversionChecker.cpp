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
        constexpr uint32_t k_nameFieldLength = 4;      ///< "name"

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

            ForEachSymbolNamed(typeName, table, [&info](const Symbol &sym)
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

            if (info.found)
            {
                return info;
            }

            const std::string bare = LastScopeSegment(typeName);
            table.ForEachSymbol([&](const std::string &qName, const std::vector<Symbol> &symbols)
            {
                if (!info.found && (qName == bare || LastScopeSegment(qName) == bare))
                {
                    for (const auto &sym : symbols)
                    {
                        if (sym.type == SymbolType::Class || sym.type == SymbolType::Interface)
                        {
                            info.found = true;
                            info.isClass = sym.type == SymbolType::Class;
                            info.isTemplate = info.isClass && sym.GetClass().isTemplate;
                            return;
                        }
                    }
                }
            });

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
            const auto strType = ctx.request.GetStringTypeName();
            const std::string_view effectiveStrType = strType.empty() ? std::string_view("string") : strType;
            return IsPrimitiveTypeName(typeName) || typeName == effectiveStrType;
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
                if (from == to)
                {
                    return true;
                }
                const auto isNumeric = [](const std::string &t)
                {
                    return t == "int" || t == "int8" || t == "int16" || t == "int32" || t == "int64" ||
                           t == "uint" || t == "uint8" || t == "uint16" || t == "uint32" || t == "uint64" ||
                           t == "float" || t == "double";
                };
                const bool fromNum = isNumeric(from);
                const bool toNum = isNumeric(to);
                if (fromNum && toNum)
                {
                    return true;
                }
                if (from == "bool" && toNum)
                {
                    return true;
                }
                if (fromNum && to == "bool")
                {
                    return true;
                }
                return false;
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
            // Reachable since the grammar gained a template class declaration and SymbolCollector
            // began setting ClassSignature::isTemplate from it. This comment used to say the
            // opposite and had simply gone stale.
            if (toDecl.isTemplate)
            {
                return true;
            }

            bool convertible = false;
            const auto acceptsFrom = [&](const Symbol &sym)
            {
                if (sym.GetFunction().modifiers.isExplicit || sym.GetFunction().modifiers.isDelete)
                {
                    return false;
                }
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
                return ExpressionType{ calleeName, true, false };
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
                const auto strType = ctx.request.GetStringTypeName();
                return ExpressionType{ strType.empty() ? "string" : std::string(strType), true, true };
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

        struct PropertyAccessInfo
        {
            bool isProperty = false;
            bool hasGet = false;
            bool hasSet = false;
            bool isIndexed = false;
            std::string propName;
            std::string receiverType;
        };

        PropertyAccessInfo InspectPropertyAccess(TSNode exprNode, const Scope *scope, const SymbolTable &table, std::string_view sourceCode, const std::string &uri)
        {
            PropertyAccessInfo info;
            std::string_view nt = NodeType(exprNode);
            if (nt == "member_expression")
            {
                TSNode objNode = ts_node_child_by_field_name(exprNode, "object", 6);
                TSNode memNode = ts_node_child_by_field_name(exprNode, "member", 6);
                if (ts_node_is_null(objNode) && ts_node_named_child_count(exprNode) > 0)
                {
                    objNode = ts_node_named_child(exprNode, 0);
                    if (ts_node_named_child_count(exprNode) > 1)
                    {
                        memNode = ts_node_named_child(exprNode, 1);
                    }
                }
                if (!ts_node_is_null(objNode) && !ts_node_is_null(memNode))
                {
                    info.propName = NodeText(memNode, sourceCode);
                    info.receiverType = ResolveExpressionType(objNode, scope, table, sourceCode, uri);
                    std::string cleanObj = CleanBaseType(info.receiverType);
                    if (!cleanObj.empty())
                    {
                        auto hierarchy = GetInheritedTypeHierarchy(cleanObj, table);
                        for (const auto &typeName : hierarchy)
                        {
                            auto propSyms = table.FindSymbolsPtr(typeName + "::" + info.propName);
                            if (propSyms)
                            {
                                for (const auto &s : *propSyms)
                                {
                                    if (s.type == SymbolType::Property && std::holds_alternative<VariableSignature>(s.signature))
                                    {
                                        const auto &vs = s.GetVariable();
                                        if (vs.isVirtualProperty)
                                        {
                                            info.isProperty = true;
                                            info.hasGet = vs.hasGet;
                                            info.hasSet = vs.hasSet;
                                            return info;
                                        }
                                    }
                                }
                            }
                            auto getSyms = table.FindSymbolsPtr(typeName + "::get_" + info.propName);
                            if (getSyms && !getSyms->empty())
                            {
                                info.isProperty = true;
                                info.hasGet = true;
                                for (const auto &gs : *getSyms)
                                {
                                    if (gs.type == SymbolType::Function && !gs.GetFunction().parameters.empty())
                                    {
                                        info.isIndexed = true;
                                    }
                                }
                            }
                            auto setSyms = table.FindSymbolsPtr(typeName + "::set_" + info.propName);
                            if (setSyms && !setSyms->empty())
                            {
                                info.isProperty = true;
                                info.hasSet = true;
                                for (const auto &ss : *setSyms)
                                {
                                    if (ss.type == SymbolType::Function && ss.GetFunction().parameters.size() > 1)
                                    {
                                        info.isIndexed = true;
                                    }
                                }
                            }
                            if (info.isProperty)
                            {
                                return info;
                            }
                        }
                    }
                }
            }
            return info;
        }

        /** @brief Emits at the exact source range of a node. */
        void EmitAtNode(TSNode node,
                        DiagnosticContext &ctx,
                        std::string_view code,
                        const std::string &from = "",
                        const std::string &to = "")
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
            if (raw.empty())
            {
                return result;
            }

            result.isHandle = raw.find('@') != std::string::npos;
            result.baseName = CleanBaseType(raw);
            if (result.baseName.empty())
            {
                return result;
            }
            if (ctx.request.IsRegisteredSymbol(result.baseName))
            {
                return result;
            }

            if (IsBuiltInValueType(result.baseName, ctx))
            {
                result.usable = true;
                return result;
            }

            if (raw.find('<') != std::string::npos || raw.find('[') != std::string::npos)
            {
                result.usable = true;
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
                std::string identName = NodeText(valueNode, sourceCode);
                while (!identName.empty() && isspace(static_cast<unsigned char>(identName.front()))) identName.erase(identName.begin());
                while (!identName.empty() && isspace(static_cast<unsigned char>(identName.back()))) identName.pop_back();

                if (!identName.empty())
                {
                    bool isTypeOrTemplate = false;
                    ForEachSymbolNamed(identName, ctx.request.symbolTable, [&](const Symbol &s) -> bool
                    {
                        if (s.type == SymbolType::Class || s.type == SymbolType::Interface || s.type == SymbolType::Typedef || s.type == SymbolType::Enum)
                        {
                            isTypeOrTemplate = true;
                            return false;
                        }
                        return true;
                    });
                    if (isTypeOrTemplate)
                    {
                        EmitAtNode(valueNode, ctx, "as-err-no-implicit-conversion", identName, declared.baseName);
                    }
                }
                return;
            }

            // A handle binds to objects.
            if (declared.isHandle)
            {
                if (source.baseName == "null")
                {
                    return;
                }
                if (source.isLiteral)
                {
                    EmitAtNode(valueNode, ctx, "as-err-no-implicit-conversion", source.baseName, declared.baseName);
                    return;
                }

                if (IsSameType(source.baseName, declared.baseName))
                {
                    return;
                }

                // If declared is a derived class of source, that's an invalid downcast without cast<T>
                if (ctx.request.symbolTable.HasSymbolAnywhere(source.baseName) && ctx.request.symbolTable.HasSymbolAnywhere(declared.baseName))
                {
                    const auto hierarchy = GetInheritedTypeHierarchy(declared.baseName, ctx.request.symbolTable);
                    if (std::find(hierarchy.begin(), hierarchy.end(), source.baseName) != hierarchy.end())
                    {
                        EmitAtNode(valueNode, ctx, "as-err-no-implicit-conversion", source.baseName + "@", declared.baseName + "@");
                        return;
                    }
                }
                return;
            }

            if (IsConvertible(source.baseName, declared.baseName, ctx))
            {
                return;
            }

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

        void CheckDefaultConstructor(TSNode declaratorNode,
                                     const std::string &typeName,
                                     DiagnosticContext &ctx)
        {
            if (typeName.empty() || IsBuiltInValueType(typeName, ctx))
            {
                return;
            }

            const SymbolTable &table = ctx.request.symbolTable;
            const TypeDeclarationInfo decl = FindTypeDeclaration(typeName, table);
            if (!decl.found || !decl.isClass || decl.isTemplate)
            {
                return;
            }

            std::vector<Symbol> constructors;
            ForEachConstructor(typeName, table, [&](const Symbol &sym)
            {
                constructors.push_back(sym);
                return false;
            });

            if (constructors.empty())
            {
                return;
            }

            bool hasZeroArg = false;
            bool zeroArgDeleted = false;
            for (const auto &ctor : constructors)
            {
                const auto &sig = ctor.GetFunction();
                bool canTakeZero = sig.parameters.empty();
                if (!canTakeZero)
                {
                    canTakeZero = std::all_of(sig.parameters.begin(), sig.parameters.end(),
                                              [](const ParameterInformation &p) { return !p.defaultValue.empty(); });
                }
                if (canTakeZero)
                {
                    hasZeroArg = true;
                    if (sig.modifiers.isDelete)
                    {
                        zeroArgDeleted = true;
                    }
                    break;
                }
            }

            TSNode nameNode = ts_node_child_by_field_name(declaratorNode, "name", 4);
            TSNode targetNode = ts_node_is_null(nameNode) ? declaratorNode : nameNode;

            if (zeroArgDeleted)
            {
                EmitAtNode(targetNode, ctx, "as-err-deleted-method-called", typeName, typeName);
            }
            else if (!hasZeroArg)
            {
                EmitAtNode(targetNode, ctx, "as-err-no-default-constructor", typeName);
            }
        }

        std::optional<Symbol> FindFuncdef(const std::string &name, const SymbolTable &table)
        {
            std::optional<Symbol> result;
            ForEachSymbolNamed(name, table, [&](const Symbol &sym)
            {
                if (sym.type == SymbolType::Funcdef)
                {
                    result = sym;
                    return true;
                }
                return false;
            });
            if (result)
            {
                return result;
            }
            const std::string bare = LastScopeSegment(name);
            table.ForEachSymbol([&](const std::string &qName, const std::vector<Symbol> &symbols)
            {
                if (!result && (qName == bare || LastScopeSegment(qName) == bare))
                {
                    for (const auto &sym : symbols)
                    {
                        if (sym.type == SymbolType::Funcdef)
                        {
                            result = sym;
                            return;
                        }
                    }
                }
            });
            return result;
        }

        bool MatchesFuncdefSignature(const FunctionSignature &fn, const FuncdefSignature &fd)
        {
            if (CleanBaseType(fn.returnType) != CleanBaseType(fd.returnType))
            {
                return false;
            }
            if (fn.parameters.size() != fd.parameters.size())
            {
                return false;
            }
            for (size_t i = 0; i < fn.parameters.size(); ++i)
            {
                if (CleanBaseType(fn.parameters[i].typeName) != CleanBaseType(fd.parameters[i].typeName))
                {
                    return false;
                }
                if (fn.parameters[i].modifier != fd.parameters[i].modifier)
                {
                    return false;
                }
                if (fn.parameters[i].isReference != fd.parameters[i].isReference)
                {
                    return false;
                }
                if (fn.parameters[i].isHandle != fd.parameters[i].isHandle)
                {
                    return false;
                }
            }
            return true;
        }

        void CheckFuncdefAssignment(TSNode targetNode,
                                    const FuncdefSignature &funcdefSig,
                                    TSNode valueNode,
                                    const Scope *scope,
                                    DiagnosticContext &ctx,
                                    std::string_view sourceCode)
        {
            if (ts_node_is_null(valueNode))
            {
                return;
            }

            TSNode actualVal = valueNode;
            if (std::string_view(NodeType(valueNode)) == "unary_expression")
            {
                TSNode op = ts_node_child_by_field_name(valueNode, "operator", 8);
                if (!ts_node_is_null(op) && NodeText(op, sourceCode) == "@")
                {
                    TSNode operand = ts_node_child_by_field_name(valueNode, "operand", 7);
                    if (!ts_node_is_null(operand))
                    {
                        actualVal = operand;
                    }
                }
            }

            std::string funcName = NodeText(actualVal, sourceCode);
            while (!funcName.empty() && isspace(static_cast<unsigned char>(funcName.front()))) funcName.erase(funcName.begin());
            while (!funcName.empty() && isspace(static_cast<unsigned char>(funcName.back()))) funcName.pop_back();

            if (funcName.empty() || funcName == "null")
            {
                return;
            }

            std::vector<Symbol> candidates;
            auto found = ctx.request.symbolTable.FindSymbols(funcName);
            for (const auto &s : found)
            {
                if (s.type == SymbolType::Function)
                {
                    candidates.push_back(s);
                }
            }
            if (candidates.empty())
            {
                std::string bare = LastScopeSegment(funcName);
                auto all = ctx.request.symbolTable.FindSymbols(bare);
                for (const auto &s : all)
                {
                    if (s.type == SymbolType::Function)
                    {
                        candidates.push_back(s);
                    }
                }
            }

            if (candidates.empty())
            {
                return;
            }

            bool matched = false;
            for (const auto &cand : candidates)
            {
                if (MatchesFuncdefSignature(cand.GetFunction(), funcdefSig))
                {
                    matched = true;
                    break;
                }
            }

            if (!matched)
            {
                EmitAtNode(targetNode, ctx, "as-err-signature-mismatch-func-handle");
            }
        }

        void CheckConstructorDelegation(TSNode funcNode,
                                        DiagnosticContext &ctx,
                                        std::string_view sourceCode)
        {
            TSNode parent = ts_node_parent(funcNode);
            while (!ts_node_is_null(parent) && std::string_view(ts_node_type(parent)) != "class_declaration")
            {
                parent = ts_node_parent(parent);
            }
            if (ts_node_is_null(parent))
            {
                return;
            }

            TSNode classNameNode = ts_node_child_by_field_name(parent, "name", 4);
            if (ts_node_is_null(classNameNode))
            {
                return;
            }
            std::string className = NodeText(classNameNode, sourceCode);

            TSNode funcNameNode = ts_node_child_by_field_name(funcNode, "name", 4);
            if (ts_node_is_null(funcNameNode) || NodeText(funcNameNode, sourceCode) != className)
            {
                return;
            }

            TSNode bodyNode = ts_node_child_by_field_name(funcNode, "body", 4);
            if (ts_node_is_null(bodyNode))
            {
                return;
            }

            const uint32_t stmtCount = ts_node_named_child_count(bodyNode);
            for (uint32_t i = 0; i < stmtCount; ++i)
            {
                TSNode stmt = ts_node_named_child(bodyNode, i);
                if (std::string_view(ts_node_type(stmt)) != "expression_statement")
                {
                    continue;
                }

                if (ts_node_named_child_count(stmt) == 0)
                {
                    continue;
                }

                TSNode expr = ts_node_named_child(stmt, 0);
                std::string_view exprType = ts_node_type(expr);
                if (exprType == node_types::CallExpression || exprType == "construct_call_expression")
                {
                    TSNode callee = ts_node_child_by_field_name(expr, "function", k_functionFieldLength);
                    if (ts_node_is_null(callee))
                    {
                        callee = ts_node_child_by_field_name(expr, "type", k_typeFieldLength);
                    }
                    if (ts_node_is_null(callee) && ts_node_child_count(expr) > 0)
                    {
                        callee = ts_node_child(expr, 0);
                    }
                    if (!ts_node_is_null(callee) && NodeText(callee, sourceCode) == className)
                    {
                        EmitAtNode(expr, ctx, "as-err-constructor-delegation-disallowed");
                    }
                }
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
                TSNode typeNode = ts_node_child_by_field_name(node, "var_type", k_varTypeFieldLength);
                if (ts_node_is_null(typeNode))
                {
                    typeNode = ts_node_child_by_field_name(node, "type", k_typeFieldLength);
                }

                std::string rawType = CleanBaseType(NodeText(typeNode, request.sourceCode));
                if (rawType == "auto")
                {
                    for (uint32_t i = 0; i < ts_node_named_child_count(node); ++i)
                    {
                        TSNode child = ts_node_named_child(node, i);
                        if (NodeType(child) != "variable_declarator")
                        {
                            continue;
                        }

                        TSNode nameNode = ts_node_child_by_field_name(child, "name", k_nameFieldLength);
                        std::string varName = NodeText(nameNode, request.sourceCode);

                        TSNode valueNode = ts_node_child_by_field_name(child, "value", k_valueFieldLength);
                        if (ts_node_is_null(valueNode))
                        {
                            valueNode = ts_node_child_by_field_name(child, "initializer", 11);
                        }
                        if (ts_node_is_null(valueNode))
                        {
                            uint32_t childCount = ts_node_child_count(child);
                            bool foundEq = false;
                            for (uint32_t c = 0; c < childCount; ++c)
                            {
                                TSNode ch = ts_node_child(child, c);
                                if (foundEq)
                                {
                                    valueNode = ch;
                                    break;
                                }
                                if (NodeText(ch, request.sourceCode) == "=")
                                {
                                    foundEq = true;
                                }
                            }
                        }

                        if (ts_node_is_null(valueNode))
                        {
                            EmitAtNode(child, ctx, "as-err-auto-requires-initializer");
                            continue;
                        }

                        // Check cyclic auto dependency (e.g. auto invalid2 = invalid2 + 1)
                        std::string valueText = NodeText(valueNode, request.sourceCode);
                        bool isCyclic = false;
                        if (!varName.empty())
                        {
                            size_t pos = 0;
                            while ((pos = valueText.find(varName, pos)) != std::string::npos)
                            {
                                bool leftBoundary = (pos == 0 || (!isalnum(static_cast<unsigned char>(valueText[pos - 1])) && valueText[pos - 1] != '_'));
                                bool rightBoundary = (pos + varName.size() >= valueText.size() || (!isalnum(static_cast<unsigned char>(valueText[pos + varName.size()])) && valueText[pos + varName.size()] != '_'));
                                if (leftBoundary && rightBoundary)
                                {
                                    isCyclic = true;
                                    break;
                                }
                                pos += varName.size();
                            }
                        }

                        if (isCyclic)
                        {
                            EmitAtNode(valueNode, ctx, "as-err-cyclic-auto-dependency", varName);
                            continue;
                        }

                        std::string rhsType = ResolveExpressionType(valueNode, scopeAt(), ctx.request.symbolTable, request.sourceCode, ctx.request.fileUri);
                        if (rhsType == "void")
                        {
                            EmitAtNode(valueNode, ctx, "as-err-cannot-infer-void");
                        }
                        else if (rhsType == "null")
                        {
                            EmitAtNode(valueNode, ctx, "as-err-cannot-infer-null");
                        }
                        else if (!rhsType.empty() && scopeAt())
                        {
                            const LocalDefinition *def = ResolveInScope(scopeAt(), varName);
                            if (def && (def->typeName == "auto" || def->typeName == "auto@"))
                            {
                                const_cast<LocalDefinition *>(def)->typeName = rhsType;
                            }
                        }
                    }
                }
                else
                {
                    const std::string rawType = NodeText(typeNode, request.sourceCode);
                    const std::string baseType = CleanBaseType(rawType);
                    auto funcdefSym = FindFuncdef(baseType, ctx.request.symbolTable);
                    if (funcdefSym)
                    {
                        for (uint32_t i = 0; i < ts_node_named_child_count(node); ++i)
                        {
                            TSNode child = ts_node_named_child(node, i);
                            if (NodeType(child) != "variable_declarator")
                            {
                                continue;
                            }
                            TSNode valNode = ts_node_child_by_field_name(child, "value", k_valueFieldLength);
                            CheckFuncdefAssignment(child, funcdefSym->GetFuncdef(), valNode, scopeAt(), ctx, request.sourceCode);
                        }
                    }

                    const DeclaredType declared = ReadDeclaredType(typeNode, ctx, request.sourceCode);
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
                                if (ClassifyNonInstantiable(declared.baseName, ctx.request.symbolTable) == NonInstantiableKind::Abstract)
                                {
                                    EmitAtNode(child, ctx, "as-err-abstract-instantiated", declared.baseName, declared.baseName);
                                }
                                else
                                {
                                    TSNode argsNode = ts_node_child_by_field_name(child, "arguments", k_argsFieldLength);
                                    TSNode valNode = ts_node_child_by_field_name(child, "value", k_valueFieldLength);
                                    if (ts_node_is_null(argsNode) && ts_node_is_null(valNode))
                                    {
                                        CheckDefaultConstructor(child, declared.baseName, ctx);
                                    }
                                    else if (!ts_node_is_null(argsNode))
                                    {
                                        CheckConstruction(argsNode, declared.baseName, scopeAt(), ctx, request.sourceCode);
                                    }
                                }
                            }
                        }
                    }
                }
            }
            else if (nodeType == "assignment_expression")
            {
                TSNode left = ts_node_child_by_field_name(node, "left", 4);
                TSNode right = ts_node_child_by_field_name(node, "right", 5);
                TSNode opNode = ts_node_child_by_field_name(node, "operator", 8);
                std::string opText = NodeText(opNode, request.sourceCode);
                if (!ts_node_is_null(left) && !ts_node_is_null(right))
                {
                    // Property access checks
                    std::string_view leftNodeType = NodeType(left);
                    if (leftNodeType == "subscript_expression" || leftNodeType == "index_expression")
                    {
                        TSNode arrayNode = ts_node_child_by_field_name(left, "array", 5);
                        if (ts_node_is_null(arrayNode) && ts_node_named_child_count(left) > 0)
                        {
                            arrayNode = ts_node_named_child(left, 0);
                        }
                        PropertyAccessInfo pInfo = InspectPropertyAccess(arrayNode, scopeAt(), ctx.request.symbolTable, request.sourceCode, ctx.request.fileUri);
                        if (pInfo.isProperty && pInfo.isIndexed && opText != "=")
                        {
                            EmitAtNode(node, ctx, "as-err-compound-assign-on-indexed-prop", pInfo.propName);
                        }
                    }
                    else
                    {
                        PropertyAccessInfo pInfo = InspectPropertyAccess(left, scopeAt(), ctx.request.symbolTable, request.sourceCode, ctx.request.fileUri);
                        if (pInfo.isProperty)
                        {
                            if (!pInfo.hasSet && pInfo.hasGet)
                            {
                                EmitAtNode(left, ctx, "as-err-read-only-property", pInfo.propName);
                            }
                            else if (opText != "=")
                            {
                                if (pInfo.receiverType.find('@') == std::string::npos)
                                {
                                    EmitAtNode(node, ctx, "as-err-compound-assign-on-value-prop", pInfo.propName);
                                }
                            }
                        }
                    }

                    std::string leftType = ResolveExpressionType(left, scopeAt(), ctx.request.symbolTable, request.sourceCode, ctx.request.fileUri);
                    std::string rightType = ResolveExpressionType(right, scopeAt(), ctx.request.symbolTable, request.sourceCode, ctx.request.fileUri);
                    std::string cleanLeft = CleanBaseType(leftType);
                    std::string cleanRight = CleanBaseType(rightType);

                    auto leftFuncdef = FindFuncdef(cleanLeft, ctx.request.symbolTable);
                    if (leftFuncdef)
                    {
                        CheckFuncdefAssignment(node, leftFuncdef->GetFuncdef(), right, scopeAt(), ctx, request.sourceCode);
                    }

                    // Handle assignment const qualifier discard check
                    bool isHandleAssignment = false;
                    TSNode actualLeft = left;
                    TSNode actualRight = right;
                    if (std::string_view(NodeType(left)) == "unary_expression")
                    {
                        TSNode op = ts_node_child_by_field_name(left, "operator", 8);
                        if (!ts_node_is_null(op) && NodeText(op, request.sourceCode) == "@")
                        {
                            isHandleAssignment = true;
                            TSNode operand = ts_node_child_by_field_name(left, "operand", 7);
                            if (!ts_node_is_null(operand))
                            {
                                actualLeft = operand;
                            }
                        }
                    }
                    if (std::string_view(NodeType(right)) == "unary_expression")
                    {
                        TSNode op = ts_node_child_by_field_name(right, "operator", 8);
                        if (!ts_node_is_null(op) && NodeText(op, request.sourceCode) == "@")
                        {
                            TSNode operand = ts_node_child_by_field_name(right, "operand", 7);
                            if (!ts_node_is_null(operand))
                            {
                                actualRight = operand;
                            }
                        }
                    }

                    if (isHandleAssignment)
                    {
                        auto getFullType = [&](TSNode n) -> std::string
                        {
                            std::string name = NodeText(n, request.sourceCode);
                            if (scopeAt())
                            {
                                if (const auto *def = ResolveInScope(scopeAt(), LastScopeSegment(name)))
                                {
                                    return def->typeName;
                                }
                            }
                            if (auto syms = ctx.request.symbolTable.FindSymbolsPtr(name))
                            {
                                for (const auto &s : *syms)
                                {
                                    if (s.type == SymbolType::Variable || s.type == SymbolType::Property)
                                    {
                                        return s.GetVariable().typeName;
                                    }
                                }
                            }
                            return "";
                        };

                        std::string leftFull = getFullType(actualLeft);
                        std::string rightFull = getFullType(actualRight);
                        bool rightConstTarget = rightFull.starts_with("const ");
                        bool leftConstTarget = leftFull.starts_with("const ");
                        if (rightConstTarget && !leftConstTarget && !cleanRight.empty() && !cleanLeft.empty())
                        {
                            EmitAtNode(right, ctx, "as-err-no-implicit-conversion", "const " + cleanRight + "@", cleanLeft + "@");
                        }
                    }

                    // Check if opAssign is explicitly deleted on LHS class
                    if (!cleanLeft.empty())
                    {
                        auto opSyms = ctx.request.symbolTable.FindSymbolsPtr(cleanLeft + "::opAssign");
                        if (opSyms)
                        {
                            for (const auto &sym : *opSyms)
                            {
                                if (sym.type == SymbolType::Function &&
                                    std::holds_alternative<FunctionSignature>(sym.signature) &&
                                    sym.GetFunction().modifiers.isDelete)
                                {
                                    EmitAtNode(node, ctx, "as-err-deleted-method-called", cleanLeft, "opAssign");
                                    break;
                                }
                            }
                        }
                    }

                    if (!cleanLeft.empty() && !cleanRight.empty() && cleanLeft != cleanRight)
                    {
                        if (!IsConvertible(cleanRight, cleanLeft, ctx))
                        {
                            EmitAtNode(right, ctx, "as-err-no-implicit-conversion", cleanRight, cleanLeft);
                        }
                    }
                }
            }
            else if (nodeType == "update_expression" || nodeType == "unary_expression" || nodeType == "postfix_expression")
            {
                TSNode opNode = ts_node_child_by_field_name(node, "operator", 8);
                std::string opText = NodeText(opNode, request.sourceCode);
                std::string nodeText = NodeText(node, request.sourceCode);
                if (opText == "++" || opText == "--" || nodeText.find("++") != std::string::npos || nodeText.find("--") != std::string::npos)
                {
                    TSNode argNode = ts_node_child_by_field_name(node, "argument", 8);
                    if (ts_node_is_null(argNode))
                    {
                        argNode = ts_node_child_by_field_name(node, "operand", 7);
                    }
                    if (ts_node_is_null(argNode))
                    {
                        uint32_t count = ts_node_named_child_count(node);
                        for (uint32_t i = 0; i < count; ++i)
                        {
                            TSNode ch = ts_node_named_child(node, i);
                            std::string_view ct = ts_node_type(ch);
                            if (ct != "operator" && ct != "++" && ct != "--")
                            {
                                argNode = ch;
                                break;
                            }
                        }
                    }
                    if (!ts_node_is_null(argNode))
                    {
                        PropertyAccessInfo pInfo = InspectPropertyAccess(argNode, scopeAt(), ctx.request.symbolTable, request.sourceCode, ctx.request.fileUri);
                        if (pInfo.isProperty)
                        {
                            EmitAtNode(node, ctx, "as-err-inc-dec-on-virtual-prop", pInfo.propName);
                        }
                    }
                }
                else if (nodeType == "unary_expression" && (opText == "-" || (!opNode.id && nodeText.rfind("-", 0) == 0)))
                {
                    TSNode operand = ts_node_child_by_field_name(node, "operand", 7);
                    if (ts_node_is_null(operand) && ts_node_named_child_count(node) > 0)
                    {
                        operand = ts_node_named_child(node, 0);
                    }
                    if (!ts_node_is_null(operand))
                    {
                        std::string opType = CleanBaseType(ResolveExpressionType(
                            operand, scopeAt(), ctx.request.symbolTable, request.sourceCode, ctx.request.fileUri));
                        if (opType == "uint" || opType == "uint8" || opType == "uint16" ||
                            opType == "uint32" || opType == "uint64")
                        {
                            EmitAtNode(node, ctx, "as-err-unary-neg-on-unsigned", opType);
                        }
                    }
                }
            }
            else if (nodeType == "member_expression")
            {
                TSNode parent = ts_node_parent(node);
                bool isLhsAssignment = false;
                bool isUnaryArg = false;
                if (!ts_node_is_null(parent))
                {
                    std::string_view pType = ts_node_type(parent);
                    if (pType == "assignment_expression")
                    {
                        TSNode leftChild = ts_node_child_by_field_name(parent, "left", 4);
                        if (ts_node_eq(leftChild, node))
                        {
                            isLhsAssignment = true;
                        }
                    }
                    else if (pType == "update_expression" || pType == "unary_expression" || pType == "postfix_expression")
                    {
                        isUnaryArg = true;
                    }
                }
                if (!isLhsAssignment && !isUnaryArg)
                {
                    PropertyAccessInfo pInfo = InspectPropertyAccess(node, scopeAt(), ctx.request.symbolTable, request.sourceCode, ctx.request.fileUri);
                    if (pInfo.isProperty && !pInfo.hasGet && pInfo.hasSet)
                    {
                        EmitAtNode(node, ctx, "as-err-write-only-property", pInfo.propName);
                    }
                }
            }
            else if (nodeType == "if_statement" || nodeType == "while_statement" ||
                     nodeType == "for_statement" || nodeType == "do_while_statement")
            {
                TSNode condNode = ts_node_child_by_field_name(node, "condition", 9);
                if (ts_node_is_null(condNode))
                {
                    uint32_t count = ts_node_named_child_count(node);
                    for (uint32_t i = 0; i < count; ++i)
                    {
                        TSNode child = ts_node_named_child(node, i);
                        std::string_view ct = ts_node_type(child);
                        if (ct != "compound_statement" && ct != "statement_block" && !ct.ends_with("_statement"))
                        {
                            condNode = child;
                            break;
                        }
                    }
                }
                if (!ts_node_is_null(condNode))
                {
                    while (!ts_node_is_null(condNode) &&
                           std::string_view(ts_node_type(condNode)) == "parenthesized_expression" &&
                           ts_node_named_child_count(condNode) > 0)
                    {
                        condNode = ts_node_named_child(condNode, 0);
                    }

                    bool isHandle = false;
                    if (scopeAt())
                    {
                        const std::string name = NodeText(condNode, request.sourceCode);
                        const LocalDefinition *def = ResolveInScope(scopeAt(), name);
                        if (def)
                        {
                            isHandle = def->typeName.find('@') != std::string::npos;
                        }
                    }

                    std::string condType = ResolveExpressionType(condNode, scopeAt(), ctx.request.symbolTable, request.sourceCode, ctx.request.fileUri);
                    if (isHandle || condType.find('@') != std::string::npos)
                    {
                        std::string baseClass = CleanBaseType(condType);
                        auto opSyms = ctx.request.symbolTable.FindSymbolsPtr(baseClass + "::opImplConv");
                        if (opSyms)
                        {
                            for (const auto &sym : *opSyms)
                            {
                                if (sym.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(sym.signature))
                                {
                                    if (CleanBaseType(sym.GetFunction().returnType) == "bool")
                                    {
                                        EmitAtNode(condNode, ctx, "as-err-ref-type-bool-conv-disallowed", baseClass, "bool");
                                        break;
                                    }
                                }
                            }
                        }
                    }
                }
            }
            else if (nodeType == "cast_expression")
            {
                CheckCast(node, scopeAt(), ctx, request.sourceCode);
            }
            else if (nodeType == "return_statement")
            {
                if (ts_node_named_child_count(node) > 0)
                {
                    TSNode expr = ts_node_named_child(node, 0);
                    TSNode parent = ts_node_parent(node);
                    while (!ts_node_is_null(parent))
                    {
                        const std::string_view pType = ts_node_type(parent);
                        if (pType == "lambda_expression" || pType == "anonymous_function")
                        {
                            break;
                        }
                        if (pType == "func_declaration" || pType == "method_declaration" ||
                            pType == "function_definition")
                        {
                            TSNode retTypeNode = ts_node_child_by_field_name(parent, "return_type", 11);
                            if (ts_node_is_null(retTypeNode))
                            {
                                retTypeNode = ts_node_child_by_field_name(parent, "type", 4);
                            }
                            if (!ts_node_is_null(retTypeNode))
                            {
                                TSNode nameNode = ts_node_child_by_field_name(parent, "name", 4);
                                uint32_t headEnd = ts_node_is_null(nameNode) ? ts_node_end_byte(retTypeNode) : ts_node_start_byte(nameNode);
                                uint32_t headStart = ts_node_start_byte(parent);
                                std::string headText = (headEnd > headStart && headEnd <= request.sourceCode.size())
                                    ? std::string(request.sourceCode.substr(headStart, headEnd - headStart))
                                    : "";
                                std::string rawRetText = NodeText(retTypeNode, request.sourceCode);
                                bool isReturnRef = headText.find('&') != std::string::npos || rawRetText.find('&') != std::string::npos;
                                if (isReturnRef)
                                {
                                    std::string exprText = NodeText(expr, request.sourceCode);
                                    while (!exprText.empty() && isspace(static_cast<unsigned char>(exprText.front()))) exprText.erase(exprText.begin());
                                    while (!exprText.empty() && isspace(static_cast<unsigned char>(exprText.back()))) exprText.pop_back();

                                    const Scope *s = scopeAt();
                                    if (s)
                                    {
                                        const LocalDefinition *def = ResolveInScope(s, exprText);
                                        if (def)
                                        {
                                            TSPoint funcStart = ts_node_start_point(parent);
                                            TSPoint funcEnd = ts_node_end_point(parent);
                                            bool isInsideFunction = (def->startLine > funcStart.row && def->startLine < funcEnd.row) ||
                                                                    (def->startLine == funcStart.row && def->startCharacter >= funcStart.column);
                                            if (isInsideFunction)
                                            {
                                                if (def->kind == LocalDefinitionKind::Parameter)
                                                {
                                                    EmitAtNode(expr, ctx, "as-err-cannot-return-param-ref", exprText);
                                                }
                                                else if (def->kind == LocalDefinitionKind::Variable)
                                                {
                                                    EmitAtNode(expr, ctx, "as-err-cannot-return-local-ref", exprText);
                                                }
                                            }
                                        }
                                    }
                                }

                                const std::string expected = CleanBaseType(rawRetText);
                                if (expected == "void")
                                {
                                    EmitAtNode(expr, ctx, "as-err-void-return-value");
                                }
                                else if (!expected.empty())
                                {
                                    const std::string actual = CleanBaseType(ResolveExpressionType(
                                        expr, scopeAt(), ctx.request.symbolTable, request.sourceCode, ctx.request.fileUri));
                                    if (!actual.empty() && actual != expected)
                                    {
                                        if (!IsConvertible(actual, expected, ctx))
                                        {
                                            EmitAtNode(expr, ctx, "as-err-no-implicit-conversion", actual, expected);
                                        }
                                    }
                                }
                            }
                            break;
                        }
                        parent = ts_node_parent(parent);
                    }
                }
            }
            else if (nodeType == node_types::CallExpression || nodeType == "construct_call_expression")
            {
                TSNode callee = ts_node_child_by_field_name(node, "type", 4);
                if (ts_node_is_null(callee))
                {
                    callee = ts_node_child_by_field_name(node, "function", k_functionFieldLength);
                }
                if (ts_node_is_null(callee) && ts_node_child_count(node) > 0)
                {
                    callee = ts_node_child(node, 0);
                }

                const std::string calleeName = CleanBaseType(NodeText(callee, request.sourceCode));
                if (ClassifyNonInstantiable(calleeName, ctx.request.symbolTable) == NonInstantiableKind::Abstract)
                {
                    EmitAtNode(node, ctx, "as-err-abstract-instantiated", calleeName, calleeName);
                }
                else
                {
                    const DeclaredType target = ReadDeclaredType(callee, ctx, request.sourceCode);
                    if (target.usable && !target.isHandle && IsSameType(calleeName, target.baseName))
                    {
                        CheckConstruction(ts_node_child_by_field_name(node, "arguments", k_argsFieldLength),
                                          target.baseName, scopeAt(), ctx, request.sourceCode);
                    }
                }
            }
            else if (nodeType == "func_declaration" || nodeType == "function_definition")
            {
                CheckConstructorDelegation(node, ctx, request.sourceCode);
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
