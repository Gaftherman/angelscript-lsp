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

/**
 * @class HoverHandler
 * @brief Handles the parsing, tokenization, and semantic analysis of source code to provide hover information.
 */
class HoverHandler
{
public:
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

    bool IsWord(std::string_view s) const noexcept;
    std::string CleanSignature(std::string str);
    bool ParseType(size_t startIdx, size_t &nextIdx, std::string &typeStr);
    void TokenizePass();
    int FindTargetTokenIdx() noexcept;
    void StructuralParsingPass();
    void ProcessScriptRule();
    void ProcessNamespaceRule(size_t openBraceIdx, const std::string &fName, const std::string &currentPrefix);
    void ProcessEnumRule(size_t openBraceIdx, const std::string &fName, const std::string &currentPrefix);
    void ProcessFuncDefRule(size_t &idxScan, const std::string &currentPrefix);
    void ProcessClassAndInterfaceRule(size_t openBraceIdx, const std::string &fName, const std::string &fType, const std::string &currentPrefix, size_t lookaheadIdx);
    void ProcessLambdaRule(size_t &k);
    void ProcessStatementRule(size_t &idxScan);
    void ProcessForLoopRule(size_t &idxScan);
    void ProcessForeachLoopRule(size_t &idxScan);
    std::string SemanticValidationPass(int targetIdx);
    std::string DeduceTypeFromRHS(size_t startIdx);
    std::string EnhanceIfFuncdef(const std::string &hoverText);
    bool MatchesQual(const DeclInfo &decl, const std::string &fullQualName) const noexcept;

    std::string_view StripAccessModifiers(std::string_view typeStr) noexcept;
    std::string_view ExtractBaseTypeName(std::string_view typeStr) noexcept;
    bool IsKeywordHoverException(std::string_view tokenText) const noexcept;
    bool IsCallableOrFieldSemantic(std::string_view typeLabel) const noexcept;
    bool IsAutoDeducibleType(std::string_view typeStr) const noexcept;
    bool IsValidTemplateToken(std::string_view tokenText) const noexcept;
    void NormalizeSignatureSpacing(std::string &signature) const;
    bool IsStructureDeclarationKeyword(std::string_view txt) const noexcept;
    bool IsStatementKeyword(std::string_view txt) const noexcept;
    bool IsStorageModifierKeyword(std::string_view txt) const noexcept;
    bool IsPrimitiveType(std::string_view txt) const noexcept;
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
    };

    /**
     * @brief Abstracted helper scanning sequential multi-variable chains separating by commas on a single expression line.
     * @param allTokens Vector containing the fully tokenized representation of the document.
     * @param startIdx The initial token index position to start scanning from.
     * @param boundaryLimit The maximum search boundary index within the token vector.
     * @return The terminal index matching the completion or delimiter of the variable chain.
     */
    size_t ScanInlineChainedDeclarations(const std::vector<HoverHandler::TokenInfo> &allTokens, size_t startIdx, size_t boundaryLimit) noexcept;
}

#endif // HOVER_HANDLER_H