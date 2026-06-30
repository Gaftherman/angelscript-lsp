/**
 * @file HoverHandler.h
 * @brief Processes the syntactic and semantic context for Language Server Protocol (LSP) hover requests.
 * @author AngelScript LSP Team
 */

#ifndef HOVER_HANDLER_H
#define HOVER_HANDLER_H

#include <string>
#include <string_view>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <nlohmann/json.hpp>
#include <angelscript.h>

using json = nlohmann::json;

namespace LspCore
{
    /**
     * @struct TokenInfo
     * @brief Contains structural and positioning information for an individual token.
     */
    struct TokenInfo
    {
        std::string_view text;
        asETokenClass tokenClass;
        size_t startPos;
        size_t endPos;
        int line;
        int character;
    };

    /**
     * @struct DeclInfo
     * @brief Stores information regarding a resolved code declaration.
     */
    struct DeclInfo
    {
        std::string name;
        std::string fullName;
        std::string type;
        std::string hoverText;
        size_t startPos = 0;
        size_t endPos = std::string::npos;
        size_t defPos = 0;
    };

    /**
     * @struct ScopeRange
     * @brief Represents the boundaries and identifiers of a specific block scope.
     */
    struct ScopeRange
    {
        std::string type;
        std::string name;
        std::string fullName;
        size_t startPos;
        size_t endPos;
    };

    /**
     * @struct ScopeFrame
     * @brief Represents an active frame in the scope stack during structural parsing.
     */
    struct ScopeFrame
    {
        std::string type;
        std::string name;
        size_t openBraceIdx;
        size_t startPos;
    };

    namespace Traversal
    {
        /**
         * @brief Shared token traversal helper mapping lambda functions.
         */
        template <typename TToken, typename TAction>
        void TraverseScriptTokens(std::vector<TToken> &tokens, TAction specialAction)
        {
            for (size_t k = 0; k < tokens.size(); ++k)
            {
                if (tokens[k].text == "function")
                {
                    specialAction(tokens[k], k);
                }
            }
        }
    }

    /**
     * @class BaseResolver
     * @brief Abstract base pipeline handling syntax token analysis and grammar rule mappings.
     */
    class BaseResolver
    {
    public:
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

        BaseResolver(asIScriptEngine *engine, std::string_view sourceCode, int line, int character);
        virtual ~BaseResolver() = default;

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
        std::string DeduceTypeFromRHS(size_t startIdx);

        void RunParserPipeline();
    };
}

/**
 * @class HoverHandler
 * @brief Handles the parsing, tokenization, and semantic analysis of source code to provide hover information.
 */
class HoverHandler : public LspCore::BaseResolver
{
public:
    /**
     * @brief Constructs a HoverHandler instance with the parameters required to evaluate the cursor position.
     * @param engine Pointer to the native AngelScript script engine context.
     * @param sourceCode The full source code string view being inspected.
     * @param line The row coordinate of the cursor.
     * @param character The column coordinate of the cursor.
     */
    HoverHandler(asIScriptEngine *engine, std::string_view sourceCode, int line, int character);

    /**
     * @brief Executes the complete analysis pipeline and returns the resulting LSP node.
     * @return A json object containing the formatted hover text response.
     */
    json Process();

private:
    std::string CleanSignature(std::string str);
    void NormalizeSignatureSpacing(std::string &signature) const;
    bool IsKeywordHoverException(std::string_view tokenText) const noexcept;
    std::string EnhanceIfFuncdef(const std::string &hoverText);
    std::string SemanticValidationPass(int targetIdx);
};

/**
 * @namespace HoverUtils
 * @brief Contains internal parameter tracking types and text scanning helpers for hover resolution.
 */
namespace HoverUtils
{
    /**
     * @struct FuncParam
     * @brief File-scoped parameter metadata placeholder ensuring stable MSVC template deduction guides.
     */
    struct FuncParam
    {
        std::string pName;
        std::string pType;
        size_t defPos = 0;
    };

    /**
     * @brief Abstracted helper scanning sequential multi-variable chains separating by commas on a single expression line.
     * @param allTokens Vector containing the fully tokenized representation of the document.
     * @param startIdx The initial token index position to start scanning from.
     * @param boundaryLimit The maximum search boundary index within the token vector.
     * @return The terminal index matching the completion or delimiter of the variable chain.
     */
    size_t ScanInlineChainedDeclarations(const std::vector<LspCore::TokenInfo> &allTokens, size_t startIdx, size_t boundaryLimit) noexcept;
}

#endif // HOVER_HANDLER_H