/**
 * @file HoverHandler.h
 * @brief Processes the syntactic and semantic context for Language Server Protocol (LSP) hover requests.
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
        std::string text;
        asETokenClass tokenClass;
        size_t startPos;
        size_t endPos;
        int line;
        int character;
    };

    /**
     * @struct DeclInfo
     * @brief Stores information regarding a resolved code declaration (e.g., variables, functions).
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
     * @param sourceCode The full source code string being inspected.
     * @param line The row coordinate of the cursor.
     * @param character The column coordinate of the cursor.
     */
    HoverHandler(asIScriptEngine *engine, const std::string &sourceCode, int line, int character);

    /**
     * @brief Executes the complete analysis pipeline and returns the resulting LSP node.
     * @return A json object containing the formatted hover text response.
     */
    json Process();

private:
    asIScriptEngine *nativeEng;
    std::string originalText;
    int targetLine;
    int targetCharacter;

    std::vector<TokenInfo> allTokens;
    std::vector<DeclInfo> declarations;
    std::vector<ScopeRange> structuralScopes;
    std::unordered_map<std::string, std::vector<std::string>> classInheritanceMapper;
    std::vector<std::string> tokenScopePrefixes;

    static const std::unordered_set<std::string> reservedKeywords;
    static const std::unordered_set<std::string> contextualKeywords;
    static const std::unordered_set<std::string> primitiveTypes;
    static const std::unordered_set<std::string> storageModifiers;

    /**
     * @brief Validates if a given string token matches a standard word structure.
     * @param s The string expression to evaluate.
     * @return true if the string is a valid word, false otherwise.
     */
    bool IsWord(const std::string &s);

    /**
     * @brief Cleans and normalizes a signature string for uniform comparison.
     * @param str The raw signature string.
     * @return The formatted and sanitized signature string.
     */
    std::string CleanSignature(std::string str);

    /**
     * @brief Parses a datatype from a given token index.
     * @param startIdx The token stream index where parsing begins.
     * @param nextIdx Output parameter tracking the next valid token position after parsing.
     * @param typeStr Output parameter receiving the extracted datatype description string.
     * @return true if a valid type syntax is successfully processed, false otherwise.
     */
    bool ParseType(size_t startIdx, size_t &nextIdx, std::string &typeStr);

    /**
     * @brief Runs the initial lexicon pass to populate the internal token list.
     */
    void TokenizePass();

    /**
     * @brief Resolves the specific token index located under the target cursor position.
     * @return The matching token index, or -1 if no token matches the coordinates.
     */
    int FindTargetTokenIdx();

    /**
     * @brief Analyzes curly braces and indentation structures to map out localized code scopes.
     */
    void StructuralParsingPass();

    /**
     * @brief Scans tokens to extract and catalog explicit code declarations.
     */
    void ExtractDeclarationsPass();

    /**
     * @brief Identifies internal lambda formulas and tracks their local declarations.
     */
    void ExtractLambdasPass();

    /**
     * @brief Evaluates code semantics at the target index to resolve contextual descriptions.
     * @param targetIdx The token index to validate.
     * @return A string containing the extracted markdown documentation or type signature.
     */
    std::string SemanticValidationPass(int targetIdx);
};

#endif // HOVER_HANDLER_H