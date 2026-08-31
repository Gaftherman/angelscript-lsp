#include "analysis/TypeConversionChecker.h"
#include "analysis/ASTUtils.h"
#include "analysis/SemanticHelpers.h"

#include <algorithm>
#include <functional>
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

        /** @brief True when a name resolves to an enum declared somewhere the analyzer can see. */
        bool ResolvesToEnum(const std::string &typeName, const SymbolTable &table)
        {
            bool found = false;
            const std::string bare = LastScopeSegment(typeName);

            for (const auto &candidate : { std::cref(typeName), std::cref(bare) })
            {
                ForEachSymbolNamed(candidate.get(), table, [&found](const Symbol &sym)
                {
                    if (sym.type == SymbolType::Enum)
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
        /** @brief A template type's parameters paired with the arguments written at the use site. */
        struct TemplateBinding
        {
            bool isTemplate = false;
            bool usable = false;      ///< False when the arity does not line up, so nothing may be concluded.
            std::vector<std::string> parameters;
            std::vector<std::string> arguments;
        };

        /**
         * @brief Reads `weakref<Node>` into the substitution `T -> Node`, from the declaration.
         *
         * The parameter names come from the class's own declaration rather than being assumed to be
         * `T`: a host is free to write `class map<K, V>`, and its constructors then mention `K` and
         * `V`.
         */
        TemplateBinding BindTemplateArguments(const std::string &writtenType, const SymbolTable &table)
        {
            TemplateBinding binding;

            const size_t open = writtenType.find('<');
            const std::string name = (open == std::string::npos) ? writtenType : writtenType.substr(0, open);

            const auto declarations = table.FindSymbolsPtr(LastScopeSegment(name));
            if (!declarations)
            {
                return binding;
            }

            for (const auto &declaration : *declarations)
            {
                if (declaration.type != SymbolType::Class ||
                    !std::holds_alternative<ClassSignature>(declaration.signature))
                {
                    continue;
                }
                const auto &cls = std::get<ClassSignature>(declaration.signature);
                if (!cls.isTemplate || cls.templateParams.empty())
                {
                    continue;
                }

                binding.isTemplate = true;
                binding.parameters = cls.templateParams;
                break;
            }

            if (!binding.isTemplate || open == std::string::npos || !writtenType.ends_with('>'))
            {
                return binding;
            }

            const std::string inner = writtenType.substr(open + 1, writtenType.size() - open - 2);
            int depth = 0;
            std::string current;
            for (char c : inner)
            {
                if (c == '<') { ++depth; }
                else if (c == '>') { --depth; }
                else if (c == ',' && depth == 0)
                {
                    binding.arguments.push_back(current);
                    current.clear();
                    continue;
                }
                current += c;
            }
            if (!current.empty())
            {
                binding.arguments.push_back(current);
            }

            for (auto &argument : binding.arguments)
            {
                while (!argument.empty() && isspace(static_cast<unsigned char>(argument.front()))) argument.erase(argument.begin());
                while (!argument.empty() && isspace(static_cast<unsigned char>(argument.back()))) argument.pop_back();
            }

            binding.usable = binding.arguments.size() == binding.parameters.size();
            return binding;
        }

        void ForEachConstructor(const std::string &typeName,
                                const SymbolTable &table,
                                const std::function<bool(const Symbol &)> &visitor)
        {
            // The arguments come off before the key is built. A constructor is stored under the
            // class's own name - `weakref::weakref` - so looking up `weakref<Node>` produced the key
            // `weakref<Node>::weakref<Node>`, which matches nothing: a template's constructors were
            // invisible here, and every `weakref<Node> w(node);` read as having none.
            const size_t open = typeName.find('<');
            const std::string unparameterized =
                (open == std::string::npos) ? typeName : typeName.substr(0, open);

            const std::string bare = LastScopeSegment(unparameterized);
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

            ForEachSymbolNamed(unparameterized + "::" + bare, table, visit);
            if (!stopped && !sawAny && bare != unparameterized)
            {
                ForEachSymbolNamed(bare + "::" + bare, table, visit);
            }

            // A class declared inside a namespace is keyed by its qualified name, so `Hook`'s
            // constructor is `Hooks::Hook::Hook` and neither lookup above reaches it - the call
            // site writes `Hook("OnMapActivate")` because it is inside the namespace, and the two
            // keys built from that spelling match nothing. The class itself was still found,
            // because FindTypeDeclaration already falls back to the last segment; the constructors
            // were not, so every construction of a namespaced class read as having none. Ten
            // corpus findings, all of them a class calling its own constructor.
            //
            // Matched on the last two segments together, so `Hooks::Hook::Hook` qualifies and a
            // stray `Hook` function somewhere else does not.
            if (!stopped && !sawAny)
            {
                const std::string suffix = "::" + bare + "::" + bare;
                table.ForEachSymbol([&](const std::string &qName, const std::vector<Symbol> &symbols)
                {
                    if (stopped || !qName.ends_with(suffix))
                    {
                        return;
                    }
                    for (const auto &sym : symbols)
                    {
                        if (visit(sym))
                        {
                            return;
                        }
                    }
                });
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
        /** @brief The workspace's string type, which defaults to `string` when unconfigured. */
        bool IsStringType(const std::string &typeName, const DiagnosticContext &ctx)
        {
            const auto strType = ctx.request.GetStringTypeName();
            return typeName == (strType.empty() ? std::string_view("string") : strType);
        }

        bool IsBuiltInValueType(const std::string &typeName, const DiagnosticContext &ctx)
        {
            return IsPrimitiveTypeName(typeName) || IsStringType(typeName, ctx);
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

            // `?` is AngelScript's variable type, not a type name: a parameter declared `const ?&in`
            // or `?&out` takes a value of any type at all. dictionary::set/get, ref, Dispose and the
            // format/scan helpers are all declared that way, so without this the analyzer reported
            // "Cannot implicitly convert 'int' to '?'" on code the real compiler accepts.
            if (IsVariableType(to) || IsVariableType(from))
            {
                return true;
            }

            // `auto` is not a type either - it is a placeholder for whatever the initializer
            // produces, and the deduction happens in the compiler. Judging a conversion against it
            // asks a question with no answer: the real target is the source's own type, so every
            // `auto` conversion is trivially fine and reporting one is always wrong.
            // tests/parity/doc_p22_auto_handle.as.
            if (from == "auto" || to == "auto")
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
                const auto isNumeric = [](const std::string &t) { return IsNumericPrimitive(t); };
                const bool fromNum = isNumeric(from);
                const bool toNum = isNumeric(to);
                if (fromNum && toNum)
                {
                    return true;
                }
                // `bool` is deliberately absent from both directions. It is not a number and
                // converts to none of them: all forty combinations of {bool -> T, T -> bool} x
                // {argument, initializer} over the ten numeric types are rejected by the compiler,
                // and so are `int(b)`, `bool(n)`, `b + 1`, `return b` from an int function, and
                // `if (n)`. It used to answer true here and in OverloadResolver, where the cost
                // was 75 spurious ambiguities over the corpus. `string s = b;` stays legal through
                // the string sink below, which is a real opAssign the add-on registers.
                if ((from == "bool" && toNum) || (fromNum && to == "bool"))
                {
                    return false;
                }

                // `string` is a sink. The standard string add-on registers an opAssign for every
                // scalar, so each of these compiles - measured one type at a time against the
                // oracle, in tests/parity/doc_p23_string_is_a_sink.as:
                //
                //     string s = i8;  … = u64;  … = f;  … = d;  … = b;   all accepted
                //
                // and the `"" + x` concatenation everyone writes asks the same question. Between
                // them this was 149 of the 273 findings the corpus audit reported, every one of
                // them legal code.
                //
                // Only into it. Nothing leaves a string implicitly - `int i = s;` is
                // "Can't implicitly convert from 'string' to 'int'" - which is doc_r25, and is why
                // this tests the target rather than treating the pair as interchangeable.
                if (IsStringType(to, ctx))
                {
                    return true;
                }

                return false;
            }

            const TypeDeclarationInfo fromDecl = FindTypeDeclaration(from, table);
            const TypeDeclarationInfo toDecl = FindTypeDeclaration(to, table);

            // Nothing reaches an enum implicitly but that same enum. The compiler's answers, from
            // tests/parity/doc_r09_int_to_enum.as and its neighbours:
            //
            //     Color c = 1;        Can't implicitly convert from 'int' to 'Color'.
            //     Color c = someUint; Can't implicitly convert from 'uint' to 'Color'.
            //     A a = B1;           Can't implicitly convert from 'B' to 'A'.
            //     int i = Color::Red; accepted - widening the other way is fine
            //
            // so an enum is a sink and only the `to` side is restricted. OverloadResolver has
            // always agreed with the compiler here, which is why a *call* was reported and an
            // assignment was not; this closes that. It has to run before the unresolved-name bail
            // below, because an enum never appears in a TypeDeclarationInfo - that only records
            // classes and interfaces - so `to` would look unresolved and the check would be skipped.
            if (ResolvesToEnum(to, table))
            {
                // Decidable only when the source is something this analyzer can see. An unresolved
                // name is an engine-registered type, and engine types carry conversions declared
                // nowhere in the source.
                if (!fromBuiltIn && !fromDecl.found && !ResolvesToEnum(from, table))
                {
                    return true;
                }
                // A class may declare an operator producing the enum, and the compiler accepts it:
                // `class W { Color opImplConv() const { … } } … Color c = w;` compiles.
                if (!fromBuiltIn && DeclaresConversionTo(from, to, table, /*implicitOnly=*/false))
                {
                    return true;
                }
                return false;
            }

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
                                        std::string_view sourceCode,
                                        int depth = 0);

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
                                        std::string_view sourceCode,
                                        int depth)
        {
            // See k_maxAstDepth in ASTUtils.h. Returning the empty type is this file's established
            // "cannot see enough to judge" answer, which every caller already treats as silence.
            if (depth > k_maxAstDepth)
            {
                return ExpressionType{};
            }

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
                           ? ResolveValueType(ts_node_named_child(node, 0), scope, ctx, sourceCode, depth + 1)
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
                    ts_node_child_by_field_name(node, "operand", k_operandFieldLength), scope, ctx, sourceCode, depth + 1);
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

        void EmitWarningAtNode(TSNode node,
                               DiagnosticContext &ctx,
                               std::string_view code,
                               const std::string &from = "",
                               const std::string &to = "")
        {
            const TSPoint start = ts_node_start_point(node);
            const TSPoint end = ts_node_end_point(node);
            ctx.EmitAtRange(start.row, start.column, end.row, end.column, code, from, to,
                            DiagnosticSeverity::Warning);
        }

        // --- Numeric conversion warnings (TYPE-03) -------------------------------------------
        //
        // The compiler emits five numeric warnings. Two of them - these - it decides from the
        // operand types alone. The other three ("Implicit conversion changed sign of value",
        // "Value is too large for data type", "Implicit conversion of value is not exact") fire
        // only on constant expressions, so answering them needs a constant folder this analyzer
        // does not have. They are left unimplemented rather than approximated: a warning that is
        // right about the shape and wrong about the value is worse than no warning.
        //
        // Everything below was measured against angelscript_oracle, not read off documentation,
        // and the measurements corrected the backlog on three points:
        //
        //   * Signed/Unsigned mismatch fires ONLY on the six comparison operators. `i * u`,
        //     `i & u` and every other arithmetic or bitwise pairing is silent, and so are
        //     assignment, argument passing and return. The backlog implied it followed the
        //     conversion, which it does not.
        //   * Width is irrelevant - every signed integer paired with every unsigned one warns -
        //     and float and double count as SIGNED: `float < uint` warns, `float < int` does not.
        //   * A compile-time constant on either side folds the comparison away and it is silent.
        //     `const int i = 1; i < u` is clean, a bare enum member is clean, and `i < 5` is
        //     clean. Only a constant whose value is itself out of range warns, and then under one
        //     of the three codes above.
        //
        // The last point is the whole false-positive risk, so the rule stays silent whenever
        // either operand is constant. That costs `u < -5`, which the compiler does warn about;
        // missing beats inventing, and the alternative is the folder again.
        //
        // Neither warning reuses IsPrimitiveWidening from OverloadResolver, and that is
        // deliberate: it lists signed/unsigned pairs as SAFE on purpose, because removing them
        // produced real false positives on `array<int> a(1)`. The compiler warns on exactly the
        // pairs that table calls safe, so the two questions need two tables.

        bool IsUnsignedIntegerPrimitive(std::string_view typeName) noexcept
        {
            return typeName == "uint" || typeName == "uint8" || typeName == "uint16" ||
                   typeName == "uint32" || typeName == "uint64";
        }

        /** @brief Signed for the purpose of the mismatch warning, which counts float and double. */
        bool IsSignedNumericPrimitive(std::string_view typeName) noexcept
        {
            return typeName == "int" || typeName == "int8" || typeName == "int16" ||
                   typeName == "int32" || typeName == "int64" ||
                   typeName == "float" || typeName == "double";
        }

        bool IsComparisonOperator(std::string_view op) noexcept
        {
            return op == "<" || op == ">" || op == "<=" || op == ">=" || op == "==" || op == "!=";
        }

        /** @brief True for a written-out number, through any parentheses and unary sign. */
        bool IsNumericLiteralExpression(TSNode node, int depth = 0)
        {
            if (ts_node_is_null(node) || depth > k_maxAstDepth)
            {
                return false;
            }

            const std::string_view nodeType = NodeType(node);
            if (nodeType.ends_with("_literal") || nodeType == "number")
            {
                return true;
            }
            if (nodeType != "parenthesized_expression" && nodeType != "unary_expression")
            {
                return false;
            }
            for (uint32_t i = 0; i < ts_node_named_child_count(node); ++i)
            {
                if (IsNumericLiteralExpression(ts_node_named_child(node, i), depth + 1))
                {
                    return true;
                }
            }
            return false;
        }

        /**
         * @brief True when the operand is something the compiler folds before it compares.
         *
         * Constants are what lets the mismatch rule stay silent instead of guessing: the compiler
         * knows the value, so it compares values rather than types and no mismatch arises. A name
         * counts as constant here when its declaration says `const` - the one spelling that
         * survives into both LocalDefinition::typeName and VariableSignature::typeName - or when
         * it is a literal.
         *
         * An enum member needs no case of its own. ResolveExpressionType answers a bare `A` with
         * its enum's name rather than `int`, so it never reaches the numeric test at all.
         *
         * An unrecognised shape answers false, which is the emitting side. That is what the
         * corpus audit in TypeConversionTest.cpp exists to hold honest.
         */
        bool IsFoldedConstantOperand(TSNode node,
                                     const Scope *scope,
                                     const DiagnosticContext &ctx,
                                     std::string_view sourceCode,
                                     int depth = 0)
        {
            if (ts_node_is_null(node) || depth > k_maxAstDepth)
            {
                return false;
            }

            const std::string_view nodeType = NodeType(node);
            if (IsNumericLiteralExpression(node))
            {
                return true;
            }

            if (nodeType == "parenthesized_expression")
            {
                for (uint32_t i = 0; i < ts_node_named_child_count(node); ++i)
                {
                    if (IsFoldedConstantOperand(ts_node_named_child(node, i), scope, ctx, sourceCode, depth + 1))
                    {
                        return true;
                    }
                }
                return false;
            }

            if (nodeType != "identifier" && nodeType != "scoped_identifier" &&
                nodeType != "qualified_identifier")
            {
                return false;
            }

            const std::string name = NodeText(node, sourceCode);
            if (scope)
            {
                if (const LocalDefinition *def = ResolveInScope(scope, LastScopeSegment(name)))
                {
                    return def->typeName.starts_with("const ");
                }
            }

            bool folded = false;
            ForEachSymbolNamed(name, ctx.request.symbolTable, [&folded](const Symbol &sym)
            {
                if (sym.type != SymbolType::Variable && sym.type != SymbolType::Property)
                {
                    return false;
                }
                folded = sym.GetVariable().typeName.starts_with("const ");
                return true;
            });
            return folded;
        }

        /**
         * @brief `as-warn-signed-unsigned-mismatch`, anchored on the operator the way the compiler
         *        anchors it.
         */
        void CheckSignedUnsignedComparison(TSNode node,
                                           const Scope *scope,
                                           DiagnosticContext &ctx,
                                           std::string_view sourceCode)
        {
            TSNode opNode = ts_node_child_by_field_name(node, "operator", 8);
            if (ts_node_is_null(opNode) || !IsComparisonOperator(NodeText(opNode, sourceCode)))
            {
                return;
            }

            TSNode left = ts_node_child_by_field_name(node, "left", 4);
            TSNode right = ts_node_child_by_field_name(node, "right", 5);
            if (ts_node_is_null(left) || ts_node_is_null(right))
            {
                return;
            }

            const std::string leftType = CleanBaseType(
                ResolveExpressionType(left, scope, ctx.request.symbolTable, sourceCode, ctx.request.fileUri));
            const std::string rightType = CleanBaseType(
                ResolveExpressionType(right, scope, ctx.request.symbolTable, sourceCode, ctx.request.fileUri));

            const bool mismatched =
                (IsUnsignedIntegerPrimitive(leftType) && IsSignedNumericPrimitive(rightType)) ||
                (IsUnsignedIntegerPrimitive(rightType) && IsSignedNumericPrimitive(leftType));
            if (!mismatched)
            {
                return;
            }

            if (IsFoldedConstantOperand(left, scope, ctx, sourceCode) ||
                IsFoldedConstantOperand(right, scope, ctx, sourceCode))
            {
                return;
            }

            EmitWarningAtNode(opNode, ctx, "as-warn-signed-unsigned-mismatch", leftType, rightType);
        }

        /**
         * @brief `as-warn-float-truncation` where a float value implicitly becomes an integer.
         *
         * Anchored at the source expression, which is where the compiler anchors it.
         *
         * A CONSTANT source is excluded, for the same reason the mismatch rule excludes one: the
         * compiler folds it and then judges the value, not the type. `const float D = 15.0;
         * int i = D;` is clean because 15.0 survives the trip exactly, and `const float D = 15.5`
         * is "Implicit conversion of value is not exact" - a different code, and one that needs
         * the constant folder. A written literal is the same story: `int i = 2.0f;` is clean and
         * `int i = 1.5f;` is not.
         *
         * This is not a hypothetical. The corpus audit's first run reported 34 findings of the
         * shape `const float WEAPON_DAMAGE = 15.0; int m_iBulletDamage = WEAPON_DAMAGE;` across
         * the Sven Co-op weapon scripts, and the compiler is silent on every one of them.
         */
        void CheckFloatTruncation(TSNode valueNode,
                                  const std::string &sourceType,
                                  const std::string &targetType,
                                  const Scope *scope,
                                  DiagnosticContext &ctx,
                                  std::string_view sourceCode)
        {
            if (ts_node_is_null(valueNode))
            {
                return;
            }
            if (!IsFloatingPointPrimitive(sourceType) || !IsIntegerPrimitive(targetType))
            {
                return;
            }
            if (IsFoldedConstantOperand(valueNode, scope, ctx, sourceCode))
            {
                return;
            }
            EmitWarningAtNode(valueNode, ctx, "as-warn-float-truncation", sourceType, targetType);
        }

        /** @brief Everything one declared type text says that the rules below need to know. */
        struct DeclaredType
        {
            std::string baseName;
            bool isHandle = false;
            bool usable = false;  ///< False when the shape is out of scope (array, template, unknown).

            /**
             * @brief True for `array<T>` and `T[]`, where baseName is the *element* type.
             *
             * CleanBaseType answers the element type, which is what the initializer comparisons
             * below want - `array<int> a = other;` compares element to element. A construction
             * argument is a different question: `array<PlayerSlide> g(33);` passes 33 to the
             * container's initial-size constructor, and comparing it against `PlayerSlide` asked
             * whether an int can become a PlayerSlide. It cannot, so three corpus declarations of
             * exactly this shape were reported.
             */
            bool isTemplateOrArray = false;
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

            // The written SHAPE is decided before anything about the base name, and the order
            // matters. CleanBaseType reduces `bool[]` to `bool`, which is a built-in value type,
            // so the test below used to claim the declaration first and return with
            // isTemplateOrArray false - and then `bool[] flags(33);` was read as constructing a
            // `bool` from 33 rather than sizing an array. It stayed silent only by accident:
            // `int[] a(33)` and `float[] a(33)` are the same mistake, and `int -> int` and
            // `int -> float` are convertible, so nothing was reported. Correcting `bool` to be
            // unconvertible from the numeric types is what made the accident visible, on
            // `bool[] g_playerGlowEnable(32+1);` in the corpus.
            if (raw.find('<') != std::string::npos || raw.find('[') != std::string::npos)
            {
                result.usable = true;
                result.isTemplateOrArray = true;
                return result;
            }

            if (IsBuiltInValueType(result.baseName, ctx))
            {
                result.usable = true;
                return result;
            }

            // An enum is not a class, but it is a type this pass can reason about completely: the
            // members are all declared in the source, nothing converts to it implicitly but itself,
            // and IsConvertible says so. Left out, `Color c = 1;` was silent while the identical
            // mistake in a call - `SetMode(1)` - was reported, because OverloadResolver had always
            // agreed with the compiler and this had not. See tests/parity/doc_r09 and doc_p15.
            if (ResolvesToEnum(result.baseName, ctx.request.symbolTable))
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

            if (!declared.isHandle)
            {
                CheckFloatTruncation(valueNode, source.baseName, declared.baseName, scope, ctx, sourceCode);
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

        /**
         * @brief The type `foreach`'s Nth loop variable takes, from the container's own declaration.
         *
         * AngelScript drives `foreach` through `opForBegin` / `opForEnd` / `opForNext` and one
         * `opForValue<N>` per loop variable, so the Nth variable's type is that method's return
         * type. Reading it back out of the stub is the general rule and needs no per-type knowledge:
         * `array<T>` declares `const T& opForValue0(uint)` and `uint opForValue1(uint)`, and
         * `dictionary` declares `const dictionaryValue& opForValue0(...)` and
         * `const string& opForValue1(...)`, which is exactly what the compiler hands the loop.
         *
         * Returns empty when the container declares no such method, which leaves the variable's
         * written `auto` in place rather than guessing at it.
         */
        std::string ForeachValueType(const std::string &containerType,
                                     uint32_t variableIndex,
                                     const DiagnosticContext &ctx)
        {
            const std::string cleaned = CleanExpressionType(containerType);
            if (cleaned.empty())
            {
                return {};
            }

            const size_t open = cleaned.find('<');
            const std::string bare =
                LastScopeSegment((open == std::string::npos) ? cleaned : cleaned.substr(0, open));

            const TemplateBinding binding = BindTemplateArguments(cleaned, ctx.request.symbolTable);
            const std::string method = "opForValue" + std::to_string(variableIndex);

            for (const std::string &candidate : GetInheritedTypeHierarchy(bare, ctx.request.symbolTable))
            {
                const auto overloads = ctx.request.symbolTable.FindSymbolsPtr(candidate + "::" + method);
                if (!overloads)
                {
                    continue;
                }
                for (const Symbol &overload : *overloads)
                {
                    if (overload.type != SymbolType::Function ||
                        !std::holds_alternative<FunctionSignature>(overload.signature))
                    {
                        continue;
                    }

                    std::string returnType = std::get<FunctionSignature>(overload.signature).returnType;
                    if (returnType.empty() || returnType == "void")
                    {
                        continue;
                    }
                    if (binding.usable)
                    {
                        for (size_t i = 0; i < binding.parameters.size(); ++i)
                        {
                            returnType = SubstituteTypeParam(returnType, binding.parameters[i],
                                                             binding.arguments[i]);
                        }
                    }
                    // `const T&` names the same type as `T` for anything the scope tree does with
                    // it, and the reference is not part of the variable's identity.
                    return CleanExpressionType(returnType);
                }
            }

            return {};
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

            // The verdict below is "no constructor accepts this", and it is only worth anything
            // when the constructors are visible. Two kinds of target they are not:
            //
            //   - A built-in value type. `string(u)`, `float(i)` - the engine registers these in
            //     C++ and no stub can express them. CheckDefaultConstructor already bails here for
            //     the same reason; this one did not, so every `string(count)` in the corpus was
            //     reported.
            //   - A name with no declaration in the workspace. `EHandle(x)`, `Vector(x)`,
            //     `array<float>(33)` - the host registers them, ForEachConstructor finds nothing,
            //     and "I found no constructor" is indistinguishable from "they are written in C++".
            //
            // An enum is neither and stays judged: `Color(1)` has no constructor by design and the
            // compiler's rule for it is known exactly, which the block further down applies.
            //
            // A visible class that declares no constructor is still reported - `class Plain {}`
            // with `Plain(1)` is an error the compiler agrees with, and that is what separates the
            // two cases. Together these were the bulk of the 273 corpus findings.
            if (IsBuiltInValueType(targetType, ctx))
            {
                return;
            }
            if (!FindTypeDeclaration(targetType, table).found && !ResolvesToEnum(targetType, table))
            {
                return;
            }

            if (AreHierarchyRelated(source.baseName, targetType, table))
            {
                return;
            }
            if (DeclaresConversionTo(source.baseName, targetType, table, /*implicitOnly=*/false))
            {
                return;
            }

            // A template's constructors are written in terms of its parameters - `weakref<T>` takes
            // a `T@` - so the argument has to be substituted in before the types can be compared.
            // Without it every `weakref<Node> w(node);` was reported as "No conversion from 'Node'
            // to 'weakref<Node>'", because `T` resolves to nothing and nothing is convertible to
            // it. Anything that cannot be substituted cleanly leaves the check inconclusive.
            const TemplateBinding binding = BindTemplateArguments(targetType, table);
            if (binding.isTemplate && !binding.usable)
            {
                return;
            }

            // `Color(1)` is the explicit conversion an enum offers, and it has no constructor
            // declaring it - the compiler accepts every numeric source (`Color(1.0f)` too) and
            // rejects the rest ("Can't implicitly convert from 'string' to 'const Color'"). Without
            // this the ForEachConstructor walk below found nothing and reported the one form that
            // exists for turning an int into an enum.
            if (ResolvesToEnum(targetType, table))
            {
                if (IsNumericPrimitive(source.baseName) || ResolvesToEnum(source.baseName, table))
                {
                    return;
                }
                EmitAtNode(argument, ctx, "as-err-no-explicit-conversion", source.baseName, targetType);
                return;
            }

            bool constructible = false;
            ForEachConstructor(targetType, table, [&](const Symbol &sym)
            {
                const auto &parameters = sym.GetFunction().parameters;
                if (!AcceptsSingleArgument(parameters))
                {
                    return false;
                }

                std::string parameterType = SingleArgumentType(parameters);
                for (size_t i = 0; i < binding.parameters.size(); ++i)
                {
                    parameterType = SubstituteTypeParam(parameterType, binding.parameters[i],
                                                        binding.arguments[i]);
                }

                if (IsConvertible(source.baseName, parameterType, ctx, 1))
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

        // --- Lambda against its target funcdef ------------------------------------------------
        //
        // A lambda takes its parameter types from the funcdef it is assigned to, so it may leave
        // them out. What it writes, though, has to match exactly - the compiler compares the
        // written signature, it does not convert it. Measured against angelscript_oracle, and the
        // exactness is the surprising half:
        //
        //     funcdef void CB(const string &in);
        //     CB@ cb = function(s) { };                    // accepted, type comes from CB
        //     CB@ cb = function(const string &in s) { };    // accepted, written and identical
        //     CB@ cb = function(string s) { };              // REJECTED - no const, no &in
        //     CB@ cb = function(string &in s) { };          // REJECTED - no const
        //     funcdef void CB(int);
        //     CB@ cb = function(uint a) { };                // REJECTED - int does not widen here
        //
        // ARITY is a hard equality even when every parameter is untyped, and a funcdef's default
        // argument does not relax it: `funcdef void CB(int a = 1)` still rejects `function()`.
        // That is the whole of what this rule can check without risking a false positive, and it
        // is also the mistake real code actually makes.
        //
        // The TYPE NAME is another matter, because the compiler resolves it and this rule only
        // reads it. All of these are ACCEPTED, and a string comparison would report every one:
        //
        //     typedef float real;  funcdef void CB(real);   function(float a)   // typedef
        //     funcdef void CB(array<int>@);                 function(int[]@ a)  // two spellings
        //     namespace N { class Foo{} funcdef void CB(Foo@); }
        //                                                   function(N::Foo@ f) // qualification
        //
        // So the name is compared by its last `::` segment, and not at all when either side names
        // a typedef - the one alias no spelling comparison can see through. `array<int>` and
        // `int[]` need no case of their own: CleanBaseType reduces both to the element type.
        //
        // The DECORATIONS - `const`, `&`, the in/out/inout modifier and `@` - are compared
        // whatever the type name is, because none of them can be hidden by a spelling: AngelScript
        // typedefs alias primitives only (see as-err-typedef-non-primitive), and a namespace
        // qualifies a name without changing whether it is a handle.

        void CheckFuncdefAssignment(TSNode targetNode,
                                    const FuncdefSignature &funcdefSig,
                                    TSNode valueNode,
                                    const Scope * /*scope*/,
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

            const std::string_view valueType = NodeType(actualVal);
            if (valueType == node_types::LambdaExpression || valueType == "anonymous_function")
            {
                // A lambda has no symbol to look up, so the name search below would find nothing
                // and return in silence - which is what it did before this branch existed.
                TSNode listNode = ts_node_child_by_field_name(actualVal, "parameters", 10);
                if (ts_node_is_null(listNode))
                {
                    return;
                }
                if (LambdaContradictsFuncdef(ReadLambdaParameters(listNode, sourceCode), funcdefSig,
                                             ctx.request.symbolTable))
                {
                    EmitAtNode(targetNode, ctx, "as-err-signature-mismatch-func-handle");
                }
                return;
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

        void VisitNode(TSNode node, const TypeConversionCheckRequest &request, DiagnosticContext &ctx, int depth = 0)
                {
            // Pathologically nested source would otherwise recurse until the stack gives out; see
            // k_maxAstDepth in ASTUtils.h.
            if (depth > k_maxAstDepth)
                return;

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

            // `foreach (auto value : container)` writes `auto` and nothing else, so without this
            // the loop variable reached every consumer typeless: no hover, no completion after
            // `value.`, nothing for the expression resolver behind the access and const passes.
            // Same guard and same write-back as the `auto` inference below - see the comment there
            // for why mutableScopeRoot is what makes touching the scope tree sound.
            if (nodeType == "foreach_statement")
            {
                TSNode collection = ts_node_child_by_field_name(node, "collection", 10);
                if (!ts_node_is_null(collection))
                {
                    const std::string containerType = ResolveExpressionType(
                        collection, scopeAt(), ctx.request.symbolTable, request.sourceCode, ctx.request.fileUri);

                    // A primitive can never be iterated: `foreach` needs opForBegin/opForEnd/
                    // opForNext/opForValue, and nothing can register those on `int`. The real
                    // compiler answers this with "Type 'int' is not valid type for foreach loops".
                    //
                    // Only primitives are reported, for the usual reason - a class that declares no
                    // opFor* here may well have them registered in C++ where no stub records it, and
                    // reporting that would be a false positive on working code.
                    const std::string containerBase = CleanExpressionType(containerType);
                    if (IsCorePrimitive(containerBase) && containerBase != "auto" && containerBase != "void")
                    {
                        EmitAtNode(collection, ctx, "as-err-invalid-foreach-container", containerBase);
                    }

                    // The write-back below is the only part that needs an unpublished tree; the
                    // diagnostic above is a plain reading of the source and is owed to the user
                    // whether or not this caller owns the scope tree exclusively.
                    if (!containerType.empty() && request.mutableScopeRoot)
                    {
                        uint32_t variableIndex = 0;
                        const uint32_t childCount = ts_node_named_child_count(node);
                        for (uint32_t i = 0; i < childCount; ++i)
                        {
                            TSNode child = ts_node_named_child(node, i);
                            if (NodeType(child) != "foreach_variable")
                            {
                                continue;
                            }

                            TSNode nameNode = ts_node_child_by_field_name(child, "name", k_nameFieldLength);
                            const std::string valueType = ForeachValueType(containerType, variableIndex, ctx);
                            ++variableIndex;

                            if (ts_node_is_null(nameNode) || valueType.empty())
                            {
                                continue;
                            }

                            // The body's scope is where the variable lives, so it is resolved from
                            // the name's own position rather than the statement's.
                            const TSPoint namePoint = ts_node_start_point(nameNode);
                            const Scope *bodyScope = FindEnclosingScope(request.scopeRoot, namePoint.row, namePoint.column);
                            const LocalDefinition *def =
                                ResolveInScope(bodyScope ? bodyScope : scopeAt(),
                                               NodeText(nameNode, request.sourceCode));

                            if (def && (def->typeName == "auto" || def->typeName == "auto@"))
                            {
                                const_cast<LocalDefinition *>(def)->typeName = valueType;
                            }
                        }
                    }
                }
            }

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
                        // Writing the deduced type back is what lets hover, completion and the
                        // other checkers see `auto` as its concrete type. It is guarded on
                        // mutableScopeRoot because it is only sound on a tree the caller has not
                        // published yet: doing it unconditionally meant the analysis thread wrote
                        // this std::string while the message loop read it for a hover.
                        else if (!rhsType.empty() && request.mutableScopeRoot && scopeAt())
                        {
                            const LocalDefinition *def = ResolveInScope(scopeAt(), varName);
                            if (def && (def->typeName == "auto" || def->typeName == "auto@"))
                            {
                                // Sound only under the guard above: mutableScopeRoot is the caller
                                // asserting it owns this exact tree exclusively, so the constness
                                // here is incidental rather than a shared-state guarantee.
                                const_cast<LocalDefinition *>(def)->typeName = rhsType;
                            }
                        }
                    }
                }
                else
                {
                    const std::string fullType = NodeText(typeNode, request.sourceCode);
                    const std::string baseType = CleanBaseType(fullType);
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
                                    else if (!ts_node_is_null(argsNode) && !declared.isTemplateOrArray)
                                    {
                                        // Skipped for a template: the argument belongs to the
                                        // container's constructor and declared.baseName is the
                                        // element type, so this would ask whether 33 can become a
                                        // PlayerSlide. The container's own constructors are the
                                        // engine's - see the visibility guard in CheckConstruction.
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

                    // Covers `i = f;` and `i += f;` alike - the compiler warns on both, and a
                    // compound assignment reaches here with the same left and right types.
                    CheckFloatTruncation(right, cleanRight, cleanLeft, scopeAt(), ctx, request.sourceCode);

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
            else if (nodeType == "binary_expression")
            {
                CheckSignedUnsignedComparison(node, scopeAt(), ctx, request.sourceCode);
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
                // Unary minus on an unsigned operand used to be reported here as
                // as-err-unary-neg-on-unsigned. It is not an error: AngelScript permits it and the
                // result wraps, exactly as it does in C and C++. Verified against the real compiler
                // for uint8, uint16, uint, uint64 and unsigned sub-expressions - every one compiles
                // clean (see ParityAuditTest). The rule fired on ordinary correct code such as
                // `-someUint`, so it was removed rather than narrowed; there is no operand type for
                // which the diagnostic would have been right.
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
                            // A lambda writes no return type; the funcdef it is handed to supplies
                            // one. Measured: `funcdef void CB(); CB@ cb = function() { return 1; };`
                            // is "Can't return value when return type is 'void'", and
                            // `funcdef int CB(); ... { return 'x'; }` is "No conversion from
                            // 'const string' to 'int'". Both are the same two checks the named-
                            // function branch below makes, against a return type read from
                            // elsewhere - so this stops at the lambda either way, having judged
                            // what it could.
                            const auto target = FuncdefTargetOfLambda(parent, ctx.request.symbolTable,
                                                                      request.sourceCode);
                            if (target)
                            {
                                const std::string expected = CleanBaseType(target->GetFuncdef().returnType);
                                if (expected == "void")
                                {
                                    EmitAtNode(expr, ctx, "as-err-void-return-value");
                                }
                                else if (!expected.empty())
                                {
                                    const std::string actual = CleanBaseType(ResolveExpressionType(
                                        expr, scopeAt(), ctx.request.symbolTable, request.sourceCode,
                                        ctx.request.fileUri));
                                    CheckFloatTruncation(expr, actual, expected, scopeAt(), ctx,
                                                         request.sourceCode);
                                    if (!actual.empty() && actual != expected &&
                                        !IsConvertible(actual, expected, ctx))
                                    {
                                        EmitAtNode(expr, ctx, "as-err-no-implicit-conversion", actual, expected);
                                    }
                                }
                            }
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
                                    CheckFloatTruncation(expr, actual, expected, scopeAt(), ctx, request.sourceCode);
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

                // `MapChangeHook( function(...) )` - a funcdef used as a conversion. This is how
                // real code writes a callback: across the whole 1,061-file corpus there is not one
                // `CB@ cb = function(...)`, and every lambda that reaches a funcdef reaches it
                // through this shape or as a call argument. The compiler answers a wrong lambda
                // here with "No matching signatures to 'CB(<auto> lambda())'", by the same rules as
                // the assignment - so it is the same call.
                //
                // Guarded on the argument actually being a lambda, because FindFuncdef falls back
                // to a last-segment scan across the whole table: without the guard, a class that
                // shares its bare name with some namespace's funcdef would take this branch and
                // lose its CheckConstruction below.
                TSNode argsNode = ts_node_child_by_field_name(node, "arguments", k_argsFieldLength);
                TSNode soleArgument = {};
                if (!ts_node_is_null(argsNode) && ts_node_named_child_count(argsNode) == 1)
                {
                    soleArgument = ts_node_named_child(argsNode, 0);
                }
                const std::string_view soleArgumentType = NodeType(soleArgument);
                const bool soleArgumentIsLambda =
                    soleArgumentType == node_types::LambdaExpression ||
                    soleArgumentType == "anonymous_function";

                std::optional<Symbol> calleeFuncdef;
                if (soleArgumentIsLambda)
                {
                    calleeFuncdef = FindFuncdef(calleeName, ctx.request.symbolTable);
                }

                if (ClassifyNonInstantiable(calleeName, ctx.request.symbolTable) == NonInstantiableKind::Abstract)
                {
                    EmitAtNode(node, ctx, "as-err-abstract-instantiated", calleeName, calleeName);
                }
                else if (calleeFuncdef)
                {
                    CheckFuncdefAssignment(node, calleeFuncdef->GetFuncdef(), soleArgument,
                                           scopeAt(), ctx, request.sourceCode);
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
                VisitNode(ts_node_named_child(node, i), request, ctx, depth + 1);
            }
        }
    }

    bool CanConvertImplicitly(const std::string &fromType, const std::string &toType,
                              const DiagnosticContext &ctx)
    {
        return IsConvertible(CleanBaseType(fromType), CleanBaseType(toType), ctx);
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
