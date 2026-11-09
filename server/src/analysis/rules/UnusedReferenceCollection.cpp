#include "analysis/index/NodeIndex.h"
#include "analysis/index/ReflectionCallFacts.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/index/SymbolTable.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/rules/UnusedSymbolChecker.h"
#include "utils/ScopedNameUtils.h"
#include "utils/StringPool.h"
#include "utils/Utils.h"

#include <algorithm>
#include <ankerl/unordered_dense.h>
#include <cstdint>
#include <string_view>
#include <tuple>
#include <vector>

namespace angel_lsp::analysis::rules
{

/**
 * @brief Combines a 32-bit line and 32-bit column into a packed 64-bit coordinate.
 * @param[in] line Zero-based source line index.
 * @param[in] col Zero-based source column character index.
 * @return Packed 64-bit integer representing the location.
 */
static inline uint64_t PackLoc(uint32_t line, uint32_t col) noexcept
{
    return (static_cast<uint64_t>(line) << 32) | static_cast<uint64_t>(col);
}

/**
 * @brief Collects source positions of all local definitions in a lexical scope tree.
 * @param[in] rootScope Root of the lexical scope tree to walk.
 * @param[in,out] defLocs Set accumulating packed coordinates of definitions.
 */
void CollectDefLocations(const Scope* rootScope, ankerl::unordered_dense::set<uint64_t>& defLocs)
{
    if (!rootScope)
        return;
    std::vector<const Scope*> worklist{rootScope};
    while (!worklist.empty())
    {
        const Scope* scope = worklist.back();
        worklist.pop_back();

        for (const auto& def : scope->definitions)
            defLocs.insert(PackLoc(def.startLine, def.startCharacter));
        for (const auto& child : scope->children)
        {
            if (child)
                worklist.push_back(child.get());
        }
    }
}

/**
 * @brief Adds every referenced name found in a scope tree, except references at definition sites and member
 *        accesses on delegate-property receivers.
 * @param[in] rootScope Root of the lexical scope tree to walk.
 * @param[in] docCtx Definition locations to skip, the source text and the rules naming delegate properties.
 * @param[in,out] refNames Receives the referenced names as views of pool storage.
 * @param[in,out] pool Owns the copies of the names, so rootScope may be released afterwards.
 */
void CollectDocReferences(const Scope* rootScope, const DocRefCollectContext& docCtx, NameViewSet& refNames,
                          utils::StringPool& pool)
{
    if (!rootScope)
        return;
    std::vector<const Scope*> worklist{rootScope};
    while (!worklist.empty())
    {
        const Scope* scope = worklist.back();
        worklist.pop_back();

        for (const auto& ref : scope->references)
        {
            if (docCtx.defLocs.contains(PackLoc(ref.startLine, ref.startCharacter)))
                continue;
            if (docCtx.rules && !docCtx.rules->baseClassDelegateProperties.empty() && ref.isMemberAccess &&
                ref.receiverEndByte > ref.receiverStartByte && !docCtx.sourceCode.empty() &&
                ref.receiverEndByte <= docCtx.sourceCode.size())
            {
                const std::string_view recv =
                    docCtx.sourceCode.substr(ref.receiverStartByte, ref.receiverEndByte - ref.receiverStartByte);
                if (std::any_of(docCtx.rules->baseClassDelegateProperties.begin(),
                                docCtx.rules->baseClassDelegateProperties.end(),
                                [recv](std::string_view prop) { return prop == recv; }))
                {
                    continue;
                }
            }
            refNames.insert(pool.Intern(ref.name));
        }
        for (const auto& child : scope->children)
        {
            if (child)
                worklist.push_back(child.get());
        }
    }
}

/**
 * @brief Collects local definitions referenced within an expression or statement.
 * @param[in] rootScope Lexical scope for symbol lookup.
 * @param[in,out] usedLocals Accumulator set receiving resolved local definitions.
 */
void CollectUsedLocals(const Scope* rootScope, ankerl::unordered_dense::set<const LocalDefinition*>& usedLocals)
{
    if (!rootScope)
        return;
    std::vector<const Scope*> worklist{rootScope};
    while (!worklist.empty())
    {
        const Scope* scope = worklist.back();
        worklist.pop_back();

        for (const auto& ref : scope->references)
        {
            if (ref.isMemberAccess || ref.isNamedArgument)
                continue;
            const LocalDefinition* def = ResolveInScope(scope, ref.name);
            if (def && (ref.startLine != def->startLine || ref.startCharacter != def->startCharacter))
                usedLocals.insert(def);
        }
        for (const auto& child : scope->children)
        {
            if (child)
                worklist.push_back(child.get());
        }
    }
}

/**
 * @brief Adds the names referenced by the other files of the module.
 * @param[in] req Request providing the module file list and the scope index.
 * @param[in,out] refNames Receives the referenced names as views of pool storage.
 * @param[in,out] pool Owns the copies of the names.
 */
void CollectCrossFileReferences(const SemanticAnalysisRequest& req, NameViewSet& refNames, utils::StringPool& pool)
{
    if (!req.scopeIndex || req.moduleFileUris.empty())
        return;
    for (const auto& uri : req.moduleFileUris)
    {
        if (uri != req.fileUri)
        {
            if (const auto root = req.scopeIndex->GetRoot(uri))
            {
                ankerl::unordered_dense::set<uint64_t> defs;
                CollectDefLocations(root.get(), defs);
                CollectDocReferences(root.get(), DocRefCollectContext{defs, std::string_view{}, nullptr}, refNames,
                                     pool);
            }
        }
    }
}

/**
 * @brief Matches a callee function expression against a configured reflection callee rule.
 * @param[in] funcText Callee text (e.g. member access or scoped name).
 * @param[in] c Configured reflection callee descriptor.
 * @return True if callee matches the configured receiver and method name.
 */
static bool MatchCallee(std::string_view funcText, const config::StringReflectionCallee& c)
{
    std::string_view receiver;
    std::string_view method = funcText;
    const auto dotPos = funcText.find('.');

    if (dotPos != std::string_view::npos)
    {
        receiver = funcText.substr(0, dotPos);
        method = funcText.substr(dotPos + 1);
    }
    else if (utils::ContainsScopeSeparator(funcText))
    {
        std::tie(receiver, method) = utils::SplitScopedName(funcText);
    }

    if (!c.callee.empty() && receiver != c.callee)
        return false;

    return std::any_of(c.methods.begin(), c.methods.end(), [&](std::string_view m) { return m == method; });
}

/**
 * @brief Registers a symbol name and its qualified variations as reflected.
 * @param[in] str Reflected symbol name text.
 * @param[in] req Semantic analysis request for symbol table lookup.
 * @param[in,out] outReflections Accumulator set for reflected symbol names.
 * @param[in,out] pool String pool storing interned copies of symbol names.
 */
static void RegisterReflectedSymbol(std::string_view str, const SemanticAnalysisRequest& req,
                                    NameViewSet& outReflections, utils::StringPool& pool)
{
    if (str.empty())
        return;

    outReflections.insert(pool.Intern(str));

    if (const auto sym = req.symbolTable.FindFirstSymbol(str))
    {
        outReflections.insert(pool.Intern(sym->name));
        outReflections.insert(pool.Intern(sym->qualifiedName));
        return;
    }

    if (!utils::ContainsScopeSeparator(str))
    {
        const auto types = req.symbolTable.FindTypeSymbolsByShortName(str);
        for (const auto& t : types)
        {
            outReflections.insert(pool.Intern(t.name));
            outReflections.insert(pool.Intern(t.qualifiedName));
        }
    }
}

/**
 * @brief Traverses lexical scopes to collect symbol names referenced in string literals.
 * @param[in] root Root lexical scope from which to collect string literal references.
 * @param[in,out] result Output set accumulating symbol names found in string literal references.
 * @param[in,out] pool String pool storing interned copies of string literal references.
 */
static void CollectScopeStringLiteralRefs(const Scope* root, NameViewSet& result, utils::StringPool& pool)
{
    if (!root)
    {
        return;
    }
    std::vector<const Scope*> stack{root};
    while (!stack.empty())
    {
        const Scope* s = stack.back();
        stack.pop_back();
        if (s->stringLiteralCallRefs)
        {
            for (const auto& strRef : *s->stringLiteralCallRefs)
            {
                result.insert(pool.Intern(strRef.name));
            }
        }
        for (const auto& child : s->children)
        {
            if (child)
            {
                stack.push_back(child.get());
            }
        }
    }
}

/**
 * @brief Collects symbols referenced via string literals and reflection calls across the module.
 * @param[in] req Semantic analysis request containing scopes, rules, and node index.
 * @param[in,out] pool Owns the copies of the names, which come from temporary lookups and reflection facts.
 * @return Names identified as referenced through reflection, viewing pool storage.
 */
NameViewSet CollectStringReflection(const SemanticAnalysisRequest& req, utils::StringPool& pool)
{
    NameViewSet result;
    if (req.scopeRoot)
    {
        CollectScopeStringLiteralRefs(req.scopeRoot.get(), result, pool);
    }
    if (req.scopeIndex && !req.moduleFileUris.empty())
    {
        for (const auto& uri : req.moduleFileUris)
        {
            if (uri != req.fileUri && !utils::IsPredefinedFile(uri, req.predefinedFileExtension))
            {
                if (const auto root = req.scopeIndex->GetRoot(uri))
                {
                    CollectScopeStringLiteralRefs(root.get(), result, pool);
                }
            }
        }
    }

    const auto* callees = req.engineRules ? &req.engineRules->unusedRules.stringReflectionCallees : nullptr;

    if (callees && !callees->empty() && req.nodeIndex)
    {
        for (const auto& call : BuildReflectionCallFacts(*req.nodeIndex, req.sourceCode))
        {
            for (const auto& c : *callees)
            {
                if (MatchCallee(call.calleeText, c))
                {
                    for (const size_t idx : c.argIndices)
                    {
                        if (idx < call.argStringValues.size())
                        {
                            const std::string_view argVal = call.argStringValues[idx];
                            RegisterReflectedSymbol(argVal, req, result, pool);
                        }
                    }
                }
            }
        }
    }
    return result;
}

} // namespace angel_lsp::analysis::rules
