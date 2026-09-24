#include "analysis/CallChecker.h"
#include "analysis/ASTUtils.h"
#include "analysis/DiagnosticCodes.h"
#include "analysis/InitializerListChecker.h"
#include "analysis/NodeIndex.h"
#include "analysis/OverloadResolver.h"
#include "analysis/SemanticHelpers.h"
#include "utils/Utils.h"

#include "parser/GrammarNames.h"
#include "parser/Keywords.h"
#include <algorithm>
#include <cctype>
#include <optional>
#include <span>
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
 *
 * @param[in] node AST node to read.
 * @param[in] sourceCode Document source text.
 * @return Owning string of the node text.
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

/**
 * @brief Trims leading and trailing whitespace in place.
 *
 * @param[in,out] s String to trim.
 */
void TrimString(std::string& s)
{
    while (!s.empty() && isspace(static_cast<unsigned char>(s.front())))
    {
        s.erase(s.begin());
    }
    while (!s.empty() && isspace(static_cast<unsigned char>(s.back())))
    {
        s.pop_back();
    }
}

/**
 * @brief Counts the arguments written between one call's parentheses.
 *
 * Counted from the separators rather than from the named children, because a named argument
 * (`Take(a: 1)`) contributes both its name and its value as named children and would count
 * twice. `Take(void)` is one argument too - AngelScript's spelling of "discard this &out" -
 * and it is an anonymous token, so an empty list is only an empty one when nothing at all
 * stands between the parentheses.
 *
 * @param[in] argumentList AST node representing the argument list.
 * @return Count of written arguments.
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

/**
 * @brief Extracts argument expression nodes from an argument list AST node.
 *
 * @param[in] argumentList AST node representing the argument list.
 * @return Vector of AST nodes for each argument expression.
 */
/**
 * @brief Selects the argument expression node from a comma-separated argument group.
 *
 * If the group contains a colon ':' (named argument, e.g. `name: expr`), the expression
 * following the colon is returned. Otherwise, the primary expression node is returned.
 *
 * @param[in] group AST nodes within one argument position.
 * @return TSNode representing the argument value expression.
 */
TSNode ExtractArgumentExpression(const std::vector<TSNode>& group)
{
    if (group.empty())
    {
        return TSNode{};
    }
    for (size_t i = 0; i < group.size(); ++i)
    {
        if (std::string_view(ts_node_type(group[i])) == ":" && i + 1 < group.size())
        {
            return group[i + 1];
        }
    }
    return group.front();
}

/**
 * @brief Extracts argument expression nodes from an argument list AST node.
 *
 * @param[in] argumentList AST node representing the argument list.
 * @return Vector of AST nodes for each argument expression.
 */
