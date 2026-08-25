#include "analysis/CallChecker.h"
#include "analysis/OverloadResolver.h"
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
        constexpr uint32_t k_functionFieldLength = 8;  ///< "function"
        constexpr uint32_t k_argumentsFieldLength = 9; ///< "arguments"
        constexpr uint32_t k_objectFieldLength = 6;    ///< "object"
        constexpr uint32_t k_memberFieldLength = 6;    ///< "member"

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

        /** @brief Strips a namespace or class qualification, leaving the last segment. */
        std::string LastScopeSegment(const std::string &name)
        {
            const size_t pos = name.rfind("::");
            return pos == std::string::npos ? name : name.substr(pos + 2);
        }

        /**
         * @brief Counts the arguments written between one call's parentheses.
         *
         * Counted from the separators rather than from the named children, because a named argument
         * (`Take(a: 1)`) contributes both its name and its value as named children and would count
         * twice. `Take(void)` is one argument too - AngelScript's spelling of "discard this &out" -
         * and it is an anonymous token, so an empty list is only an empty one when nothing at all
         * stands between the parentheses.
         */
        uint32_t CountArguments(TSNode argumentList)
        {
            if (ts_node_is_null(argumentList))
            {
                return 0;
            }

            uint32_t commas = 0;
            bool sawArgument = false;

            const uint32_t count = ts_node_child_count(argumentList);
            for (uint32_t i = 0; i < count; ++i)
            {
                const std::string_view childType = ts_node_type(ts_node_child(argumentList, i));
                if (childType == ",")
                {
                    ++commas;
                }
                else if (childType != "(" && childType != ")" && childType != "comment")
                {
                    sawArgument = true;
                }
            }

            return sawArgument ? commas + 1 : 0;
        }

        std::vector<TSNode> GetArgumentNodes(TSNode argumentList)
        {
            std::vector<TSNode> argNodes;
            if (ts_node_is_null(argumentList))
            {
                return argNodes;
            }

            const uint32_t count = ts_node_child_count(argumentList);
            for (uint32_t i = 0; i < count; ++i)
            {
                TSNode child = ts_node_child(argumentList, i);
                const std::string_view childType = ts_node_type(child);
                if (childType != "(" && childType != ")" && childType != "," && childType != "comment")
                {
                    argNodes.push_back(child);
                }
            }
            return argNodes;
        }

        /** @brief What one declaration will accept, in argument counts. */
        struct Arity
        {
            uint32_t required = 0;
            uint32_t maximum = 0;
            bool variadic = false;
        };

        Arity ArityOf(const FunctionSignature &sig)
        {
            Arity arity;
            for (const auto &param : sig.parameters)
            {
                // `...` reaches the collector as a parameter with nothing in it but its own text.
                if (param.rawText.find("...") != std::string::npos)
                {
                    arity.variadic = true;
                    continue;
                }

                ++arity.maximum;
                if (param.defaultValue.empty())
                {
                    ++arity.required;
                }
            }
            return arity;
        }

        /**
         * @brief True when a name also denotes a type, which makes `Name(...)` a construction.
         *
         * `array<int>(5)`, `Foo(1)` and - the one the corpus audit found - `VoteBlocked(this.X)`,
         * where VoteBlocked is a funcdef and the parentheses build a delegate from a method. None
         * of those is a call to a function of that name, and the enclosing class happening to
         * declare a method called VoteBlocked is a coincidence this rule must not read anything
         * into.
         */
        bool NamesAType(const std::string &name, const SymbolTable &table)
        {
            const auto symbols = table.FindSymbolsPtr(name);
            return symbols && std::any_of(symbols->begin(), symbols->end(), [](const Symbol &sym)
            {
                return sym.type == SymbolType::Class || sym.type == SymbolType::Interface ||
                       sym.type == SymbolType::Funcdef || sym.type == SymbolType::Enum ||
                       sym.type == SymbolType::Typedef;
            });
        }

        bool IsFunctionSymbol(const Symbol &sym)
        {
            return sym.type == SymbolType::Function &&
                   std::holds_alternative<FunctionSignature>(sym.signature);
        }

        /** @brief What the pass concluded about one call's candidates. */
        struct CandidateSet
        {
            bool decided = false;  ///< False means stay silent: nothing visible to judge against.
            bool accepts = false;  ///< True when some candidate takes the written argument count.
        };

        CandidateSet JudgeAgainst(const std::vector<Symbol> &candidates, uint32_t argumentCount)
        {
            CandidateSet result;
            for (const auto &sym : candidates)
            {
                result.decided = true;

                const Arity arity = ArityOf(sym.GetFunction());
                if (arity.variadic)
                {
                    // Accepts any count, so the question is answered and closed.
                    result.accepts = true;
                    return result;
                }
                if (argumentCount >= arity.required && argumentCount <= arity.maximum)
                {
                    result.accepts = true;
                    return result;
                }
            }
            return result;
        }

        /**
         * @brief True when every type in a chain resolves to a declaration this analyzer can read.
         *
         * The claim is "no visible declaration takes this many arguments", so every declaration has
         * to be on the table before it may be made. One unresolved base is an engine-registered
         * type, and those carry overloads written down in no source here.
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

        /** @brief Declarations of a method, across a type's whole visible hierarchy. */
        std::vector<Symbol> FindMethodCandidates(const std::string &typeName,
                                                 const std::string &methodName,
                                                 const SymbolTable &table)
        {
            std::vector<Symbol> candidates;
            for (const auto &owner : GetInheritedTypeHierarchy(typeName, table))
            {
                const auto found = table.FindSymbolsPtr(owner + "::" + methodName);
                if (!found)
                {
                    continue;
                }
                for (const auto &sym : *found)
                {
                    if (IsFunctionSymbol(sym))
                    {
                        candidates.push_back(sym);
                    }
                }
            }
            return candidates;
        }

        /**
         * @brief Declarations an unqualified call can reach, when that question is answerable.
         *
         * Narrower than it first looks, and every restriction was put here by a corpus finding:
         *
         * - a call written inside a class body is not judged at all. `VoteBlocked(this.VoteBlocked)`
         *   builds a delegate from an engine-registered funcdef, and the enclosing class declaring
         *   a method of that name is a coincidence - but the funcdef is invisible, so the two are
         *   indistinguishable from here. `Precache(keyvalues)` is the same shape;
         * - a global declared in another file is not a candidate. Two Sven Co-op plugins that never
         *   include one another both declare `Stop`, and matching a call in one against the other's
         *   signature is reading a relationship that does not exist. Same file is the one module
         *   boundary this pass can be certain of.
         *
         * What is left is the case people actually get wrong and this can actually decide: a call
         * to a function declared beside it.
         */
        std::vector<Symbol> FindFreeCandidates(TSNode callNode,
                                               const std::string &name,
                                               std::string_view sourceCode,
                                               const std::string &fileUri,
                                               const std::string &predefinedExtension,
                                               const SymbolTable &table)
        {
            std::vector<Symbol> candidates;

            // The table lookup comes first because it is a hash probe and the container walk is a
            // climb up the tree, and most unqualified calls in real code name an engine function
            // with no visible declaration at all - so the cheap test answers nearly all of them.
            const auto globals = table.FindSymbolsPtr(name);
            if (!globals)
            {
                return candidates;
            }

            for (const auto &sym : *globals)
            {
                if (IsFunctionSymbol(sym) && sym.containerName.empty() &&
                    (sym.fileUri == fileUri || utils::IsPredefinedFile(sym.fileUri, predefinedExtension)))
                {
                    candidates.push_back(sym);
                }
            }

            if (candidates.empty())
            {
                return candidates;
            }

            for (const auto &container : GetEnclosingContainers(callNode, sourceCode))
            {
                if (container.kind == ContainerKind::Class || container.kind == ContainerKind::Interface)
                {
                    candidates.clear();
                    return candidates;
                }
            }
            return candidates;
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

        void CheckCall(TSNode node, const CallCheckRequest &request, const Scope *scope, DiagnosticContext &ctx)
        {
            TSNode callee = ts_node_child_by_field_name(node, "function", k_functionFieldLength);
            TSNode arguments = ts_node_child_by_field_name(node, "arguments", k_argumentsFieldLength);
            if (ts_node_is_null(callee) || ts_node_is_null(arguments))
            {
                return;
            }

            const SymbolTable &table = ctx.request.symbolTable;
            const std::string_view calleeType = ts_node_type(callee);
            const uint32_t argumentCount = CountArguments(arguments);

            std::vector<Symbol> candidates;
            std::string reportedName;

            if (calleeType == "member_expression")
            {
                TSNode objectNode = ts_node_child_by_field_name(callee, "object", k_objectFieldLength);
                TSNode memberNode = ts_node_child_by_field_name(callee, "member", k_memberFieldLength);
                if (ts_node_is_null(objectNode) || ts_node_is_null(memberNode))
                {
                    return;
                }

                const std::string objectType = CleanBaseType(ResolveExpressionType(
                    objectNode, scope, table, request.sourceCode, ctx.request.fileUri));
                if (objectType.empty() || !HierarchyIsFullyVisible(objectType, table))
                {
                    return;
                }

                reportedName = NodeText(memberNode, request.sourceCode);
                candidates = FindMethodCandidates(objectType, reportedName, table);
            }
            else if (calleeType == "scoped_identifier" || calleeType == "identifier")
            {
                const std::string written = NodeText(callee, request.sourceCode);
                if (written.empty())
                {
                    return;
                }

                // A local or a parameter of the same name shadows the function: `Callback@ Think;`
                // then `Think()` is a call through a handle, and which funcdef that reaches is not
                // a question this pass answers.
                //
                // Only those two kinds. The scope tree also records functions and methods as
                // definitions, and treating one as a shadow of itself silenced every unqualified
                // call there was - which is what the first run of these tests found.
                if (scope)
                {
                    const LocalDefinition *shadow = ResolveInScope(scope, LastScopeSegment(written));
                    if (shadow && (shadow->kind == LocalDefinitionKind::Variable ||
                                   shadow->kind == LocalDefinitionKind::Parameter))
                    {
                        return;
                    }
                }

                if (NamesAType(LastScopeSegment(written), table))
                {
                    return;
                }

                reportedName = LastScopeSegment(written);

                if (written.find("::") != std::string::npos)
                {
                    // A qualified name is the key the collector stored it under, and it names one
                    // thing - so it is asked for whole, wherever it was declared.
                    if (const auto found = table.FindSymbolsPtr(written))
                    {
                        for (const auto &sym : *found)
                        {
                            if (IsFunctionSymbol(sym))
                            {
                                candidates.push_back(sym);
                            }
                        }
                    }
                }
                else
                {
                    candidates = FindFreeCandidates(node, written, request.sourceCode,
                                                    ctx.request.fileUri, ctx.request.predefinedFileExtension, table);
                }
            }
            else
            {
                // A lambda, an indexed value, a call through a returned handle: no declaration to
                // count against.
                return;
            }

            const CandidateSet judged = JudgeAgainst(candidates, argumentCount);
            if (!judged.decided || !judged.accepts)
            {
                if (judged.decided && !judged.accepts)
                {
                    const TSPoint start = ts_node_start_point(callee);
                    const TSPoint end = ts_node_end_point(arguments);
                    ctx.EmitAtRange(start.row, start.column, end.row, end.column,
                                    "as-err-call-argument-count", reportedName, std::to_string(argumentCount));
                }
                return;
            }

            // Argument count is valid. Now check argument types against overload candidates.
            std::vector<TSNode> argNodes = GetArgumentNodes(arguments);
            std::vector<std::string> argTypes;
            bool allArgsResolved = true;

            for (const auto &argNode : argNodes)
            {
                std::string argType = ResolveExpressionType(argNode, scope, table, request.sourceCode, ctx.request.fileUri);
                if (argType.empty())
                {
                    allArgsResolved = false;
                }
                argTypes.push_back(argType);
            }

            if (allArgsResolved && !argTypes.empty())
            {
                std::vector<Symbol> matchingArityCandidates;
                for (const auto &sym : candidates)
                {
                    const Arity arity = ArityOf(sym.GetFunction());
                    if (arity.variadic || (argumentCount >= arity.required && argumentCount <= arity.maximum))
                    {
                        matchingArityCandidates.push_back(sym);
                    }
                }

                if (!matchingArityCandidates.empty())
                {
                    OverloadMatchResult match = ResolveBestOverload(matchingArityCandidates, argTypes, table);
                    if (match.isAmbiguous)
                    {
                        const TSPoint start = ts_node_start_point(callee);
                        const TSPoint end = ts_node_end_point(arguments);
                        ctx.EmitAtRange(start.row, start.column, end.row, end.column,
                                        "as-err-call-ambiguous", reportedName);
                    }
                    else if (match.viableCandidates.empty() || match.bestScore >= 999)
                    {
                        bool emittedSpecificConversion = false;
                        if (matchingArityCandidates.size() == 1)
                        {
                            const auto &fn = matchingArityCandidates[0].GetFunction();
                            for (size_t i = 0; i < argTypes.size() && i < fn.parameters.size(); ++i)
                            {
                                int score = ScoreArgumentMatch(argTypes[i], fn.parameters[i], table);
                                if (score >= 999)
                                {
                                    const TSPoint aStart = ts_node_start_point(argNodes[i]);
                                    const TSPoint aEnd = ts_node_end_point(argNodes[i]);
                                    ctx.EmitAtRange(aStart.row, aStart.column, aEnd.row, aEnd.column,
                                                    "as-err-no-implicit-conversion", argTypes[i], fn.parameters[i].typeName);
                                    emittedSpecificConversion = true;
                                    break;
                                }
                            }
                        }

                        if (!emittedSpecificConversion)
                        {
                            const TSPoint start = ts_node_start_point(callee);
                            const TSPoint end = ts_node_end_point(arguments);
                            ctx.EmitAtRange(start.row, start.column, end.row, end.column,
                                            "as-err-call-no-matching-signature", reportedName);
                        }
                    }
                }
            }
        }

        void VisitNode(TSNode node, const CallCheckRequest &request, DiagnosticContext &ctx)
        {
            if (std::string_view(ts_node_type(node)) == "call_expression")
            {
                const TSPoint start = ts_node_start_point(node);
                CheckCall(node, request,
                          FindInnermostScope(request.scopeRoot, start.row, start.column), ctx);
            }

            const uint32_t childCount = ts_node_named_child_count(node);
            for (uint32_t i = 0; i < childCount; ++i)
            {
                VisitNode(ts_node_named_child(node, i), request, ctx);
            }
        }
    }

    void CheckCallArguments(const CallCheckRequest &request, DiagnosticContext &ctx)
    {
        if (ts_node_is_null(request.root) || request.sourceCode.empty())
        {
            return;
        }

        // A stub describes an API rather than using one, so it has no calls worth judging - the
        // same exemption every other use-site pass carries.
        if (utils::IsPredefinedFile(ctx.request.fileUri, ctx.request.predefinedFileExtension))
        {
            return;
        }

        VisitNode(request.root, request, ctx);
    }
}
