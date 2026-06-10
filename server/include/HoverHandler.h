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
    bool IsWord(const std::string_view &s) const;

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
     * @brief Main processing pipeline dispatcher. Iterates and tracks global tokens based on the SCRIPT grammar production.
     */
    void ProcessScriptRule();

    /**
     * @brief Isolates the token stream parsing tracking nested namespace identifiers.
     * @param openBraceIdx The index of the open curly brace for the namespace scope.
     * @param fName The name identifier of the namespace.
     * @param currentPrefix The active fully-qualified prefix path.
     */
    void ProcessNamespaceRule(size_t openBraceIdx, const std::string &fName, const std::string &currentPrefix);

    /**
     * @brief Captures enum symbols and registers constants with incremental value assignments.
     * @param openBraceIdx The index of the open curly brace for the enum scope.
     * @param fName The name identifier of the enum.
     * @param currentPrefix The active fully-qualified prefix path.
     */
    void ProcessEnumRule(size_t openBraceIdx, const std::string &fName, const std::string &currentPrefix);

    /**
     * @brief Registers funcdef token scopes as functional blueprints for function pointer trackers.
     * @param idxScan Mutable reference to the current token stream scanning index position.
     * @param currentPrefix The active fully-qualified prefix path.
     */
    void ProcessFuncDefRule(size_t &idxScan, const std::string &currentPrefix);

    /**
     * @brief Maps class structures, interfaces, and mixins, keeping track of inheritance rules.
     * @param openBraceIdx The index of the open curly brace for the class scope.
     * @param fName The name identifier of the class or interface structure.
     * @param fType The structural declaration string subtype.
     * @param currentPrefix The active fully-qualified prefix path.
     * @param lookaheadIdx The identifier index location for scanning ancestors.
     */
    void ProcessClassAndInterfaceRule(size_t openBraceIdx, const std::string &fName, const std::string &fType, const std::string &currentPrefix, size_t lookaheadIdx);

    /**
     * @brief Mapped to the EBNF LAMBDA spec. Isolates inner closures and tracks variable lifetimes.
     * @param k Mutable reference tracking the internal cursor position within the token stream.
     */
    void ProcessLambdaRule(size_t &k);

    /**
     * @brief Decouples statements from control expressions, routing tokens to loops or constructs.
     * @param idxScan Mutable reference to the current token index being parsed.
     */
    void ProcessStatementRule(size_t &idxScan);

    /**
     * @brief Processes traditional for loop declarations and extracts internal iteration variables.
     * @param idxScan Mutable reference to the token stream index tracking execution.
     */
    void ProcessForLoopRule(size_t &idxScan);

    /**
     * @brief Processes modern foreach loop headers to deduce and track element types from containers.
     * @param idxScan Mutable reference to the token stream index tracking execution.
     */
    void ProcessForeachLoopRule(size_t &idxScan);

    /**
     * @brief Evaluates code semantics at the target index to resolve contextual descriptions.
     * @param targetIdx The token index to validate.
     * @return A string containing the extracted markdown documentation or type signature.
     */
    std::string SemanticValidationPass(int targetIdx);

    /**
     * @brief Evaluates the expression on the right-hand side of an assignment to infer types for auto.
     * @param startIdx The token index where the right-hand side expression begins.
     * @return The deduced type string value.
     */
    std::string DeduceTypeFromRHS(size_t startIdx);

    /**
     * @brief Appends signature blueprints automatically when looking up customized functional pointer keywords.
     * @param hoverText The original markdown baseline string.
     * @return The customized, appended signature block.
     */
    std::string EnhanceIfFuncdef(const std::string &hoverText);

    /**
     * @brief Matches full qualifier rules to determine structural ancestry constraints.
     * @param decl Structural information containing full validation signatures.
     * @param fullQualName The current target qualification name context string.
     * @return true if qualification constraint requirements are fully met, false otherwise.
     */
    bool MatchesQual(const DeclInfo &decl, const std::string &fullQualName) const;

    // =========================================================================
    // HIGH-PERFORMANCE ABSTRACTED ZERO-ALLOCATION UTILITIES & PREDIATCES
    // =========================================================================

    /**
     * @brief Scans and strips visibility/structural prefixes from signatures dynamically.
     * @param typeStr The original source type string view to process.
     * @return A zero-allocation view of the baseline underlying datatype.
     */
    std::string_view StripAccessModifiers(std::string_view typeStr) noexcept;

    /**
     * @brief Isolates the pure underlying typename from wrappers, templates, references, or handles.
     * @param typeStr Typename description context string view.
     * @return String representing the unadorned baseline identifier name.
     */
    std::string ExtractBaseTypeName(std::string_view typeStr);

    /**
     * @brief Evaluates if a specified token represents a targeted system hover skip rule.
     * @param tokenText Token value being evaluated.
     * @return true if context meets restricted structural bypass constraints.
     */
    bool IsKeywordHoverException(std::string_view tokenText) const noexcept;

    /**
     * @brief Evaluates if a declaration label belongs to executable functions or structured properties.
     * @param typeLabel Semantic tag description to evaluate.
     * @return true if label matches active call or semantic structures.
     */
    bool IsCallableOrFieldSemantic(std::string_view typeLabel) const noexcept;

    /**
     * @brief Evaluates whether a signature contains an un-deduced placeholder type.
     * @param typeStr Contextual description signature view.
     * @return true if signature demands processing by the deduction sub-engine.
     */
    bool IsAutoDeducibleType(std::string_view typeStr) const noexcept;

    /**
     * @brief Evaluates if a given token fits the structural grammar definitions allowed in a template parameter list.
     * @param tokenText Token literal context view.
     * @return true if token falls within baseline valid EBNF boundaries.
     */
    bool IsValidTemplateToken(std::string_view tokenText) const noexcept;

    /**
     * @brief Normalizes formatting footprints inside signature chains to bypass allocation spikes.
     * @param signature Reference targeting the mutable output sequence description string.
     */
    void NormalizeSignatureSpacing(std::string &signature) const;

    /**
     * @brief Categorizes if a token text matches a structural type declaration keyword.
     * @param txt String context being evaluated.
     * @return true if text matches structure keywords, false otherwise.
     */
    bool IsStructureDeclarationKeyword(std::string_view txt) const;

    /**
     * @brief Categorizes if a token text matches standard control statement sequences.
     * @param txt String context being evaluated.
     * @return true if text matches statement keywords, false otherwise.
     */
    bool IsStatementKeyword(std::string_view txt) const;

    /**
     * @brief Categorizes if a token text represents restricted system storage boundaries.
     * @param txt String context being evaluated.
     * @return true if text matches modifier keywords, false otherwise.
     */
    bool IsStorageModifierKeyword(std::string_view txt) const;

    /**
     * @brief Categorizes if a token text maps directly to standard fundamental system types.
     * @param txt String context being evaluated.
     * @return true if text matches primitive keywords, false otherwise.
     */
    bool IsPrimitiveType(std::string_view txt) const;
};

#endif // HOVER_HANDLER_H