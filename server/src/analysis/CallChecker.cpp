#include "analysis/CallChecker.h"
#include "analysis/ASTUtils.h"
#include "analysis/InitializerListChecker.h"
#include "analysis/OverloadResolver.h"
#include "analysis/SemanticHelpers.h"
#include "utils/Utils.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>
#include "parser/GrammarNames.h"

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

        constexpr uint32_t k_functionFieldLength = 8;  ///< "function"
        constexpr uint32_t k_argumentsFieldLength = 9; ///< "arguments"
        constexpr uint32_t k_objectFieldLength = 6;    ///< "object"
        constexpr uint32_t k_memberFieldLength = 6;    ///< "member"

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
        /**
         * @brief True when a member name is its own class's constructor or destructor.
         *
         * Matched by name because that is the convention the analyzer uses throughout - a
         * constructor is an ordinary Function stored under `Class::Class` (see the constructor
         * lookup further down, which relies on the same thing). The type may arrive qualified, so
         * the comparison is against its last `::` segment.
         */
        bool IsConstructorOrDestructorName(const std::string &memberName, const std::string &typeName)
        {
            if (memberName.empty() || typeName.empty())
            {
                return false;
            }

            const size_t at = typeName.rfind("::");
            const std::string_view shortName =
                at == std::string::npos ? std::string_view(typeName)
                                        : std::string_view(typeName).substr(at + 2);

            if (memberName == shortName)
            {
                return true;
            }

            return memberName.front() == '~' && std::string_view(memberName).substr(1) == shortName;
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
                    if (!IsFunctionSymbol(sym))
                    {
                        continue;
                    }

                    // A method redeclared in a subclass OVERRIDES the base's; it does not compete
                    // with it. GetInheritedTypeHierarchy walks derived-first, so the first
                    // declaration of a given parameter list is the one that wins and every later
                    // one is the same method seen further up.
                    //
                    // Without this the hierarchy handed back both, they scored identically, and the
                    // call was reported "Multiple matching signatures" - 75 times over the corpus,
                    // every one on legal code. The library that produces them declares
                    // `class json : meta_api::json::v2::json` and restates its methods, which is an
                    // ordinary way to write an interface summary. HasSameSignature did not catch it
                    // because it compares the qualified name too, and `json::Contains` and
                    // `meta_api::json::v2::json::Contains` are genuinely different names for what
                    // is one method.
                    //
                    // The same tie also handed DefiniteAssignmentChecker an arbitrary overload to
                    // read `&out` from, which is where the last six of its findings came from.
                    const bool overriddenLower =
                        std::any_of(candidates.begin(), candidates.end(),
                                    [&sym](const Symbol &kept) { return HasSameParameterList(kept, sym); });
                    if (!overriddenLower)
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
                                               const SymbolTable &table,
                                               const rules::RuleIndex &index)
        {
            std::vector<Symbol> candidates;

            // One hash probe before the tree climb. allNames holds the unqualified spelling of
            // every symbol in the workspace, so a name nothing declares - which is what most
            // unqualified calls in real code are, naming an engine function with no visible
            // declaration - is answered here and never walks anything.
            if (!index.allNames.contains(name))
            {
                return candidates;
            }

            // Which scopes an unqualified name may name from here, innermost first, ending at the
            // global scope. This pass used to look only at the global one, and the collector keys a
            // namespaced function under its qualified name alone - `TEST::my_test_func`, never
            // `my_test_func` - so inside a namespace the probe found nothing and every call in the
            // file went unchecked. The identical call at file scope was checked.
            // tests/parity/doc_r12 against doc_r14 is exactly that pair.
            std::vector<std::string> reachableScopes;
            for (const auto &container : GetEnclosingContainers(callNode, sourceCode))
            {
                if (container.kind == ContainerKind::Class || container.kind == ContainerKind::Interface)
                {
                    // A call inside a class body is a method call on `this` as often as not, and
                    // FindMethodCandidates owns that question.
                    return candidates;
                }
                if (container.kind == ContainerKind::Namespace)
                {
                    reachableScopes.push_back(container.qualifiedName);
                }
            }
            reachableScopes.emplace_back();

            // Same file, or a predefined stub. A global declared in another file is not a
            // candidate: two plugins that never include one another both declare `Stop`, and
            // matching a call in one against the other's signature reads a relationship that does
            // not exist.
            const auto collectFrom = [&](const std::string &scopeName)
            {
                const std::string key = scopeName.empty() ? name : scopeName + "::" + name;
                const auto found = table.FindSymbolsPtr(key);
                if (!found)
                {
                    return;
                }

                for (const auto &sym : *found)
                {
                    if (IsFunctionSymbol(sym) &&
                        (sym.fileUri == fileUri || utils::IsPredefinedFile(sym.fileUri, predefinedExtension)))
                    {
                        candidates.push_back(sym);
                    }
                }
            };

            // A lexical scope shadows: the first one that declares the name is the only one
            // consulted, and an overload in an enclosing scope does not join it. The compiler is
            // explicit about that -
            //
            //     void f(int i) {}
            //     namespace N { void f(string s) {} void g() { f(1); } }
            //                                                  ^ No matching signatures to 'f(const int)'
            //
            // - so breaking matters for the verdict and not only for speed. A wider set can only
            // make a bad call look matchable.
            for (const auto &scopeName : reachableScopes)
            {
                collectFrom(scopeName);
                if (!candidates.empty())
                {
                    break;
                }
            }

            // A using-directive does not shadow, and does not stop the search either: when nothing
            // lexical declares the name, every imported namespace contributes at once and ordinary
            // overload resolution decides between them. Again from the compiler:
            //
            //     namespace A { void f(string s) {} }
            //     namespace B { void f(int i) {} }
            //     using namespace A;  using namespace B;
            //     void g() { f(1); }              // compiles, picks B::f
            //     void g() { f("x"); }            // Multiple matching signatures, when both take string
            //
            // which is why these are merged rather than broken on, and why an ambiguity among them
            // is left to ResolveBestOverload to find instead of being special-cased here.
            //
            // Collected from the whole document rather than from each directive's own scope. A
            // `using namespace` inside a namespace body is scoped to it, so this is wider than the
            // language - but only in the direction of finding a declaration that really exists,
            // which at worst judges a call the engine would reject as undefined anyway.
            if (candidates.empty())
            {
                TSNode documentRoot = callNode;
                while (!ts_node_is_null(ts_node_parent(documentRoot)))
                {
                    documentRoot = ts_node_parent(documentRoot);
                }

                for (const auto &imported : CollectUsingNamespaces(documentRoot, sourceCode))
                {
                    if (!imported.empty() &&
                        std::find(reachableScopes.begin(), reachableScopes.end(), imported) ==
                            reachableScopes.end())
                    {
                        collectFrom(imported);
                    }
                }
            }

            return candidates;
        }

        /**
         * @brief Judges a lambda argument against the funcdef parameter it lands on.
         *
         * The same problem the initializer-list check above solves, and for the same reason: a
         * lambda resolves to no type, so `allArgsResolved` is false and the entire overload check
         * below is skipped for any call that takes one. Every lambda in the 1,061-file corpus is a
         * call argument, so that silence covered all of them.
         *
         * Measured against angelscript_oracle:
         *
         *     void Take(CB@ c);   Take(function(int a){})  -> accepted
         *                         Take(function(a){})      -> accepted, type comes from CB
         *                         Take(function(){})       -> "No matching signatures to
         *                                                      'Take(<auto> lambda())'"
         *     void T(A@); void T(B@);
         *                         T(function(int a){})     -> accepted, one overload matches
         *                         T(function(int, int){})  -> "No matching signatures"
         *                         T(function(a){})         -> "Multiple matching signatures"
         *
         * A candidate is viable when every lambda argument satisfies the funcdef at its position;
         * none viable is the rejection, more than one is the ambiguity. If ANY candidate's
         * parameter at a lambda position is not a funcdef this analyzer can see - the usual case
         * for a host-registered callback - nothing is judged at all, because the question is then
         * about a signature that is not in the workspace.
         */
        void CheckLambdaArguments(const std::vector<TSNode> &argNodes,
                                  const std::vector<Symbol> &candidates,
                                  TSNode callee,
                                  TSNode arguments,
                                  const std::string &reportedName,
                                  const SymbolTable &table,
                                  std::string_view sourceCode,
                                  DiagnosticContext &ctx)
        {
            std::vector<size_t> lambdaPositions;
            for (size_t i = 0; i < argNodes.size(); ++i)
            {
                if (IsLambdaExpression(argNodes[i]))
                {
                    lambdaPositions.push_back(i);
                }
            }
            if (lambdaPositions.empty() || candidates.empty())
            {
                return;
            }

            // The funcdef each accepting candidate wants, per lambda position. Two candidates that
            // name the SAME funcdef are the same overload reached twice - the corpus flattens
            // twenty projects into one directory, so a function declared once per project arrives
            // as several identical symbols - and that is not the ambiguity the compiler means.
            std::vector<std::vector<std::string>> acceptedShapes;

            for (const auto &candidate : candidates)
            {
                const auto &fn = candidate.GetFunction();
                std::vector<std::string> shape;
                bool takesEveryLambda = true;

                for (const size_t position : lambdaPositions)
                {
                    if (position >= fn.parameters.size())
                    {
                        takesEveryLambda = false;
                        break;
                    }

                    const std::string parameterType = CleanBaseType(fn.parameters[position].typeName);
                    const auto funcdef = FindFuncdefSymbol(parameterType, table);
                    if (!funcdef)
                    {
                        return;
                    }
                    if (LambdaContradictsFuncdef(argNodes[position], funcdef->GetFuncdef(),
                                                 table, sourceCode))
                    {
                        takesEveryLambda = false;
                        break;
                    }
                    shape.push_back(funcdef->name);
                }

                if (takesEveryLambda)
                {
                    acceptedShapes.push_back(std::move(shape));
                }
            }

            const TSPoint start = ts_node_start_point(callee);
            const TSPoint end = ts_node_end_point(arguments);

            if (acceptedShapes.empty())
            {
                ctx.EmitAtRange(start.row, start.column, end.row, end.column,
                                "as-err-call-no-matching-signature", reportedName);
                return;
            }

            const bool everyShapeIdentical =
                std::all_of(acceptedShapes.begin(), acceptedShapes.end(),
                            [&](const std::vector<std::string> &shape)
                            { return shape == acceptedShapes.front(); });
            if (acceptedShapes.size() > 1 && !everyShapeIdentical)
            {
                ctx.EmitAtRange(start.row, start.column, end.row, end.column,
                                "as-err-call-ambiguous", reportedName);
            }
        }

        void CheckCall(TSNode node, const CallCheckRequest &request, const Scope *scope, DiagnosticContext &ctx)
        {
            TSNode callee = parser::GetChildByField(node, parser::fields::Function);
            TSNode arguments = parser::GetChildByField(node, parser::fields::Arguments);
            if (ts_node_is_null(callee) || ts_node_is_null(arguments))
            {
                return;
            }

            const SymbolTable &table = ctx.request.symbolTable;
            const std::string_view calleeType = ts_node_type(callee);
            const uint32_t argumentCount = CountArguments(arguments);

            std::vector<Symbol> candidates;

            // Whether the set came from free-function lookup rather than from a type's members.
            // Only the free set is a complete picture: member lookup has precedence rules this pass
            // does not model - a mixin's method beats the base class's, and the class's own beats
            // the mixin's - so two same-named members are routinely not a choice at all.
            bool candidatesAreFreeFunctions = false;
            std::string reportedName;

            if (calleeType == "member_expression")
            {
                TSNode objectNode = parser::GetChildByField(callee, parser::fields::Object);
                TSNode memberNode = parser::GetChildByField(callee, parser::fields::Member);
                if (ts_node_is_null(objectNode) || ts_node_is_null(memberNode))
                {
                    return;
                }

                // `int[]` is `array<int>`, and only the template spelling reached the branch below.
                // The bracket one cleaned to `int`, which has no hierarchy, so the visibility guard
                // sent every call on a bracket-declared array away unchecked.
                const std::string rawObjType = CanonicalizeArrayType(
                    ResolveExpressionType(objectNode, scope, table, request.sourceCode, ctx.request.fileUri),
                    ctx.request.GetArrayTypeName().empty() ? "array" : ctx.request.GetArrayTypeName());
                std::string objectType = CleanBaseType(rawObjType);
                std::vector<std::string> templateArgs;
                if (rawObjType.find('<') != std::string::npos && rawObjType.ends_with('>'))
                {
                    size_t openBracket = rawObjType.find('<');
                    std::string tmplName = rawObjType.substr(0, openBracket);
                    while (!tmplName.empty() && isspace(static_cast<unsigned char>(tmplName.front()))) tmplName.erase(tmplName.begin());
                    while (!tmplName.empty() && isspace(static_cast<unsigned char>(tmplName.back()))) tmplName.pop_back();
                    if (table.FindSymbolsPtr(tmplName))
                    {
                        objectType = tmplName;
                        std::string argStr = rawObjType.substr(openBracket + 1, rawObjType.size() - openBracket - 2);
                        templateArgs.push_back(argStr);
                    }
                }

                if (objectType.empty() || !HierarchyIsFullyVisible(objectType, table))
                {
                    return;
                }

                reportedName = NodeText(memberNode, request.sourceCode);

                // A constructor is not a member you can call on an instance. AngelScript has no
                // syntax for it - the real compiler answers `t.Thing()` with "No matching symbol
                // 'Thing'" - but this analyzer resolved it happily, because a constructor is stored
                // as `Thing::Thing` and that is exactly the key a method lookup builds. So the
                // lookup succeeded and the call was accepted.
                //
                // Safe to report: the guard above has already established that the type and its
                // whole hierarchy are visible, and a member whose name is its own class is a
                // constructor in every case - there is no other declaration that can produce it.
                if (IsConstructorOrDestructorName(reportedName, objectType))
                {
                    const TSPoint ctorStart = ts_node_start_point(memberNode);
                    const TSPoint ctorEnd = ts_node_end_point(memberNode);
                    ctx.EmitAtRange(ctorStart.row, ctorStart.column, ctorEnd.row, ctorEnd.column,
                                    "as-err-constructor-not-callable", reportedName, objectType);
                    return;
                }

                candidates = FindMethodCandidates(objectType, reportedName, table);
                if (!templateArgs.empty())
                {
                    for (auto &sym : candidates)
                    {
                        if (sym.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(sym.signature))
                        {
                            auto fn = sym.GetFunction();
                            for (auto &p : fn.parameters)
                            {
                                p.typeName = SubstituteTypeParam(p.typeName, "T", templateArgs[0]);
                                p.baseTypeName = SubstituteTypeParam(p.baseTypeName, "T", templateArgs[0]);
                            }
                            sym.signature = fn;
                        }
                    }
                }
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
                    candidatesAreFreeFunctions = true;
                    candidates = FindFreeCandidates(node, written, request.sourceCode,
                                                    ctx.request.fileUri, ctx.request.predefinedFileExtension,
                                                    table, ctx.request.GetRuleIndex());
                }
            }
            else
            {
                // A lambda, an indexed value, a call through a returned handle: no declaration to
                // count against.
                return;
            }

            // Check for positional arguments following named arguments
            bool sawNamedArg = false;
            const uint32_t totalChildren = ts_node_child_count(arguments);
            std::vector<std::vector<TSNode>> argGroups;
            std::vector<TSNode> currentGroup;
            for (uint32_t i = 0; i < totalChildren; ++i)
            {
                TSNode child = ts_node_child(arguments, i);
                std::string_view ct = ts_node_type(child);
                if (ct == "(" || ct == ")" || ct == "comment")
                {
                    continue;
                }
                if (ct == ",")
                {
                    if (!currentGroup.empty())
                    {
                        argGroups.push_back(std::move(currentGroup));
                        currentGroup.clear();
                    }
                }
                else
                {
                    currentGroup.push_back(child);
                }
            }
            if (!currentGroup.empty())
            {
                argGroups.push_back(std::move(currentGroup));
            }

            for (const auto &group : argGroups)
            {
                bool isNamed = false;
                for (const auto &token : group)
                {
                    std::string_view tType = ts_node_type(token);
                    if (tType == ":" || tType == "named_argument")
                    {
                        isNamed = true;
                        break;
                    }
                    std::string text = NodeText(token, request.sourceCode);
                    if (text.find(':') != std::string::npos && text.find('"') == std::string::npos && text.find('\'') == std::string::npos)
                    {
                        isNamed = true;
                        break;
                    }
                }

                if (isNamed)
                {
                    sawNamedArg = true;
                }
                else if (sawNamedArg)
                {
                    if (!group.empty())
                    {
                        const TSPoint aStart = ts_node_start_point(group.front());
                        const TSPoint aEnd = ts_node_end_point(group.back());
                        ctx.EmitAtRange(aStart.row, aStart.column, aEnd.row, aEnd.column,
                                        "as-err-positional-after-named-arg");
                    }
                    return;
                }
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

            // A zero-argument *free* call is judged too. The empty-argument guard that used to sit
            // here read as harmless - with no arguments there are no argument types to check - but
            // ambiguity does not need one:
            //
            //     namespace PackageA { void Initialize() {} }
            //     namespace PackageB { void Initialize() {} }
            //     using namespace PackageA;  using namespace PackageB;
            //     void Main() { Initialize(); }
            //                   ^ Multiple matching signatures to 'Initialize()'
            //
            // Two identical signatures under different qualified names is what HasSameSignature
            // already distinguishes from the same function declared twice, so nothing else had to
            // change for that.
            //
            // Members are excluded, and the corpus is why: `class DerivedA : Base, MixinA {}` where
            // both declare `GetName()` compiles, because the mixin's member beats the base's and
            // the class's own beats the mixin's. Those precedence rules live in member lookup, not
            // here, so a member set of two same-named candidates is not evidence of a choice.
            std::vector<Symbol> matchingArityCandidates;
            for (const auto &sym : candidates)
            {
                const Arity arity = ArityOf(sym.GetFunction());
                if (arity.variadic || (argumentCount >= arity.required && argumentCount <= arity.maximum))
                {
                    matchingArityCandidates.push_back(sym);
                }
            }

            // An initializer list argument. `take({1, 2})` compiles - the compiler takes the target
            // type from the parameter the list lands on and builds the list against it, so
            // `take({"x"})` against `array<int>` is "Can't implicitly convert from 'const string' to
            // 'int&'" (tests/parity/doc_r18_initlist_call_argument.as). The list itself resolves to
            // no type, so nothing in this pass ever judged it.
            //
            // Only where the parameter is not in question: one candidate of this arity, and no
            // named argument to move the positions around. With two overloads the compiler's own
            // answer to a list argument is "Multiple matching signatures to 'take({...})'" - which
            // parameter type the list was meant for is precisely what is undecided, so a verdict
            // about its shape would be a guess wearing an error's clothes.
            if (matchingArityCandidates.size() == 1 && !sawNamedArg)
            {
                const auto &only = matchingArityCandidates.front().GetFunction();
                for (size_t i = 0; i < argNodes.size() && i < only.parameters.size(); ++i)
                {
                    if (NodeType(argNodes[i]) == "initializer_list")
                    {
                        CheckInitializerListAgainstType(argNodes[i], only.parameters[i].typeName,
                                                        request.sourceCode, scope, ctx);
                    }
                }
            }

            // A lambda argument, which resolves to no type and therefore turns allArgsResolved
            // off for the whole call - see CheckLambdaArguments for what the compiler does answer.
            if (!sawNamedArg)
            {
                CheckLambdaArguments(argNodes, matchingArityCandidates, callee, arguments,
                                     reportedName, table, request.sourceCode, ctx);
            }

            if (allArgsResolved && (!argTypes.empty() || candidatesAreFreeFunctions))
            {
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
                        // Which argument is at fault, when every candidate agrees on it.
                        //
                        // With one overload the answer is simply the first parameter that cannot
                        // take its argument. With several it is only meaningful when they all fail
                        // at the same position: if one rejects argument 0 and another argument 1,
                        // there is no single offending argument to point at and the generic message
                        // is the honest one. Underlining a position only some overloads object to
                        // would be worse than saying nothing specific.
                        bool emittedSpecificConversion = false;
                        {
                            size_t blamedArgument = argTypes.size();
                            bool everyCandidateAgrees = !matchingArityCandidates.empty();

                            for (const auto &candidate : matchingArityCandidates)
                            {
                                const auto &fn = candidate.GetFunction();
                                size_t firstBad = argTypes.size();
                                for (size_t i = 0; i < argTypes.size() && i < fn.parameters.size(); ++i)
                                {
                                    if (ScoreArgumentMatch(argTypes[i], fn.parameters[i], table) >= 999)
                                    {
                                        firstBad = i;
                                        break;
                                    }
                                }

                                if (firstBad == argTypes.size())
                                {
                                    // This overload takes every argument, so the call failed for a
                                    // reason no single argument explains.
                                    everyCandidateAgrees = false;
                                    break;
                                }
                                if (blamedArgument == argTypes.size())
                                {
                                    blamedArgument = firstBad;
                                }
                                else if (blamedArgument != firstBad)
                                {
                                    everyCandidateAgrees = false;
                                    break;
                                }
                            }

                            if (everyCandidateAgrees && blamedArgument < argNodes.size())
                            {
                                // The expected type is named from the first candidate; with several
                                // they differ, and one concrete example reads better than a list.
                                const auto &fn = matchingArityCandidates[0].GetFunction();
                                const std::string expected =
                                    blamedArgument < fn.parameters.size()
                                        ? fn.parameters[blamedArgument].typeName
                                        : std::string();

                                const TSPoint aStart = ts_node_start_point(argNodes[blamedArgument]);
                                const TSPoint aEnd = ts_node_end_point(argNodes[blamedArgument]);
                                ctx.EmitAtRange(aStart.row, aStart.column, aEnd.row, aEnd.column,
                                                "as-err-no-implicit-conversion",
                                                argTypes[blamedArgument], expected);
                                emittedSpecificConversion = true;
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
                    else
                    {
                        const auto &targetCandidate = match.bestCandidate ? *match.bestCandidate : matchingArityCandidates[0];
                        const auto &fn = targetCandidate.GetFunction();
                        for (size_t i = 0; i < argNodes.size() && i < fn.parameters.size(); ++i)
                        {
                            const auto &param = fn.parameters[i];
                            if (param.rawText.find("&out") != std::string::npos ||
                                param.typeName.find("&out") != std::string::npos ||
                                param.typeName.find("& out") != std::string::npos)
                            {
                                std::string aText = NodeText(argNodes[i], request.sourceCode);
                                while (!aText.empty() && isspace(static_cast<unsigned char>(aText.front()))) aText.erase(aText.begin());
                                while (!aText.empty() && isspace(static_cast<unsigned char>(aText.back()))) aText.pop_back();
                                if (aText != "void")
                                {
                                    TSNode argNode = argNodes[i];
                                    std::string_view aType = ts_node_type(argNode);

                                    // `@x` is how a handle is handed to a `?&out` parameter, and it
                                    // is every bit as much an l-value as `x` - `ref::opCast(?&out)`
                                    // is called as `r.opCast(@target);` and the real compiler
                                    // accepts it. Looking only at the outer node saw a unary
                                    // expression and reported the argument as unassignable.
                                    if (aType == "unary_expression" && aText.starts_with("@"))
                                    {
                                        TSNode operand = parser::GetChildByField(argNode, parser::fields::Operand);
                                        if (!ts_node_is_null(operand))
                                        {
                                            argNode = operand;
                                            aType = ts_node_type(argNode);
                                            aText = NodeText(argNode, request.sourceCode);
                                            while (!aText.empty() && isspace(static_cast<unsigned char>(aText.front()))) aText.erase(aText.begin());
                                            while (!aText.empty() && isspace(static_cast<unsigned char>(aText.back()))) aText.pop_back();
                                        }
                                    }

                                    bool isLVal = false;
                                    if (aType == "identifier" || aType == "scoped_identifier")
                                    {
                                        if (scope)
                                        {
                                            const auto *def = ResolveInScope(scope, aText);
                                            if (def && (def->kind == LocalDefinitionKind::Variable || def->kind == LocalDefinitionKind::Parameter))
                                            {
                                                isLVal = true;
                                            }
                                        }
                                        if (!isLVal)
                                        {
                                            auto syms = table.FindSymbols(aText);
                                            for (const auto &s : syms)
                                            {
                                                if (s.type == SymbolType::Variable && !s.GetVariable().modifiers.isConst)
                                                {
                                                    isLVal = true;
                                                    break;
                                                }
                                            }
                                        }
                                    }
                                    else if (aType == "member_expression" || aType == "index_expression")
                                    {
                                        isLVal = true;
                                    }
                                    if (!isLVal)
                                    {
                                        const TSPoint aStart = ts_node_start_point(argNodes[i]);
                                        const TSPoint aEnd = ts_node_end_point(argNodes[i]);
                                        ctx.EmitAtRange(aStart.row, aStart.column, aEnd.row, aEnd.column,
                                                        "as-err-lvalue-required-for-out-param");
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        void CheckVariableDirectInitialization(
            TSNode varDeclNode,
            const CallCheckRequest &request,
            DiagnosticContext &ctx)
        {
            TSNode varTypeNode = parser::GetChildByField(varDeclNode, parser::fields::VarType);
            if (ts_node_is_null(varTypeNode))
            {
                varTypeNode = parser::GetChildByField(varDeclNode, parser::fields::Type);
            }
            if (ts_node_is_null(varTypeNode))
            {
                uint32_t count = ts_node_named_child_count(varDeclNode);
                for (uint32_t i = 0; i < count; ++i)
                {
                    TSNode child = ts_node_named_child(varDeclNode, i);
                    if (std::string_view(ts_node_type(child)) == "type")
                    {
                        varTypeNode = child;
                        break;
                    }
                }
            }
            if (ts_node_is_null(varTypeNode))
            {
                return;
            }

            std::string rawTypeStr = NodeText(varTypeNode, request.sourceCode);
            std::string declaredType = CleanExpressionType(rawTypeStr);
            if (declaredType.empty() || declaredType == "auto")
            {
                return;
            }

            // `int[] a(33)` sizes an array; it does not construct an `int` from 33. The bracket
            // spelling reduces to its ELEMENT type here - CleanBaseType takes `bool[]` to `bool` -
            // so the primitive branch below read `bool[] flags(32+1);` as converting 32+1 into a
            // bool. It stayed silent for years because `int[] a(33)` and `float[] a(33)` are the
            // same misreading and `int -> int` and `int -> float` score fine; correcting `bool` to
            // be unconvertible from the numeric types is what made it visible, on
            // `bool[] g_playerGlowEnable(32+1);` in the corpus.
            //
            // The angle spelling needs no guard: `array<int>` keeps `array` as its container name,
            // which is not a primitive, so it already takes the class path.
            if (rawTypeStr.find('[') != std::string::npos)
            {
                return;
            }

            TemplateTypeInfo tmplInfo = ParseTemplateType(declaredType);
            std::string baseName = tmplInfo.containerName.empty() ? declaredType : tmplInfo.containerName;
            baseName = CleanBaseType(baseName);

            uint32_t declaratorCount = ts_node_named_child_count(varDeclNode);
            for (uint32_t d = 0; d < declaratorCount; ++d)
            {
                TSNode declarator = ts_node_named_child(varDeclNode, d);
                if (std::string_view(ts_node_type(declarator)) != "variable_declarator")
                {
                    continue;
                }

                TSNode argListNode = parser::GetChildByField(declarator, parser::fields::Arguments);
                if (ts_node_is_null(argListNode))
                {
                    continue;
                }

                const TSPoint start = ts_node_start_point(declarator);
                const Scope *scope = FindInnermostScope(request.scopeRoot, start.row, start.column);

                std::vector<TSNode> argNodes = GetArgumentNodes(argListNode);
                uint32_t argCount = static_cast<uint32_t>(argNodes.size());

                std::vector<std::string> argTypes;
                for (TSNode argNode : argNodes)
                {
                    argTypes.push_back(ResolveExpressionType(argNode, scope, ctx.request.symbolTable, request.sourceCode, ctx.request.fileUri));
                }

                if (IsCorePrimitive(baseName))
                {
                    if (argCount != 1)
                    {
                        const TSPoint aStart = ts_node_start_point(argListNode);
                        const TSPoint aEnd = ts_node_end_point(argListNode);
                        std::string sigAttempt = declaredType + "(";
                        for (size_t a = 0; a < argTypes.size(); ++a)
                        {
                            if (a > 0) sigAttempt += ", ";
                            sigAttempt += argTypes[a];
                        }
                        sigAttempt += ")";
                        ctx.EmitAtRange(aStart.row, aStart.column, aEnd.row, aEnd.column,
                                        "as-err-no-matching-constructor", sigAttempt);
                    }
                    else
                    {
                        ParameterInformation dummyParam{baseName, baseName, "", ""};
                        int score = ScoreArgumentMatch(argTypes[0], dummyParam, ctx.request.symbolTable);
                        if (score >= 999)
                        {
                            const TSPoint aStart = ts_node_start_point(argListNode);
                            const TSPoint aEnd = ts_node_end_point(argListNode);
                            ctx.EmitAtRange(aStart.row, aStart.column, aEnd.row, aEnd.column,
                                            "as-err-no-implicit-conversion", argTypes[0], baseName);
                        }
                    }
                    continue;
                }

                // Look up constructors
                std::vector<Symbol> rawConstructors;
                auto found = ctx.request.symbolTable.FindSymbols(baseName + "::" + baseName);
                for (const auto &s : found)
                {
                    if (s.type == SymbolType::Function)
                    {
                        rawConstructors.push_back(s);
                    }
                }
                if (rawConstructors.empty())
                {
                    auto found2 = ctx.request.symbolTable.FindSymbols(baseName);
                    for (const auto &s : found2)
                    {
                        if (s.type == SymbolType::Function)
                        {
                            rawConstructors.push_back(s);
                        }
                    }
                }

                if (rawConstructors.empty())
                {
                    // Knowing the type exists is not the same as being able to see its
                    // constructors, and only the second justifies complaining about one.
                    //
                    // `array` is known because TypeConfig names it, and `string` because the engine
                    // registers it - neither declares a constructor anywhere this analyzer can
                    // read. Treating "known" as "fully visible" made `array<int> a(10)` report
                    // "No matching signatures to 'array<int>(int)'" on correct code, which the real
                    // compiler accepts without a word. That is the exact failure mode the
                    // silent-unless-fully-visible policy exists to prevent.
                    //
                    // A class declared in *script* is different: if it declares no constructor at
                    // all, only the implicit no-argument one exists, and a call passing arguments
                    // really is wrong.
                    //
                    // A class declared in a predefined stub is not. Its factories are registered in
                    // C++ and the stub is under no obligation to repeat them - AS-Harness's own
                    // as.predefined declares `class array<T>` with no constructor whatsoever, while
                    // the engine registers three. Seeing that declaration says the type exists; it
                    // says nothing about how many ways there are to build one.
                    bool declarationVisible = false;
                    if (const auto declarations = ctx.request.symbolTable.FindSymbolsPtr(baseName))
                    {
                        for (const auto &declaration : *declarations)
                        {
                            if (declaration.type == SymbolType::Class &&
                                std::holds_alternative<ClassSignature>(declaration.signature) &&
                                !utils::IsPredefinedFile(declaration.fileUri,
                                                         ctx.request.predefinedFileExtension))
                            {
                                declarationVisible = true;
                                break;
                            }
                        }
                    }

                    if (declarationVisible)
                    {
                        const TSPoint aStart = ts_node_start_point(argListNode);
                        const TSPoint aEnd = ts_node_end_point(argListNode);
                        std::string sigAttempt = declaredType + "(";
                        for (size_t a = 0; a < argTypes.size(); ++a)
                        {
                            if (a > 0) sigAttempt += ", ";
                            sigAttempt += argTypes[a];
                        }
                        sigAttempt += ")";
                        ctx.EmitAtRange(aStart.row, aStart.column, aEnd.row, aEnd.column,
                                        "as-err-no-matching-constructor", sigAttempt);
                    }
                    continue;
                }

                // Template specialization
                std::vector<Symbol> candidates;
                if (!tmplInfo.templateArgs.empty())
                {
                    std::vector<std::string> templateParams;
                    auto classSymbols = ctx.request.symbolTable.FindSymbols(baseName);
                    for (const auto &cs : classSymbols)
                    {
                        if (cs.type == SymbolType::Class && std::holds_alternative<ClassSignature>(cs.signature))
                        {
                            templateParams = cs.GetClass().templateParams;
                            break;
                        }
                    }
                    if (templateParams.empty())
                    {
                        templateParams.push_back("T");
                    }

                    for (const auto &sym : rawConstructors)
                    {
                        Symbol specSym = sym;
                        if (std::holds_alternative<FunctionSignature>(specSym.signature))
                        {
                            auto &fn = specSym.GetFunction();
                            for (auto &param : fn.parameters)
                            {
                                for (size_t t = 0; t < templateParams.size() && t < tmplInfo.templateArgs.size(); ++t)
                                {
                                    const std::string &paramName = templateParams[t];
                                    const std::string &concreteArg = tmplInfo.templateArgs[t];
                                    if (param.baseTypeName == paramName)
                                    {
                                        param.baseTypeName = concreteArg;
                                    }
                                    size_t pos = 0;
                                    while ((pos = param.typeName.find(paramName, pos)) != std::string::npos)
                                    {
                                        bool beforeOk = (pos == 0 || !isalnum(static_cast<unsigned char>(param.typeName[pos - 1])));
                                        bool afterOk = (pos + paramName.size() >= param.typeName.size() || !isalnum(static_cast<unsigned char>(param.typeName[pos + paramName.size()])));
                                        if (beforeOk && afterOk)
                                        {
                                            param.typeName.replace(pos, paramName.size(), concreteArg);
                                            pos += concreteArg.size();
                                        }
                                        else
                                        {
                                            pos += paramName.size();
                                        }
                                    }
                                }
                            }
                        }
                        candidates.push_back(std::move(specSym));
                    }
                }
                else
                {
                    candidates = rawConstructors;
                }

                // Check arity
                std::vector<Symbol> matchingArityCandidates;
                for (const auto &sym : candidates)
                {
                    if (std::holds_alternative<FunctionSignature>(sym.signature))
                    {
                        const Arity arity = ArityOf(sym.GetFunction());
                        if (arity.variadic || (argCount >= arity.required && argCount <= arity.maximum))
                        {
                            matchingArityCandidates.push_back(sym);
                        }
                    }
                }

                if (matchingArityCandidates.empty())
                {
                    const TSPoint aStart = ts_node_start_point(argListNode);
                    const TSPoint aEnd = ts_node_end_point(argListNode);
                    std::string sigAttempt = declaredType + "(";
                    for (size_t a = 0; a < argTypes.size(); ++a)
                    {
                        if (a > 0) sigAttempt += ", ";
                        sigAttempt += argTypes[a];
                    }
                    sigAttempt += ")";
                    ctx.EmitAtRange(aStart.row, aStart.column, aEnd.row, aEnd.column,
                                    "as-err-no-matching-constructor", sigAttempt);
                    continue;
                }

                // Score matching arity overloads
                auto match = ResolveBestOverload(matchingArityCandidates, argTypes, ctx.request.symbolTable);
                if (match.bestScore >= 999 || match.bestCandidate == nullptr)
                {
                    const TSPoint aStart = ts_node_start_point(argListNode);
                    const TSPoint aEnd = ts_node_end_point(argListNode);
                    std::string sigAttempt = declaredType + "(";
                    for (size_t a = 0; a < argTypes.size(); ++a)
                    {
                        if (a > 0) sigAttempt += ", ";
                        sigAttempt += argTypes[a];
                    }
                    sigAttempt += ")";
                    ctx.EmitAtRange(aStart.row, aStart.column, aEnd.row, aEnd.column,
                                    "as-err-no-matching-constructor", sigAttempt);
                    continue;
                }
            }
        }

        void VisitNode(TSNode node, const CallCheckRequest &request, DiagnosticContext &ctx, int depth = 0)
                {
            // Pathologically nested source would otherwise recurse until the stack gives out; see
            // k_maxAstDepth in ASTUtils.h.
            if (depth > k_maxAstDepth)
                return;

            std::string_view nodeType = ts_node_type(node);
            if (nodeType == "call_expression")
            {
                const TSPoint start = ts_node_start_point(node);
                CheckCall(node, request,
                          FindInnermostScope(request.scopeRoot, start.row, start.column), ctx);
            }
            else if (nodeType == "variable_declaration")
            {
                CheckVariableDirectInitialization(node, request, ctx);
            }

            const uint32_t childCount = ts_node_named_child_count(node);
            for (uint32_t i = 0; i < childCount; ++i)
            {
                VisitNode(ts_node_named_child(node, i), request, ctx, depth + 1);
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
