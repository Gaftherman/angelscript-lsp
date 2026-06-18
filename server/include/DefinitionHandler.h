/**
 * @file DefinitionHandler.h
 * @brief Context-aware Language Server Protocol definition mapping controller layout.
 * @author AngelScript LSP Team
 */

#ifndef DEFINITION_HANDLER_H
#define DEFINITION_HANDLER_H

#include <string_view>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <nlohmann/json.hpp>
#include <angelscript.h>

using json = nlohmann::json;

/**
 * @namespace DefinitionUtils
 * @brief Contains internal helper structures and utilities for LSP definition resolution.
 */
namespace DefinitionUtils
{
    /**
     * @struct FuncParam
     * @brief Represents a parsed function or method parameter details.
     */
    struct FuncParam
    {
        std::string pName;
        std::string pType;
    };

    /**
     * @class DefinitionResolver
     * @brief Internal engine pipeline that tokenizes and parses source states to map definitions.
     */
    class DefinitionResolver
    {
    public:
        struct TokenInfo
        {
            std::string_view text;
            asETokenClass tokenClass;
            size_t startPos;
            size_t endPos;
            int line;
            int character;
        };

        struct DeclInfo
        {
            std::string name;
            std::string fullName;
            std::string type;
            std::string hoverText;
            size_t scopeStart = 0;
            size_t scopeEnd = std::string::npos;
            size_t defPos = 0;
        };

        struct ScopeRange
        {
            std::string type;
            std::string name;
            std::string fullName;
            size_t startPos;
            size_t endPos;
        };

        struct ScopeFrame
        {
            std::string type;
            std::string name;
            size_t openBraceIdx;
            size_t startPos;
        };

        asIScriptEngine *nativeEng;
        std::string_view originalText;
        int targetLine;
        int targetCharacter;

        std::vector<TokenInfo> allTokens;
        std::vector<DeclInfo> declarations;
        std::vector<ScopeRange> structuralScopes;
        std::unordered_map<std::string, std::vector<std::string>> classInheritanceMapper;
        std::vector<std::string> tokenScopePrefixes;

        static const std::unordered_set<std::string_view> reservedKeywords;
        static const std::unordered_set<std::string_view> contextualKeywords;
        static const std::unordered_set<std::string_view> primitiveTypes;
        static const std::unordered_set<std::string_view> storageModifiers;
        static const std::unordered_set<std::string_view> structureKeywords;
        static const std::unordered_set<std::string_view> statementKeywords;

        /**
         * @brief Constructor initializing the tracking context fields.
         */
        DefinitionResolver(asIScriptEngine *engine, std::string_view sourceCode, int line, int character);

        bool IsStructureDeclarationKeyword(std::string_view txt) const noexcept;
        bool IsStatementKeyword(std::string_view txt) const noexcept;
        bool IsStorageModifierKeyword(std::string_view txt) const noexcept;
        bool IsPrimitiveType(std::string_view txt) const noexcept;
        bool IsWord(std::string_view s) const noexcept;
        bool IsCallableOrFieldSemantic(std::string_view typeLabel) const noexcept;
        bool IsAutoDeducibleType(std::string_view typeStr) const noexcept;
        bool IsValidTemplateToken(std::string_view tokenText) const noexcept;

        std::string_view StripAccessModifiers(std::string_view typeStr) noexcept;
        std::string_view ExtractBaseTypeName(std::string_view typeStr) noexcept;
        bool MatchesQual(const DeclInfo &decl, const std::string &fullQualName) const noexcept;

        void TokenizePass();
        int FindTargetTokenIdx() noexcept;
        bool ParseType(size_t startIdx, size_t &nextIdx, std::string &typeStr);
        void StructuralParsingPass();
        void ProcessEnumRule(size_t openBraceIdx, const std::string &fName, const std::string &currentPrefix);
        void ProcessClassAndInterfaceRule(size_t openBraceIdx, const std::string &fName, const std::string &fType, const std::string &currentPrefix, size_t lookaheadIdx);
        void ProcessFuncDefRule(size_t &idxScan, const std::string &currentPrefix);
        void ProcessForLoopRule(size_t &idxScan);
        void ProcessForeachLoopRule(size_t &idxScan);
        void ProcessScriptRule();
        void ProcessLambdaRule(size_t &k);
        size_t ResolveDefinitionPosition(int targetIdx, size_t &outLength);
    };

    /**
     * @brief Assembles a standard LSP Location object representation in JSON format.
     * @param uri Document target absolute reference path string.
     * @param sourceCode Active loaded character stream data layout.
     * @param offset Absolute flat character coordinates position layout index.
     * @param wordLength Spanning token character allocation length metric.
     * @return Generated json object tracking bounds positioning layout metrics.
     */
    json CreateLocationJson(std::string_view uri, std::string_view sourceCode, size_t offset, size_t wordLength);
}

/**
 * @class DefinitionHandler
 * @brief Resolves definition coordinates by utilizing token harvest hierarchies matching the hover engine.
 */
class DefinitionHandler
{
public:
    DefinitionHandler() = default;
    ~DefinitionHandler() = default;

    /**
     * @brief Evaluates code state context to map the target symbol declaration location.
     * @param engine Pointer to the active core AngelScript compiler engine instance framework.
     * @param request Incoming client JSON-RPC definition request payload framing.
     * @param sourceCode Multi-line text buffer layout matching the active script document.
     * @return A json Location object tracking absolute file URI and coordinates range, or nullptr.
     */
    static json HandleDefinitionRequest(asIScriptEngine *engine, const json &request, std::string_view sourceCode);
};

#endif // DEFINITION_HANDLER_H