std::vector<TSNode> GetArgumentNodes(TSNode argumentList)
{
    std::vector<TSNode> argNodes;
    if (ts_node_is_null(argumentList))
    {
        return argNodes;
    }

    const uint32_t count = ts_node_child_count(argumentList);
    std::vector<TSNode> currentGroup;
    for (uint32_t i = 0; i < count; ++i)
    {
        TSNode child = ts_node_child(argumentList, i);
        const std::string_view childType = ts_node_type(child);
        if (childType == "(" || childType == ")" || childType == "comment")
        {
            continue;
        }
        if (childType == ",")
        {
            if (!currentGroup.empty())
            {
                argNodes.push_back(ExtractArgumentExpression(currentGroup));
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
        argNodes.push_back(ExtractArgumentExpression(currentGroup));
    }
    return argNodes;
}

/**
 * @brief Extracts the parameter name from a named argument token group (e.g. `name: expr`).
 * @param[in] group AST nodes within one argument position.
 * @param[in] sourceCode Document source text.
 * @return Argument identifier name if named argument, otherwise empty string.
 */
std::string ExtractArgumentName(const std::vector<TSNode>& group, std::string_view sourceCode)
{
    for (size_t i = 0; i < group.size(); ++i)
    {
        if (std::string_view(ts_node_type(group[i])) == ":" && i > 0)
        {
            std::string name = NodeText(group[i - 1], sourceCode);
            TrimString(name);
            return name;
        }
    }
    return "";
}

/**
 * @brief Extracts parameter names for each argument in an argument list AST node.
 * @param[in] argumentList AST node representing the argument list.
 * @param[in] sourceCode Document source text.
 * @return Vector of argument names (empty string for positional arguments).
 */
std::vector<std::string> GetArgumentNames(TSNode argumentList, std::string_view sourceCode)
{
    std::vector<std::string> argNames;
    if (ts_node_is_null(argumentList))
    {
        return argNames;
    }

    const uint32_t count = ts_node_child_count(argumentList);
    std::vector<TSNode> currentGroup;
    for (uint32_t i = 0; i < count; ++i)
    {
        TSNode child = ts_node_child(argumentList, i);
        const std::string_view childType = ts_node_type(child);
        if (childType == "(" || childType == ")" || childType == "comment")
        {
            continue;
        }
        if (childType == ",")
        {
            if (!currentGroup.empty())
            {
                argNames.push_back(ExtractArgumentName(currentGroup, sourceCode));
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
        argNames.push_back(ExtractArgumentName(currentGroup, sourceCode));
    }
    return argNames;
}

/** @brief What one declaration will accept, in argument counts. */
struct Arity
{
    uint32_t required = 0;
    uint32_t maximum = 0;
    bool variadic = false;
};

/**
 * @brief Calculates arity boundaries of a function signature.
 *
 * @param[in] sig Function signature to inspect.
 * @return Calculated Arity boundaries.
 */
Arity ArityOf(const FunctionSignature& sig)
{
    Arity arity;
    for (const auto& param : sig.parameters)
    {
        // `...` reaches the collector as a parameter with nothing in it but its own text.
        if (param.rawText.find("...") != std::string::npos || param.typeName.find("...") != std::string::npos)
        {
            arity.variadic = true;
            continue;
        }

        ++arity.maximum;
        if (param.defaultValue.empty() && param.rawText.find('=') == std::string::npos)
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
 *
 * @param[in] name Identifier name.
 * @param[in] table Symbol table.
 * @return True if the name denotes a type.
 */
bool NamesAType(const std::string& name, const SymbolTable& table)
{
    if (IsCorePrimitive(name))
    {
        return true;
    }
    if (const auto symbols = table.FindSymbolsPtr(name))
    {
        if (std::any_of(symbols->begin(), symbols->end(),
                        [](const Symbol& sym)
                        {
                            return sym.type == SymbolType::Class || sym.type == SymbolType::Interface ||
                                   sym.type == SymbolType::Funcdef || sym.type == SymbolType::Enum ||
                                   sym.type == SymbolType::Typedef;
                        }))
        {
            return true;
        }
    }
    const std::string shortName = LastScopeSegment(name);
    const auto matches = table.FindTypeSymbolsByShortName(shortName);
    return !matches.empty();
}

/**
 * @brief True when a symbol represents a function with a valid signature.
 *
 * @param[in] sym Symbol to check.
 * @return True if the symbol is a function.
 */
bool IsFunctionSymbol(const Symbol& sym)
{
    return sym.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(sym.signature);
}

/** @brief What the pass concluded about one call's candidates. */
struct CandidateSet
{
    bool decided = false; ///< False means stay silent: nothing visible to judge against.
    bool accepts = false; ///< True when some candidate takes the written argument count.
};

/**
 * @brief Evaluates whether candidate set accepts an argument count.
 *
 * @param[in] candidates Available function candidates.
 * @param[in] argumentCount Number of arguments provided.
 * @return CandidateSet verdict.
 */
CandidateSet JudgeAgainst(const std::vector<Symbol>& candidates, uint32_t argumentCount)
{
    CandidateSet result;
    for (const auto& sym : candidates)
    {
        result.decided = true;

        const Arity arity = ArityOf(sym.GetFunction());
        if (argumentCount >= arity.required && (arity.variadic || argumentCount <= arity.maximum))
        {
            result.accepts = true;
            return result;
        }
    }
    return result;
}

/**
 * @brief True when a member name is its own class's constructor or destructor.
 *
 * Matched by name because that is the convention the analyzer uses throughout - a
 * constructor is an ordinary Function stored under `Class::Class` (see the constructor
 * lookup further down, which relies on the same thing). The type may arrive qualified, so
 * the comparison is against its last `::` segment.
 *
 * @param[in] memberName Member identifier text.
 * @param[in] typeName Owner type name.
 * @return True if name denotes constructor or destructor.
 */
bool IsConstructorOrDestructorName(const std::string& memberName, const std::string& typeName)
{
    if (memberName.empty() || typeName.empty())
    {
        return false;
    }

    const size_t at = typeName.rfind("::");
    const std::string_view shortName =
        at == std::string::npos ? std::string_view(typeName) : std::string_view(typeName).substr(at + 2);

    if (memberName == shortName)
    {
        return true;
    }

    return memberName.front() == '~' && std::string_view(memberName).substr(1) == shortName;
}

/**
 * @brief Declarations of a method, across a type's whole visible hierarchy.
 *
 * @param[in] typeName Name of the class type.
 * @param[in] methodName Name of the method.
 * @param[in] table Symbol table.
 * @return Candidate symbols matching the method name.
 */
std::vector<Symbol> FindMethodCandidates(const std::string& typeName, const std::string& methodName,
                                         const SymbolTable& table)
{
    std::vector<Symbol> candidates;
    for (const auto& owner : GetInheritedTypeHierarchy(typeName, table))
    {
        const auto found = table.FindSymbolsPtr(owner + "::" + methodName);
        if (!found)
        {
            continue;
        }
        for (const auto& sym : *found)
        {
            if (!IsFunctionSymbol(sym))
            {
                continue;
            }

            const bool overriddenLower =
                std::any_of(candidates.begin(), candidates.end(),
                            [&sym](const Symbol& kept)
                            {
                                return HasSameParameterList(kept, sym) &&
                                       kept.GetFunction().modifiers.isConst == sym.GetFunction().modifiers.isConst;
                            });
            if (!overriddenLower)
            {
                candidates.push_back(sym);
            }
        }
    }
    return candidates;
}

/** @brief Context bundled for free candidate lookup. */
struct FreeLookupContext
{
    TSNode callNode;
    std::string_view sourceCode;
    const std::string& fileUri;
    const std::string& predefinedExtension;
    const SymbolTable& table;
    const rules::RuleIndex& index;
};

/**
 * @brief Collects enclosing namespace scopes up to global scope.
 *
 * @param[in] callNode AST node of call expression.
 * @param[in] sourceCode Document source text.
 * @return Ordered list of reachable namespace scopes.
 */
std::vector<std::string> CollectReachableScopes(TSNode callNode, std::string_view sourceCode)
{
    std::vector<std::string> reachableScopes;
    for (const auto& container : GetEnclosingContainers(callNode, sourceCode))
    {
        if (container.kind == ContainerKind::Class || container.kind == ContainerKind::Interface)
        {
            continue;
        }
        if (container.kind == ContainerKind::Namespace)
        {
            reachableScopes.push_back(container.qualifiedName);
        }
    }
    reachableScopes.emplace_back();
    return reachableScopes;
}

/**
 * @brief Collects function symbols matching name from a specific scope.
 *
 * @param[in] scopeName Name of scope or empty for global.
 * @param[in] name Unqualified symbol name.
 * @param[in] ctx Lookup context.
 * @param[in,out] candidates Destination vector for collected symbols.
 */
void CollectScopeCandidates(const std::string& scopeName, const std::string& name, const FreeLookupContext& ctx,
                            std::vector<Symbol>& candidates)
{
    const std::string key = scopeName.empty() ? name : scopeName + "::" + name;
    const auto found = ctx.table.FindSymbolsPtr(key);
    if (!found)
    {
        return;
    }

    for (const auto& sym : *found)
    {
        if (IsFunctionSymbol(sym) &&
            (sym.fileUri == ctx.fileUri || utils::IsPredefinedFile(sym.fileUri, ctx.predefinedExtension)))
        {
            candidates.push_back(sym);
        }
    }
}

/**
 * @brief Collects candidates from using namespace directives at document root.
 *
 * @param[in] name Unqualified function name.
 * @param[in] reachableScopes Already visited lexical scopes.
 * @param[in] ctx Lookup context.
 * @param[in,out] candidates Destination vector for collected symbols.
 */
void CollectUsingNamespaceCandidates(const std::string& name, const std::vector<std::string>& reachableScopes,
                                     const FreeLookupContext& ctx, std::vector<Symbol>& candidates)
{
    TSNode documentRoot = ctx.callNode;
    while (!ts_node_is_null(ts_node_parent(documentRoot)))
    {
        documentRoot = ts_node_parent(documentRoot);
    }

    for (const auto& imported : CollectUsingNamespaces(documentRoot, ctx.sourceCode))
    {
        if (!imported.empty() &&
            std::find(reachableScopes.begin(), reachableScopes.end(), imported) == reachableScopes.end())
        {
            CollectScopeCandidates(imported, name, ctx, candidates);
        }
    }
}

/**
 * @brief Declarations an unqualified call can reach, when that question is answerable.
 *
 * @param[in] name Function identifier name.
 * @param[in] ctx Free candidate lookup context.
 * @return Reachable function candidate symbols.
 */
std::vector<Symbol> FindFreeCandidates(const std::string& name, const FreeLookupContext& ctx)
{
    std::vector<Symbol> candidates;
    if (!ctx.index.allNames.contains(name))
    {
        return candidates;
    }

    const std::vector<std::string> reachableScopes = CollectReachableScopes(ctx.callNode, ctx.sourceCode);
    for (const auto& scopeName : reachableScopes)
    {
        CollectScopeCandidates(scopeName, name, ctx, candidates);
        if (!candidates.empty())
        {
            return candidates;
        }
    }

    CollectUsingNamespaceCandidates(name, reachableScopes, ctx, candidates);
    return candidates;
}

/** @brief Context bundled for lambda argument verification. */
struct LambdaCheckContext
{
    TSNode callee;
    TSNode arguments;
    std::string reportedName;
    const SymbolTable& table;
    std::string_view sourceCode;
    const std::vector<TSNode>& argNodes;
    std::vector<size_t> lambdaPositions;
};

enum class LambdaCandidateStatus
{
    Accepted,
    Rejected,
    UnresolvableFuncdef
};

/**
 * @brief Finds indices of arguments that are lambda expressions.
 *
 * @param[in] argNodes List of argument AST nodes.
 * @return 0-based indices where arguments are lambdas.
 */
std::vector<size_t> FindLambdaPositions(const std::vector<TSNode>& argNodes)
{
    std::vector<size_t> positions;
    for (size_t i = 0; i < argNodes.size(); ++i)
    {
        if (IsLambdaExpression(argNodes[i]))
        {
            positions.push_back(i);
        }
    }
    return positions;
}

/**
 * @brief Evaluates whether candidate accepts all lambda arguments.
 *
 * @param[in] candidate Function candidate to check.
 * @param[in] lctx Lambda verification context.
 * @param[out] shape Collected funcdef names for matched lambda parameters.
 * @return Status of match.
 */
LambdaCandidateStatus EvaluateCandidateLambdaShape(const Symbol& candidate, const LambdaCheckContext& lctx,
                                                   std::vector<std::string>& shape)
{
    const auto& fn = candidate.GetFunction();
    for (const size_t position : lctx.lambdaPositions)
    {
        if (position >= fn.parameters.size())
        {
            return LambdaCandidateStatus::Rejected;
        }

        const std::string parameterType = CleanBaseType(fn.parameters[position].typeName);
        const auto funcdef = FindFuncdefSymbol(parameterType, lctx.table);
        if (!funcdef)
        {
            return LambdaCandidateStatus::UnresolvableFuncdef;
        }
        if (LambdaContradictsFuncdef(lctx.argNodes[position], funcdef->GetFuncdef(), lctx.table, lctx.sourceCode))
        {
            return LambdaCandidateStatus::Rejected;
        }
        shape.push_back(funcdef->name);
    }
    return LambdaCandidateStatus::Accepted;
}

/**
 * @brief Emits diagnostics for lambda shape matching results.
 *
 * @param[in] acceptedShapes Matrix of accepted funcdef shapes per viable candidate.
 * @param[in] lctx Lambda verification context.
 * @param[in,out] ctx Diagnostic context.
 */
void ReportLambdaDiagnostics(const std::vector<std::vector<std::string>>& acceptedShapes,
                             const LambdaCheckContext& lctx, DiagnosticContext& ctx)
{
    const TSPoint start = ts_node_start_point(lctx.callee);
    const TSPoint end = ts_node_end_point(lctx.arguments);

    if (acceptedShapes.empty())
    {
        ctx.EmitAtRange({start.row, start.column, end.row, end.column}, "as-err-call-no-matching-signature",
                        lctx.reportedName);
        return;
    }

    const bool everyShapeIdentical =
        std::all_of(acceptedShapes.begin(), acceptedShapes.end(),
                    [&](const std::vector<std::string>& shape) { return shape == acceptedShapes.front(); });
    if (acceptedShapes.size() > 1 && !everyShapeIdentical)
    {
        ctx.EmitAtRange({start.row, start.column, end.row, end.column}, "as-err-call-ambiguous", lctx.reportedName);
    }
}

/**
 * @brief Judges a lambda argument against the funcdef parameter it lands on.
 *
 * @param[in] candidates Candidate function overloads.
 * @param[in] lctx Lambda verification context.
 * @param[in,out] ctx Diagnostic context.
 */
void CheckLambdaArguments(std::span<const Symbol* const> candidates, const LambdaCheckContext& lctx,
                          DiagnosticContext& ctx)
{
    if (lctx.lambdaPositions.empty() || candidates.empty())
    {
        return;
    }

    std::vector<std::vector<std::string>> acceptedShapes;
    for (const auto* candidate : candidates)
    {
        if (!candidate)
        {
            continue;
        }
        std::vector<std::string> shape;
        const auto status = EvaluateCandidateLambdaShape(*candidate, lctx, shape);
        if (status == LambdaCandidateStatus::UnresolvableFuncdef)
        {
            return;
        }
        if (status == LambdaCandidateStatus::Accepted)
        {
            acceptedShapes.push_back(std::move(shape));
        }
    }

    ReportLambdaDiagnostics(acceptedShapes, lctx, ctx);
}

/** @brief Context bundled for overall call validation. */
struct CallValidationContext
{
    TSNode callNode;
    TSNode callee;
    TSNode arguments;
    const CallCheckRequest& request;
    const Scope* scope;
    DiagnosticContext& ctx;
};

/** @brief Result of callee resolution stage. */
struct CalleeResolution
{
    std::vector<Symbol> candidates;
    std::string reportedName;
    bool candidatesAreFreeFunctions = false;
    bool isUnqualifiedClassCall = false;
    bool isReceiverConst = false;
    bool shouldCheck = true;
};

/** @brief Resolved object type and template arguments for member calls. */
struct ObjectTypeInfo
{
    std::string objectType;
    std::vector<std::string> templateArgs;
    bool isConst = false;
};

/**
 * @brief Checks whether an object expression node resolves to a const variable or property.
 *
 * @param[in] objectNode AST node of object expression.
 * @param[in] valCtx Call validation context.
 * @return True if the object node is const.
 */
bool IsObjectNodeConst(TSNode objectNode, const CallValidationContext& valCtx)
{
    const std::string_view nodeType = ts_node_type(objectNode);
    if (nodeType != "identifier" && nodeType != "scoped_identifier")
    {
        return false;
    }

    const std::string name = NodeText(objectNode, valCtx.request.sourceCode);
    if (valCtx.scope)
    {
        if (const auto* def = ResolveInScope(valCtx.scope, name))
        {
            if (!def->typeName.empty() && HasConstModifier(def->typeName))
            {
                return true;
            }
        }
    }

    if (const auto syms = valCtx.ctx.request.symbolTable.FindSymbolsPtr(name))
    {
        for (const auto& sym : *syms)
        {
            if ((sym.type == SymbolType::Variable || sym.type == SymbolType::Property) &&
                sym.GetVariable().modifiers.isConst)
            {
                return true;
            }
        }
    }
    return false;
}

/**
 * @brief Resolves object type and template arguments from member expression.
 *
 * @param[in] objectNode AST node of object expression.
 * @param[in] valCtx Call validation context.
 * @return Resolved ObjectTypeInfo.
 */
ObjectTypeInfo ResolveMemberObjectType(TSNode objectNode, const CallValidationContext& valCtx)
{
    ObjectTypeInfo info;
    const std::string rawObjType = CanonicalizeArrayType(
        ResolveExpressionType(objectNode, {valCtx.scope, valCtx.ctx.request.symbolTable, valCtx.request.sourceCode,
                                           valCtx.ctx.request.fileUri}),
        valCtx.ctx.request.GetArrayTypeName().empty() ? "array" : valCtx.ctx.request.GetArrayTypeName());
    info.objectType = CleanBaseType(rawObjType);
    info.isConst = rawObjType.starts_with("const ") || rawObjType.ends_with("const") || HasConstModifier(rawObjType) ||
                   IsObjectNodeConst(objectNode, valCtx);

    if (rawObjType.find('<') != std::string::npos && rawObjType.ends_with('>'))
    {
        const size_t openBracket = rawObjType.find('<');
        std::string tmplName = rawObjType.substr(0, openBracket);
        TrimString(tmplName);
        if (valCtx.ctx.request.symbolTable.FindSymbolsPtr(tmplName))
        {
            info.objectType = tmplName;
            std::string argStr = rawObjType.substr(openBracket + 1, rawObjType.size() - openBracket - 2);
            info.templateArgs.push_back(std::move(argStr));
        }
    }
    return info;
}

/**
 * @brief Replaces template parameter occurrences in a function signature.
 *
 * @param[in,out] fn Signature to mutate.
 * @param[in] paramName Template parameter name (e.g. "T").
 * @param[in] concreteType Concrete type name to substitute.
 */
void SubstituteFunctionTemplateParams(FunctionSignature& fn, const std::string& paramName,
                                      const std::string& concreteType)
{
    fn.returnType = SubstituteTypeParam(fn.returnType, paramName, concreteType);
    for (auto& p : fn.parameters)
    {
        p.typeName = SubstituteTypeParam(p.typeName, paramName, concreteType);
        p.baseTypeName = SubstituteTypeParam(p.baseTypeName, paramName, concreteType);
    }
}

/**
 * @brief Applies template argument substitutions across candidate methods.
 *
 * @param[in,out] candidates Candidate symbols to specialize.
 * @param[in] typeName Owner type name.
 * @param[in] fallbackArgs Fallback template arguments when generic binding is absent.
 * @param[in] table Symbol table.
 */
void ApplyTemplateSubstitutions(std::vector<Symbol>& candidates, const std::string& typeName,
                                const std::vector<std::string>& fallbackArgs, const SymbolTable& table)
{
    const auto binding = BindTemplateArguments(typeName, table);
    if (binding.usable)
    {
        for (auto& sym : candidates)
        {
            if (sym.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(sym.signature))
            {
                auto fn = sym.GetFunction();
                for (size_t i = 0; i < binding.parameters.size(); ++i)
                {
                    SubstituteFunctionTemplateParams(fn, binding.parameters[i], binding.arguments[i]);
                }
                sym.signature = fn;
            }
        }
    }
    else if (!fallbackArgs.empty())
    {
        for (auto& sym : candidates)
        {
            if (sym.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(sym.signature))
            {
                auto fn = sym.GetFunction();
                for (auto& p : fn.parameters)
                {
                    p.typeName = SubstituteTypeParam(p.typeName, "T", fallbackArgs[0]);
                    p.baseTypeName = SubstituteTypeParam(p.baseTypeName, "T", fallbackArgs[0]);
                }
                sym.signature = fn;
            }
        }
    }
}

/**
 * @brief Filters method candidates based on the constness of the receiver.
 *
 * A const receiver can only call const methods. A mutable receiver prefers
 * non-const methods over const methods when parameter signatures are identical.
 *
 * @param[in,out] candidates Candidate symbols to filter.
 * @param[in] isReceiverConst Whether the receiver object is const.
 */
void FilterMethodCandidatesConstness(std::vector<Symbol>& candidates, bool isReceiverConst)
{
    if (isReceiverConst)
    {
        const bool hasConst = std::any_of(candidates.begin(), candidates.end(),
                                          [](const Symbol& sym)
                                          {
                                              return sym.type == SymbolType::Function &&
                                                     std::holds_alternative<FunctionSignature>(sym.signature) &&
                                                     sym.GetFunction().modifiers.isConst;
                                          });
        if (hasConst)
        {
            std::erase_if(candidates,
                          [](const Symbol& sym)
                          {
                              return sym.type == SymbolType::Function &&
                                     std::holds_alternative<FunctionSignature>(sym.signature) &&
                                     !sym.GetFunction().modifiers.isConst;
                          });
        }
        else
        {
            candidates.clear();
        }
    }
    else
    {
        std::erase_if(candidates,
                      [&candidates](const Symbol& sym)
                      {
                          if (sym.type != SymbolType::Function ||
                              !std::holds_alternative<FunctionSignature>(sym.signature) ||
                              !sym.GetFunction().modifiers.isConst)
                          {
                              return false;
                          }
                          return std::any_of(candidates.begin(), candidates.end(),
                                             [&sym](const Symbol& other)
                                             {
                                                 return &sym != &other && other.type == SymbolType::Function &&
                                                        std::holds_alternative<FunctionSignature>(other.signature) &&
                                                        !other.GetFunction().modifiers.isConst &&
                                                        HasSameParameterList(sym, other);
                                             });
                      });
    }
}

/**
 * @brief Checks if a node is enclosed within a const method declaration.
 *
 * @param[in] node AST node.
 * @param[in] sourceCode Document source text.
 * @return True if enclosed within a const method.
 */
bool IsEnclosingMethodConst(TSNode node, std::string_view sourceCode)
{
    TSNode curr = node;
    while (!ts_node_is_null(curr))
    {
        if (std::string_view(ts_node_type(curr)) == parser::nodes::FuncDeclaration)
        {
            const uint32_t childCount = ts_node_named_child_count(curr);
            for (uint32_t i = 0; i < childCount; ++i)
            {
                TSNode child = ts_node_named_child(curr, i);
                if (std::string_view(ts_node_type(child)) == parser::nodes::FuncAttributes)
                {
                    return NodeText(child, sourceCode).find("const") != std::string::npos;
                }
            }
            return false;
        }
        curr = ts_node_parent(curr);
    }
    return false;
}

/**
 * @brief Resolves candidates and reports invalid constructors for member calls.
 *
 * @param[in] valCtx Call validation context.
 * @return Resolved CalleeResolution.
 */
CalleeResolution ResolveMemberCallee(const CallValidationContext& valCtx)
{
    CalleeResolution res;
    TSNode objectNode = parser::GetChildByField(valCtx.callee, parser::fields::Object);
    TSNode memberNode = parser::GetChildByField(valCtx.callee, parser::fields::Member);
    if (ts_node_is_null(objectNode) || ts_node_is_null(memberNode))
    {
        res.shouldCheck = false;
        return res;
    }

    const auto objInfo = ResolveMemberObjectType(objectNode, valCtx);
    if (objInfo.objectType.empty() || !HierarchyIsFullyVisible(objInfo.objectType, valCtx.ctx.request.symbolTable))
    {
        res.shouldCheck = false;
        return res;
    }

    res.reportedName = NodeText(memberNode, valCtx.request.sourceCode);
    if (IsConstructorOrDestructorName(res.reportedName, objInfo.objectType))
    {
        const TSPoint ctorStart = ts_node_start_point(memberNode);
        const TSPoint ctorEnd = ts_node_end_point(memberNode);
        valCtx.ctx.EmitAtRange({ctorStart.row, ctorStart.column, ctorEnd.row, ctorEnd.column},
                               "as-err-constructor-not-callable", res.reportedName, objInfo.objectType);
        res.shouldCheck = false;
        return res;
    }

    res.isReceiverConst = objInfo.isConst;
    res.candidates = FindMethodCandidates(objInfo.objectType, res.reportedName, valCtx.ctx.request.symbolTable);
    FilterMethodCandidatesConstness(res.candidates, res.isReceiverConst);
    ApplyTemplateSubstitutions(res.candidates, objInfo.objectType, objInfo.templateArgs,
                               valCtx.ctx.request.symbolTable);
    return res;
}

/**
 * @brief Checks whether an identifier is shadowed by a local/parameter or names a type.
 *
 * @param[in] shortName Unqualified identifier.
 * @param[in] scope Lexical scope.
 * @param[in] table Symbol table.
 * @return True if shadowed or names a type.
 */
bool IsShadowedOrTypeName(const std::string& shortName, const Scope* scope, const SymbolTable& table)
{
    if (scope)
    {
        const LocalDefinition* shadow = ResolveInScope(scope, shortName);
        if (shadow && (shadow->kind == LocalDefinitionKind::Variable || shadow->kind == LocalDefinitionKind::Parameter))
        {
            return true;
        }
    }
    return NamesAType(shortName, table);
}

/**
 * @brief Finds enclosing class or interface name if inside one.
 *
 * @param[in] node AST node.
 * @param[in] sourceCode Document source text.
 * @return Enclosing class name or empty string.
 */
std::string FindEnclosingClassOrInterface(TSNode node, std::string_view sourceCode)
{
    for (const auto& container : GetEnclosingContainers(node, sourceCode))
    {
        if (container.kind == ContainerKind::Class || container.kind == ContainerKind::Interface)
        {
            return container.qualifiedName.empty() ? container.name : container.qualifiedName;
        }
    }
    return "";
}

/**
 * @brief Resolves candidates for identifier and scoped identifier callees.
 *
 * @param[in] valCtx Call validation context.
 * @return Resolved CalleeResolution.
 */
CalleeResolution ResolveIdentifierCallee(const CallValidationContext& valCtx)
{
    CalleeResolution res;
    const std::string written = NodeText(valCtx.callee, valCtx.request.sourceCode);
    if (written.empty())
    {
        res.shouldCheck = false;
        return res;
    }

    const std::string shortName = LastScopeSegment(written);
    if (IsShadowedOrTypeName(shortName, valCtx.scope, valCtx.ctx.request.symbolTable))
    {
        res.shouldCheck = false;
        return res;
    }

    res.reportedName = shortName;
    if (written.find("::") != std::string::npos)
    {
        if (const auto found = valCtx.ctx.request.symbolTable.FindSymbolsPtr(written))
        {
            for (const auto& sym : *found)
            {
                if (IsFunctionSymbol(sym))
                {
                    res.candidates.push_back(sym);
                }
            }
        }
        return res;
    }

    const std::string enclosingClass = FindEnclosingClassOrInterface(valCtx.callNode, valCtx.request.sourceCode);
    if (!enclosingClass.empty())
    {
        res.candidates = FindMethodCandidates(enclosingClass, written, valCtx.ctx.request.symbolTable);
        if (!res.candidates.empty())
        {
            res.candidatesAreFreeFunctions = false;
            res.isUnqualifiedClassCall = true;
            res.isReceiverConst = IsEnclosingMethodConst(valCtx.callNode, valCtx.request.sourceCode);
            FilterMethodCandidatesConstness(res.candidates, res.isReceiverConst);
            ApplyTemplateSubstitutions(res.candidates, enclosingClass, {}, valCtx.ctx.request.symbolTable);
            return res;
        }
    }

    res.candidatesAreFreeFunctions = true;
    const FreeLookupContext freeCtx{valCtx.callNode,
                                    valCtx.request.sourceCode,
                                    valCtx.ctx.request.fileUri,
                                    valCtx.ctx.request.predefinedFileExtension,
                                    valCtx.ctx.request.symbolTable,
                                    valCtx.ctx.request.GetRuleIndex()};
    res.candidates = FindFreeCandidates(written, freeCtx);
    return res;
}

/**
 * @brief Validates argument ordering to prevent positional arguments after named ones.
 *
 * @param[in] arguments AST arguments node.
 * @param[out] sawNamedArg True if at least one named argument was found.
 * @param[in,out] ctx Diagnostic context.
 * @return True if argument ordering is valid.
 */
bool ValidateArgumentOrdering(TSNode arguments, bool& sawNamedArg, DiagnosticContext& ctx)
{
    sawNamedArg = false;
    const uint32_t totalChildren = ts_node_child_count(arguments);
    std::vector<std::vector<TSNode>> argGroups;
    std::vector<TSNode> currentGroup;
    for (uint32_t i = 0; i < totalChildren; ++i)
    {
        TSNode child = ts_node_child(arguments, i);
        const std::string_view ct = ts_node_type(child);
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

    for (const auto& group : argGroups)
    {
        bool isNamed = false;
        for (const auto& token : group)
        {
            if (std::string_view(ts_node_type(token)) == ":")
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
                ctx.EmitAtRange({aStart.row, aStart.column, aEnd.row, aEnd.column},
                                "as-err-positional-after-named-arg");
            }
            return false;
        }
    }
    return true;
}

bool CheckArgIsLValue(TSNode argNode, std::string_view sourceCode, const Scope* scope, const SymbolTable& table);

/** @brief Argument nodes and resolved types for a call expression. */
struct CallArgTypes
{
    std::vector<TSNode> argNodes;
    std::vector<std::string> argTypes;
    std::vector<std::string> argNames;
    std::vector<bool> argIsLValue;
    bool allArgsResolved = true;
};

/**
 * @brief Resolves expression types for each call argument and flags bare types.
 *
 * @param[in] valCtx Call validation context.
 * @return Resolved CallArgTypes structure.
 */
CallArgTypes ResolveCallArguments(const CallValidationContext& valCtx)
{
    CallArgTypes result;
    result.argNodes = GetArgumentNodes(valCtx.arguments);
    result.argNames = GetArgumentNames(valCtx.arguments, valCtx.request.sourceCode);

    for (const auto& argNode : result.argNodes)
    {
        result.argIsLValue.push_back(
            CheckArgIsLValue(argNode, valCtx.request.sourceCode, valCtx.scope, valCtx.ctx.request.symbolTable));

        if (auto dataTypeName =
                IsBareDataType(argNode, valCtx.scope, valCtx.ctx.request.symbolTable, valCtx.request.sourceCode))
        {
            const TSPoint aStart = ts_node_start_point(argNode);
            const TSPoint aEnd = ts_node_end_point(argNode);
            valCtx.ctx.EmitAtRange({aStart.row, aStart.column, aEnd.row, aEnd.column},
                                   diagnostics::codes::ExpressionIsDataType, *dataTypeName);
            result.allArgsResolved = false;
            result.argTypes.push_back("");
            continue;
        }

        std::string argType = ResolveExpressionType(argNode, {valCtx.scope, valCtx.ctx.request.symbolTable,
                                                              valCtx.request.sourceCode, valCtx.ctx.request.fileUri});
        if (argType.empty())
        {
            result.allArgsResolved = false;
        }
        result.argTypes.push_back(std::move(argType));
    }
    return result;
}
} // namespace

std::vector<const Symbol*> FilterMatchingArityCandidates(const std::vector<Symbol>& candidates, uint32_t argumentCount)
{
    std::vector<const Symbol*> matching;
    matching.reserve(candidates.size());
    for (const auto& sym : candidates)
    {
        if (std::holds_alternative<FunctionSignature>(sym.signature))
        {
            const Arity arity = ArityOf(sym.GetFunction());
            if (argumentCount >= arity.required && (arity.variadic || argumentCount <= arity.maximum))
            {
                matching.push_back(&sym);
            }
        }
    }
    return matching;
}

namespace
{
/**
 * @brief Checks initializer list arguments against parameter target types.
 *
 * @param[in] argNodes Call argument AST nodes.
 * @param[in] fn Target function signature.
 * @param[in] valCtx Call validation context.
 */
void CheckInitializerListArgs(const std::vector<TSNode>& argNodes, const FunctionSignature& fn,
                              const CallValidationContext& valCtx)
{
    for (size_t i = 0; i < argNodes.size() && i < fn.parameters.size(); ++i)
    {
        if (NodeType(argNodes[i]) == "initializer_list")
        {
            CheckInitializerListAgainstType(argNodes[i], fn.parameters[i].typeName,
                                            {valCtx.request.sourceCode, valCtx.scope}, valCtx.ctx);
        }
    }
}

/**
 * @brief Checks malformed ternary expressions used as call arguments.
 *
 * @param[in] argNodes Argument AST nodes.
 * @param[in] argTypes Resolved argument types.
 * @param[in] candidates Available matching candidates.
 * @param[in] valCtx Call validation context.
 */
void CheckMalformedTernaryArgs(const std::vector<TSNode>& argNodes, const std::vector<std::string>& argTypes,
                               std::span<const Symbol* const> candidates, const CallValidationContext& valCtx)
{
    if (candidates.empty() || !candidates[0])
    {
        return;
    }
    const auto& fn = candidates[0]->GetFunction();

    for (size_t i = 0; i < argNodes.size() && i < argTypes.size() && i < fn.parameters.size(); ++i)
    {
        if (std::string_view(ts_node_type(argNodes[i])) != parser::nodes::TernaryExpression || !argTypes[i].empty())
        {
            continue;
        }

        const std::string expected = fn.parameters[i].typeName;
        TSNode consequence = parser::GetChildByField(argNodes[i], parser::fields::Consequence);
        TSNode alternative = parser::GetChildByField(argNodes[i], parser::fields::Alternative);
        std::string t1 = ResolveExpressionType(consequence, {valCtx.scope, valCtx.ctx.request.symbolTable,
                                                             valCtx.request.sourceCode, valCtx.ctx.request.fileUri});
        std::string t2 = ResolveExpressionType(alternative, {valCtx.scope, valCtx.ctx.request.symbolTable,
                                                             valCtx.request.sourceCode, valCtx.ctx.request.fileUri});

        std::string badType = (!t1.empty() && t1 != expected) ? t1 : t2;
        if (badType.empty())
        {
            badType = (!t2.empty() ? t2 : t1);
        }
        if (badType.empty())
        {
            badType = "unknown";
        }

        const TSPoint aStart = ts_node_start_point(argNodes[i]);
        const TSPoint aEnd = ts_node_end_point(argNodes[i]);
        valCtx.ctx.EmitAtRange({aStart.row, aStart.column, aEnd.row, aEnd.column}, "as-err-no-implicit-conversion",
                               badType, expected);
    }
}

/**
 * @brief Finds index of first parameter failing conversion in a candidate.
 *
 * @param[in] candidate Candidate function.
 * @param[in] argTypes Argument types.
 * @param[in] table Symbol table.
 * @return Index of first mismatched argument or argTypes.size().
 */
size_t FindCandidateBadArgument(const Symbol& candidate, const std::vector<std::string>& argTypes,
                                const SymbolTable& table)
{
    const auto& fn = candidate.GetFunction();
    for (size_t i = 0; i < argTypes.size() && i < fn.parameters.size(); ++i)
    {
        if (!EvaluateArgumentConversion(argTypes[i], fn.parameters[i], table).IsViable())
        {
            return i;
        }
    }
    return argTypes.size();
}

/**
 * @brief Determines if all candidate overloads agree on a single bad argument index.
 *
 * @param[in] candidates Candidate functions.
 * @param[in] argTypes Argument types.
 * @param[in] table Symbol table.
 * @return Blamed argument index if agreed, or std::nullopt.
 */
std::optional<size_t> FindAgreedBlamedArgument(std::span<const Symbol* const> candidates,
                                               const std::vector<std::string>& argTypes, const SymbolTable& table)
{
    if (candidates.empty())
    {
        return std::nullopt;
    }

    size_t blamed = argTypes.size();
    for (const auto* candidate : candidates)
    {
        if (!candidate)
        {
            continue;
        }
        const size_t firstBad = FindCandidateBadArgument(*candidate, argTypes, table);
        if (firstBad == argTypes.size())
        {
            return std::nullopt;
        }
        if (blamed == argTypes.size())
        {
            blamed = firstBad;
        }
        else if (blamed != firstBad)
        {
            return std::nullopt;
        }
    }
    return blamed < argTypes.size() ? std::optional<size_t>(blamed) : std::nullopt;
}

/**
 * @brief Emits diagnostics when overload resolution finds no viable candidates.
 *
 * @param[in] candidates Candidate functions.
 * @param[in] args Call arguments info.
 * @param[in] reportedName Reported callee name.
 * @param[in] valCtx Call validation context.
 */
void ReportOverloadResolutionFailure(std::span<const Symbol* const> candidates, const CallArgTypes& args,
                                     const std::string& reportedName, const CallValidationContext& valCtx)
{
    const auto blamedArg = FindAgreedBlamedArgument(candidates, args.argTypes, valCtx.ctx.request.symbolTable);
    if (blamedArg && *blamedArg < args.argNodes.size())
    {
        const auto& fn = candidates[0]->GetFunction();
        const std::string expected =
            *blamedArg < fn.parameters.size() ? fn.parameters[*blamedArg].typeName : std::string();
        const TSPoint aStart = ts_node_start_point(args.argNodes[*blamedArg]);
        const TSPoint aEnd = ts_node_end_point(args.argNodes[*blamedArg]);
        valCtx.ctx.EmitAtRange({aStart.row, aStart.column, aEnd.row, aEnd.column}, "as-err-no-implicit-conversion",
                               args.argTypes[*blamedArg], expected);
        return;
    }

    if (args.allArgsResolved)
    {
        const TSPoint start = ts_node_start_point(valCtx.callee);
        const TSPoint end = ts_node_end_point(valCtx.arguments);
        valCtx.ctx.EmitAtRange({start.row, start.column, end.row, end.column}, "as-err-call-no-matching-signature",
                               reportedName);
    }
}

/**
 * @brief Checks whether a named symbol resolves to an assignable non-const variable.
 *
 * @param[in] name Symbol name.
 * @param[in] scope Lexical scope.
 * @param[in] table Symbol table.
 * @return True if assignable variable.
 */
bool IsAssignableLValueSymbol(std::string_view name, const Scope* scope, const SymbolTable& table)
{
    if (scope)
    {
        const auto* def = ResolveInScope(scope, name);
        if (def && (def->kind == LocalDefinitionKind::Variable || def->kind == LocalDefinitionKind::Parameter))
        {
            return true;
        }
    }
    auto syms = table.FindSymbolsPtr(std::string(name));
    if (syms)
    {
        for (const auto& s : *syms)
        {
            if (s.type == SymbolType::Variable && !s.GetVariable().modifiers.isConst)
            {
                return true;
            }
        }
    }
    return false;
}

/**
 * @brief Validates if an AST node is a valid L-value for an out parameter.
 *
 * @param[in] argNode Argument AST node.
 * @param[in] sourceCode Document source text.
 * @param[in] scope Lexical scope.
 * @param[in] table Symbol table.
 * @return True if node is an assignable L-value.
 */
bool CheckArgIsLValue(TSNode argNode, std::string_view sourceCode, const Scope* scope, const SymbolTable& table)
{
    std::string_view aType = ts_node_type(argNode);
    std::string aText = NodeText(argNode, sourceCode);
    TrimString(aText);

    if (aType == "unary_expression" && aText.starts_with("@"))
    {
        TSNode operand = parser::GetChildByField(argNode, parser::fields::Operand);
        if (!ts_node_is_null(operand))
        {
            argNode = operand;
            aType = ts_node_type(argNode);
            aText = NodeText(argNode, sourceCode);
            TrimString(aText);
        }
    }

    if (aType == "identifier" || aType == "scoped_identifier")
    {
        return IsAssignableLValueSymbol(aText, scope, table);
    }
    return (aType == "member_expression" || aType == "index_expression");
}

/**
 * @brief Validates that arguments passed to &out parameters are assignable L-values.
 *
 * @param[in] candidate Chosen function overload.
 * @param[in] argNodes Call argument AST nodes.
 * @param[in] valCtx Call validation context.
 */
void ValidateOutArguments(const Symbol& candidate, const std::vector<TSNode>& argNodes,
                          const CallValidationContext& valCtx)
{
    const auto& fn = candidate.GetFunction();
    for (size_t i = 0; i < argNodes.size() && i < fn.parameters.size(); ++i)
    {
        const auto& param = fn.parameters[i];
        if (param.rawText.find("&out") == std::string::npos && param.typeName.find("&out") == std::string::npos &&
            param.typeName.find("& out") == std::string::npos)
        {
            continue;
        }

        std::string aText = NodeText(argNodes[i], valCtx.request.sourceCode);
        TrimString(aText);
        if (aText == "void")
        {
            continue;
        }

        if (!CheckArgIsLValue(argNodes[i], valCtx.request.sourceCode, valCtx.scope, valCtx.ctx.request.symbolTable))
        {
            const TSPoint aStart = ts_node_start_point(argNodes[i]);
            const TSPoint aEnd = ts_node_end_point(argNodes[i]);
            valCtx.ctx.EmitAtRange({aStart.row, aStart.column, aEnd.row, aEnd.column},
                                   "as-err-lvalue-required-for-out-param");
        }
    }
}

/**
 * @brief Validates if candidate signature can accept named and positional arguments.
 * @param[in] sig Candidate function signature.
 * @param[in] argNames Vector of argument names (empty for positional).
 * @param[in] argTypes Vector of argument types.
 * @param[in] symbolTable Symbol table for type conversion checking.
 * @return True if candidate matches the named arguments call.
 */
bool CheckCandidateNamedArgs(const FunctionSignature& sig, const std::vector<std::string>& argNames,
                             const std::vector<std::string>& argTypes, const SymbolTable& symbolTable)
{
    std::unordered_set<size_t> matchedParams;
    for (size_t i = 0; i < argNames.size(); ++i)
    {
        size_t paramIdx = size_t(-1);
        if (!argNames[i].empty())
        {
            for (size_t p = 0; p < sig.parameters.size(); ++p)
            {
                if (sig.parameters[p].name == argNames[i])
                {
                    paramIdx = p;
                    break;
                }
            }
            if (paramIdx == size_t(-1) || matchedParams.contains(paramIdx))
            {
                return false;
            }
        }
        else
        {
            paramIdx = i;
            if (paramIdx >= sig.parameters.size())
            {
                const auto arity = ArityOf(sig);
                if (!arity.variadic)
                {
                    return false;
                }
            }
        }
        if (paramIdx < sig.parameters.size())
        {
            matchedParams.insert(paramIdx);

            if (!argTypes[i].empty())
            {
                const auto conv = EvaluateArgumentConversion(argTypes[i], sig.parameters[paramIdx], symbolTable, true);
                if (!conv.IsViable())
                {
                    return false;
                }
            }
        }
    }

    for (size_t p = 0; p < sig.parameters.size(); ++p)
    {
        if (!matchedParams.contains(p) && sig.parameters[p].defaultValue.empty())
        {
            return false;
        }
    }
    return true;
}

/**
 * @brief Checks overload resolution for calls specifying named arguments.
 * @param[in] matchingArity Candidates of matching arity.
 * @param[in] args Resolved call arguments.
 * @param[in] calleeRes Callee resolution details.
 * @param[in] valCtx Call validation context.
 */
void HandleNamedCallOverloads(std::span<const Symbol* const> matchingArity, const CallArgTypes& args,
                              const CalleeResolution& calleeRes, const CallValidationContext& valCtx)
{
    bool matchedAny = false;
    for (const Symbol* cand : matchingArity)
    {
        if (cand && std::holds_alternative<FunctionSignature>(cand->signature))
        {
            if (CheckCandidateNamedArgs(cand->GetFunction(), args.argNames, args.argTypes,
                                        valCtx.ctx.request.symbolTable))
            {
                matchedAny = true;
                break;
            }
        }
    }
    if (!matchedAny && args.allArgsResolved)
    {
        const TSPoint start = ts_node_start_point(valCtx.callee);
        const TSPoint end = ts_node_end_point(valCtx.arguments);
        valCtx.ctx.EmitAtRange({start.row, start.column, end.row, end.column}, "as-err-call-no-matching-signature",
                               calleeRes.reportedName);
    }
}

/**
 * @brief Dispatches overload resolution and checks ambiguity, conversions, and out parameters.
 *
 * @param[in] matchingArity Candidates of matching arity.
 * @param[in] args Resolved argument types.
 * @param[in] calleeRes Callee resolution details.
 * @param[in] valCtx Call validation context.
 */
void CheckCallOverloads(std::span<const Symbol* const> matchingArity, const CallArgTypes& args,
                        const CalleeResolution& calleeRes, const CallValidationContext& valCtx)
{
    if ((args.argTypes.empty() && !calleeRes.candidatesAreFreeFunctions) || matchingArity.empty())
    {
        return;
    }

    const bool hasNamedArgs =
        std::any_of(args.argNames.begin(), args.argNames.end(), [](const std::string& n) { return !n.empty(); });
    if (hasNamedArgs)
    {
        HandleNamedCallOverloads(matchingArity, args, calleeRes, valCtx);
        return;
    }

    OverloadMatchResult match =
        ResolveBestOverload(matchingArity, args.argTypes, valCtx.ctx.request.symbolTable, args.argIsLValue);
    if (match.isAmbiguous && args.allArgsResolved)
    {
        const TSPoint start = ts_node_start_point(valCtx.callee);
        const TSPoint end = ts_node_end_point(valCtx.arguments);
        valCtx.ctx.EmitAtRange({start.row, start.column, end.row, end.column}, "as-err-call-ambiguous",
                               calleeRes.reportedName);
    }
    else if (match.viableCandidates.empty() || match.bestCandidate == nullptr)
    {
        ReportOverloadResolutionFailure(matchingArity, args, calleeRes.reportedName, valCtx);
    }
    else
    {
        ValidateOutArguments(*match.bestCandidate, args.argNodes, valCtx);
    }
}

/**
 * @brief Resolves callee candidates based on node type.
 *
 * @param[in] valCtx Call validation context.
 * @return Resolved CalleeResolution.
 */
CalleeResolution ResolveCalleeCandidates(const CallValidationContext& valCtx)
{
    const std::string_view calleeType = ts_node_type(valCtx.callee);
    if (calleeType == "member_expression")
    {
        return ResolveMemberCallee(valCtx);
    }
    if (calleeType == "scoped_identifier" || calleeType == "identifier" || calleeType == "function" ||
        parser::keywords::IsKeyword(calleeType))
    {
        return ResolveIdentifierCallee(valCtx);
    }
    CalleeResolution res;
    res.shouldCheck = false;
    return res;
}

/**
 * @brief Validates argument count against candidate arities and emits diagnostics on failure.
 *
 * @param[in] calleeRes Callee resolution details.
 * @param[in] argumentCount Number of passed arguments.
 * @param[in] valCtx Call validation context.
 * @return True if argument count is accepted.
 */
bool CheckArgumentCount(const CalleeResolution& calleeRes, uint32_t argumentCount, const CallValidationContext& valCtx)
{
    const CandidateSet judged = JudgeAgainst(calleeRes.candidates, argumentCount);
    if (!judged.decided || !judged.accepts)
    {
        if (judged.decided && !judged.accepts && !calleeRes.isUnqualifiedClassCall)
        {
            const TSPoint start = ts_node_start_point(valCtx.callee);
            const TSPoint end = ts_node_end_point(valCtx.arguments);
            valCtx.ctx.EmitAtRange({start.row, start.column, end.row, end.column}, "as-err-call-argument-count",
                                   calleeRes.reportedName, std::to_string(argumentCount));
        }
        return false;
    }
    return true;
}

/**
 * @brief Validates a single function call expression.
 *
 * @param[in] node AST call expression node.
 * @param[in] request Analysis call request.
 * @param[in] scope Lexical scope.
 * @param[in,out] ctx Diagnostic context.
 */
void CheckCall(TSNode node, const CallCheckRequest& request, const Scope* scope, DiagnosticContext& ctx)
{
    TSNode callee = parser::GetChildByField(node, parser::fields::Function);
    TSNode arguments = parser::GetChildByField(node, parser::fields::Arguments);
    if (ts_node_is_null(callee) || ts_node_is_null(arguments))
    {
        return;
    }

    const CallValidationContext valCtx{node, callee, arguments, request, scope, ctx};
    const CalleeResolution calleeRes = ResolveCalleeCandidates(valCtx);
    if (!calleeRes.shouldCheck)
    {
        return;
    }

    bool sawNamedArg = false;
    if (!ValidateArgumentOrdering(arguments, sawNamedArg, ctx))
    {
        return;
    }

    const uint32_t argumentCount = CountArguments(arguments);
    if (!CheckArgumentCount(calleeRes, argumentCount, valCtx))
    {
        return;
    }

    const CallArgTypes args = ResolveCallArguments(valCtx);
    const std::vector<const Symbol*> matchingArity = FilterMatchingArityCandidates(calleeRes.candidates, argumentCount);

    if (matchingArity.size() == 1 && !sawNamedArg && matchingArity.front())
    {
        CheckInitializerListArgs(args.argNodes, matchingArity.front()->GetFunction(), valCtx);
    }

    if (!sawNamedArg)
    {
        const LambdaCheckContext lctx{callee,
                                      arguments,
                                      calleeRes.reportedName,
                                      ctx.request.symbolTable,
                                      request.sourceCode,
                                      args.argNodes,
                                      FindLambdaPositions(args.argNodes)};
        CheckLambdaArguments(matchingArity, lctx, ctx);
    }

    CheckMalformedTernaryArgs(args.argNodes, args.argTypes, matchingArity, valCtx);
    CheckCallOverloads(matchingArity, args, calleeRes, valCtx);
}

/**
 * @brief Finds the type AST node within a variable declaration node.
 *
 * @param[in] varDeclNode AST variable declaration node.
 * @return Type AST node or null node if not found.
 */
TSNode FindVariableTypeNode(TSNode varDeclNode)
{
    TSNode varTypeNode = parser::GetChildByField(varDeclNode, parser::fields::VarType);
    if (!ts_node_is_null(varTypeNode))
    {
        return varTypeNode;
    }
    varTypeNode = parser::GetChildByField(varDeclNode, parser::fields::Type);
    if (!ts_node_is_null(varTypeNode))
    {
        return varTypeNode;
    }
    const uint32_t count = ts_node_named_child_count(varDeclNode);
    for (uint32_t i = 0; i < count; ++i)
    {
        TSNode child = ts_node_named_child(varDeclNode, i);
        if (std::string_view(ts_node_type(child)) == "type")
        {
            return child;
        }
    }
    return varTypeNode;
}

/**
 * @brief Formats an attempted constructor call signature for error reporting.
 *
 * @param[in] targetType Declared type name.
 * @param[in] argTypes Resolved argument types.
 * @return Formatted signature string, e.g. "Type(int, string)".
 */
std::string FormatSignatureAttempt(const std::string& targetType, const std::vector<std::string>& argTypes)
{
    std::string sig = targetType + "(";
    for (size_t a = 0; a < argTypes.size(); ++a)
    {
        if (a > 0)
        {
            sig += ", ";
        }
        sig += argTypes[a];
    }
    sig += ")";
    return sig;
}

/** @brief Context bundled for variable direct initialization verification. */
struct VarInitContext
{
    std::string declaredType;
    std::string baseName;
    TemplateTypeInfo tmplInfo;
    const CallCheckRequest& request;
    DiagnosticContext& ctx;
};

/**
 * @brief Verifies direct initialization of a primitive type.
 *
 * @param[in] argListNode Argument list AST node.
 * @param[in] argTypes Resolved argument types.
 * @param[in] vctx Variable initialization context.
 */
void CheckPrimitiveDirectInit(TSNode argListNode, const std::vector<std::string>& argTypes, const VarInitContext& vctx)
{
    if (argTypes.size() != 1)
    {
        const TSPoint aStart = ts_node_start_point(argListNode);
        const TSPoint aEnd = ts_node_end_point(argListNode);
        vctx.ctx.EmitAtRange({aStart.row, aStart.column, aEnd.row, aEnd.column}, "as-err-no-matching-constructor",
                             FormatSignatureAttempt(vctx.declaredType, argTypes));
    }
    else
    {
        ParameterInformation dummyParam{vctx.baseName, vctx.baseName, "", ""};
        const auto conv = EvaluateArgumentConversion(argTypes[0], dummyParam, vctx.ctx.request.symbolTable);
        if (!conv.IsViable())
        {
            const TSPoint aStart = ts_node_start_point(argListNode);
            const TSPoint aEnd = ts_node_end_point(argListNode);
            vctx.ctx.EmitAtRange({aStart.row, aStart.column, aEnd.row, aEnd.column}, "as-err-no-implicit-conversion",
                                 argTypes[0], vctx.baseName);
        }
    }
}

/**
 * @brief Looks up constructor declarations for a given type name in symbol table.
 *
 * @param[in] baseName Cleaned type name.
 * @param[in] table Symbol table.
 * @return Candidate constructor symbols.
 */
std::vector<Symbol> LookupRawConstructors(const std::string& baseName, const SymbolTable& table)
{
    std::vector<Symbol> rawConstructors;
    auto collectFunctions = [&](const std::string& key)
    {
        if (const auto found = table.FindSymbolsPtr(key))
        {
            for (const auto& s : *found)
            {
                if (s.type == SymbolType::Function)
                {
                    rawConstructors.push_back(s);
                }
            }
        }
    };

    const std::string shortName = LastScopeSegment(baseName);
    collectFunctions(baseName + "::" + shortName);
    if (shortName != baseName)
    {
        collectFunctions(baseName + "::" + baseName);
    }
    if (rawConstructors.empty())
    {
        collectFunctions(baseName);
    }
    if (rawConstructors.empty())
    {
        for (const auto& sym : table.FindTypeSymbolsByShortName(shortName))
        {
            if (sym.type == SymbolType::Class)
            {
                collectFunctions(sym.name + "::" + shortName);
            }
        }
    }
    return rawConstructors;
}

/**
 * @brief Checks if a class declaration is visible in script source (not predefined).
 *
 * @param[in] baseName Type name.
 * @param[in] table Symbol table.
 * @param[in] predefinedExt Predefined file extension.
 * @return True if class declaration is visible in script.
 */
bool IsClassDeclarationVisible(const std::string& baseName, const SymbolTable& table, const std::string& predefinedExt)
{
    auto checkBucket = [&](const std::vector<Symbol>& syms)
    {
        for (const auto& declaration : syms)
        {
            if (declaration.type == SymbolType::Class &&
                std::holds_alternative<ClassSignature>(declaration.signature) &&
                !utils::IsPredefinedFile(declaration.fileUri, predefinedExt))
            {
                return true;
            }
        }
        return false;
    };

    if (const auto declarations = table.FindSymbolsPtr(baseName))
    {
        if (checkBucket(*declarations))
        {
            return true;
        }
    }
    const std::string shortName = LastScopeSegment(baseName);
    return checkBucket(table.FindTypeSymbolsByShortName(shortName));
}

/**
 * @brief Retrieves template parameter names declared on a class.
 *
 * @param[in] baseName Class name.
 * @param[in] table Symbol table.
 * @return Template parameter names (e.g. {"T"}).
 */
std::vector<std::string> GetClassTemplateParams(const std::string& baseName, const SymbolTable& table)
{
    std::vector<std::string> templateParams;
    if (auto classSymbols = table.FindSymbolsPtr(baseName))
    {
        for (const auto& cs : *classSymbols)
        {
            if (cs.type == SymbolType::Class && std::holds_alternative<ClassSignature>(cs.signature))
            {
                templateParams = cs.GetClass().templateParams;
                break;
            }
        }
    }
    if (templateParams.empty())
    {
        templateParams.push_back("T");
    }
    return templateParams;
}

/**
 * @brief Specializes a single constructor parameter by replacing template parameters.
 *
 * @param[in,out] param Parameter information to specialize.
 * @param[in] paramName Template parameter name.
 * @param[in] concreteArg Concrete argument type.
 */
void SpecializeConstructorParam(ParameterInformation& param, const std::string& paramName,
                                const std::string& concreteArg)
{
    if (param.baseTypeName == paramName)
    {
        param.baseTypeName = concreteArg;
    }
    size_t pos = 0;
    while ((pos = param.typeName.find(paramName, pos)) != std::string::npos)
    {
        const bool beforeOk = (pos == 0 || !isalnum(static_cast<unsigned char>(param.typeName[pos - 1])));
        const bool afterOk = (pos + paramName.size() >= param.typeName.size() ||
                              !isalnum(static_cast<unsigned char>(param.typeName[pos + paramName.size()])));
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

/**
 * @brief Specializes raw constructor candidates using template argument mappings.
 *
 * @param[in] rawConstructors Unspecialized constructor symbols.
 * @param[in] tmplInfo Parsed template type information.
 * @param[in] baseName Base type name.
 * @param[in] table Symbol table.
 * @return Specialized constructor symbols.
 */
std::vector<Symbol> SpecializeConstructors(const std::vector<Symbol>& rawConstructors, const TemplateTypeInfo& tmplInfo,
                                           const std::string& baseName, const SymbolTable& table)
{
    if (tmplInfo.templateArgs.empty())
    {
        return rawConstructors;
    }

    const std::vector<std::string> templateParams = GetClassTemplateParams(baseName, table);
    std::vector<Symbol> candidates;

    for (const auto& sym : rawConstructors)
    {
        Symbol specSym = sym;
        if (std::holds_alternative<FunctionSignature>(specSym.signature))
        {
            auto& fn = specSym.GetFunction();
            for (auto& param : fn.parameters)
            {
                for (size_t t = 0; t < templateParams.size() && t < tmplInfo.templateArgs.size(); ++t)
                {
                    SpecializeConstructorParam(param, templateParams[t], tmplInfo.templateArgs[t]);
                }
            }
        }
        candidates.push_back(std::move(specSym));
    }
    return candidates;
}

/**
 * @brief Verifies constructor candidate overloads against provided arguments.
 *
 * @param[in] argListNode Argument list AST node.
 * @param[in] candidates Candidate constructor symbols.
 * @param[in] argTypes Resolved argument types.
 * @param[in] vctx Variable initialization context.
 */
void CheckConstructorOverload(TSNode argListNode, const std::vector<Symbol>& candidates,
                              const std::vector<std::string>& argTypes, const VarInitContext& vctx)
{
    const uint32_t argCount = static_cast<uint32_t>(argTypes.size());
    const std::vector<const Symbol*> matchingArity = FilterMatchingArityCandidates(candidates, argCount);

    if (matchingArity.empty())
    {
        const TSPoint aStart = ts_node_start_point(argListNode);
        const TSPoint aEnd = ts_node_end_point(argListNode);
        vctx.ctx.EmitAtRange({aStart.row, aStart.column, aEnd.row, aEnd.column}, "as-err-no-matching-constructor",
                             FormatSignatureAttempt(vctx.declaredType, argTypes));
        return;
    }

    auto match = ResolveBestOverload(matchingArity, argTypes, vctx.ctx.request.symbolTable);
    if (match.bestCandidate == nullptr)
    {
        const TSPoint aStart = ts_node_start_point(argListNode);
        const TSPoint aEnd = ts_node_end_point(argListNode);
        vctx.ctx.EmitAtRange({aStart.row, aStart.column, aEnd.row, aEnd.column}, "as-err-no-matching-constructor",
                             FormatSignatureAttempt(vctx.declaredType, argTypes));
    }
}

/**
 * @brief Verifies direct initialization arguments for a single variable declarator.
 *
 * @param[in] declarator AST variable declarator node.
 * @param[in] vctx Variable initialization context.
 */
void CheckDeclaratorDirectInit(TSNode declarator, const VarInitContext& vctx)
{
    TSNode argListNode = parser::GetChildByField(declarator, parser::fields::Arguments);
    if (ts_node_is_null(argListNode))
    {
        return;
    }

    const TSPoint start = ts_node_start_point(declarator);
    const Scope* scope = FindInnermostScope(vctx.request.scopeRoot, start.row, start.column);

    const std::vector<TSNode> argNodes = GetArgumentNodes(argListNode);
    std::vector<std::string> argTypes;
    for (TSNode argNode : argNodes)
    {
        argTypes.push_back(ResolveExpressionType(
            argNode, {scope, vctx.ctx.request.symbolTable, vctx.request.sourceCode, vctx.ctx.request.fileUri}));
    }

    if (IsCorePrimitive(vctx.baseName))
    {
        CheckPrimitiveDirectInit(argListNode, argTypes, vctx);
        return;
    }

    const std::vector<Symbol> rawConstructors = LookupRawConstructors(vctx.baseName, vctx.ctx.request.symbolTable);
    if (rawConstructors.empty())
    {
        if (argTypes.empty())
        {
            return;
        }
        if (IsClassDeclarationVisible(vctx.baseName, vctx.ctx.request.symbolTable,
                                      vctx.ctx.request.predefinedFileExtension))
        {
            const TSPoint aStart = ts_node_start_point(argListNode);
            const TSPoint aEnd = ts_node_end_point(argListNode);
            vctx.ctx.EmitAtRange({aStart.row, aStart.column, aEnd.row, aEnd.column}, "as-err-no-matching-constructor",
                                 FormatSignatureAttempt(vctx.declaredType, argTypes));
        }
        return;
    }

    const std::vector<Symbol> candidates =
        SpecializeConstructors(rawConstructors, vctx.tmplInfo, vctx.baseName, vctx.ctx.request.symbolTable);
    CheckConstructorOverload(argListNode, candidates, argTypes, vctx);
}

/**
 * @brief Checks direct variable initialization syntax against accessible constructors.
 *
 * @param[in] varDeclNode AST variable declaration node.
 * @param[in] request Analysis call request.
 * @param[in,out] ctx Diagnostic context.
 */
void CheckVariableDirectInitialization(TSNode varDeclNode, const CallCheckRequest& request, DiagnosticContext& ctx)
{
    TSNode varTypeNode = FindVariableTypeNode(varDeclNode);
    if (ts_node_is_null(varTypeNode))
    {
        return;
    }

    const std::string rawTypeStr = NodeText(varTypeNode, request.sourceCode);
    const std::string declaredType = CleanExpressionType(rawTypeStr);
    if (declaredType.empty() || declaredType == "auto" || rawTypeStr.find('[') != std::string::npos)
    {
        return;
    }

    const TemplateTypeInfo tmplInfo = ParseTemplateType(declaredType);
    std::string baseName = tmplInfo.containerName.empty() ? declaredType : tmplInfo.containerName;
    baseName = CleanBaseType(baseName);

    const VarInitContext vctx{declaredType, baseName, tmplInfo, request, ctx};
    const uint32_t declaratorCount = ts_node_named_child_count(varDeclNode);
    for (uint32_t d = 0; d < declaratorCount; ++d)
    {
        TSNode declarator = ts_node_named_child(varDeclNode, d);
        if (std::string_view(ts_node_type(declarator)) == "variable_declarator")
        {
            CheckDeclaratorDirectInit(declarator, vctx);
        }
    }
}

/**
 * @brief Non-recursive worklist traversal of AST nodes for call checking.
 *
 * @param[in] root Root AST node.
 * @param[in] request Analysis call request.
 * @param[in,out] ctx Diagnostic context.
 */
void TraverseAstNodes(TSNode root, const CallCheckRequest& request, DiagnosticContext& ctx)
{
    std::vector<TSNode> stack;
    stack.push_back(root);

    while (!stack.empty())
    {
        TSNode node = stack.back();
        stack.pop_back();

        if (ts_node_is_null(node))
        {
            continue;
        }

        const std::string_view nodeType = ts_node_type(node);
        if (nodeType == "call_expression")
        {
            const TSPoint start = ts_node_start_point(node);
            CheckCall(node, request, FindInnermostScope(request.scopeRoot, start.row, start.column), ctx);
        }
        else if (nodeType == "variable_declaration")
        {
            CheckVariableDirectInitialization(node, request, ctx);
        }

        const uint32_t childCount = ts_node_named_child_count(node);
        for (uint32_t i = childCount; i > 0; --i)
        {
            stack.push_back(ts_node_named_child(node, i - 1));
        }
    }
}
} // namespace

void CheckCallArguments(const CallCheckRequest& request, DiagnosticContext& ctx)
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
        auto callNodes = request.nodeIndex->Nodes(parser::nodes::CallExpression);
        auto varNodes = request.nodeIndex->Nodes(parser::nodes::VariableDeclaration);
        size_t i = 0;
        size_t j = 0;
        while (i < callNodes.size() || j < varNodes.size())
        {
            bool takeCall = false;
            if (i < callNodes.size() && j < varNodes.size())
            {
                takeCall = (ts_node_start_byte(callNodes[i]) <= ts_node_start_byte(varNodes[j]));
            }
            else if (i < callNodes.size())
            {
                takeCall = true;
            }

            if (takeCall)
            {
                TSNode node = callNodes[i++];
                const TSPoint start = ts_node_start_point(node);
                CheckCall(node, request, FindInnermostScope(request.scopeRoot, start.row, start.column), ctx);
            }
            else
            {
                TSNode node = varNodes[j++];
                CheckVariableDirectInitialization(node, request, ctx);
            }
        }
        return;
    }

    TraverseAstNodes(request.root, request, ctx);
}
} // namespace angel_lsp::analysis
