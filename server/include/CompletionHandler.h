/**
 * @file CompletionHandler.h
 * @brief Encapsulates the semantic resolution and item generation for LSP autocompletion.
 */

#pragma once

#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include <angelscript.h>
#include "TokenHarvester.h"

/**
 * @class CompletionHandler
 * @brief State-driven handler that processes context chains and AST data to provide completion items.
 */
class CompletionHandler
{
private:
    asIScriptEngine *engine;
    asIScriptModule *module;
    TokenHarvester::CompletionContext ctx;
    std::string enclosingClass;
    std::vector<TokenHarvester::LocalVariable> localVars;
    std::vector<TokenHarvester::ScriptClass> customClasses;
    std::vector<TokenHarvester::GlobalFunction> tokenFuncs;
    std::vector<TokenHarvester::GlobalVariable> tokenGlobalVars;

    std::function<void(const std::string &)> logger;

    nlohmann::json itemsArray;

public:
    /**
     * @brief Constructs a new CompletionHandler instance to resolve contextual autocompletion suggestions.
     * @param eng Pointer to the active AngelScript engine instance.
     * @param mod Pointer to the active AngelScript module.
     * @param context The natively resolved context for autocompletion triggering.
     * @param encClass The name of the enclosing class, if the cursor is within a class scope.
     * @param locals A collection of local variables active at the current cursor position.
     * @param classes A collection of user-defined script classes available in the scope.
     * @param funcs A collection of globally defined script functions.
     * @param globals A collection of globally defined variables.
     */
    CompletionHandler(asIScriptEngine *eng, asIScriptModule *mod,
                      const TokenHarvester::CompletionContext &context,
                      const std::string &encClass,
                      const std::vector<TokenHarvester::LocalVariable> &locals,
                      const std::vector<TokenHarvester::ScriptClass> &classes,
                      const std::vector<TokenHarvester::GlobalFunction> &funcs,
                      const std::vector<TokenHarvester::GlobalVariable> &globals,
                      std::function<void(const std::string &)> logFn = nullptr);

    /**
     * @brief Evaluates the current context and returns the populated JSON array of completion items.
     * @param originalText The raw document text being analyzed.
     * @param cursorAbsPos The absolute linear index position of the cursor within the text.
     * @return A JSON array containing LSP-compliant completion items.
     */
    nlohmann::json GenerateItems(const std::string &originalText, size_t cursorAbsPos);

private:
    /**
     * @brief Evaluates a member access chain and populates autocompletion items for the natively resolved object type.
     */
    void HandleMemberAccess();

    /**
     * @brief Evaluates variables, functions, and keywords active at the current cursor scope, populating the completion items.
     * @param originalText The raw string representation of the source code being analyzed.
     * @param cursorAbsPos The absolute linear index position of the cursor within the text.
     */
    void HandleGlobalScope(const std::string &originalText, size_t cursorAbsPos);

    /**
     * @brief Determines the initial data type at the root of a member access chain.
     * @param rootName The identifier name of the root object or function.
     * @param rootIsMethod Flag indicating whether the root identifier represents a function call.
     * @return The resolved data type name of the root object as a string.
     */
    std::string ResolveRootType(const std::string &rootName, bool rootIsMethod);

    /**
     * @brief Traverses a sequence of member accesses to determine the final evaluated type in an object chain.
     * @param inferredTypeName The baseline type name starting the chain evaluation.
     * @return The final evaluated data type name after fully traversing the chain.
     */
    std::string WalkObjectChain(std::string inferredTypeName);

    /**
     * @brief Extracts class or native members of a specified type and appends them to the autocompletion items list.
     * @param inferredTypeName The fully resolved type name whose members should be retrieved.
     */
    void PopulateMembers(const std::string &inferredTypeName);

    /**
     * @brief Retrieves the native AngelScript type information object corresponding to a given type name.
     * @param typeName The string representation of the data type to query.
     * @return A pointer to the corresponding asITypeInfo structure, or nullptr if the type cannot be found.
     */
    asITypeInfo *GetNativeTypeInfo(const std::string &typeName);

    /**
     * @brief Parses a discrete textual segment of an object chain into its logical base identifier and applied modifiers.
     * @param segment The raw text fragment from the chain to analyze (e.g., "myArray[0]" or "myFunction()").
     * @param outName Output parameter populated with the base identifier string after parsing.
     * @param outDerefCount Output parameter tracking the number of array dereference modifiers found.
     * @param outIsMethod Output boolean flag asserting true if the segment identifies as a method or function call.
     */
    void ParseSegment(const std::string &segment, std::string &outName, int &outDerefCount, bool &outIsMethod);
};