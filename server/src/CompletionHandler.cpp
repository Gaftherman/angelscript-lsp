/**
 * @file CompletionHandler.cpp
 * @brief Implementation of semantic type resolution for autocompletion.
 */

#include "CompletionHandler.h"
#include <unordered_map>
#include <fmt/core.h>

using json = nlohmann::json;

/**
 * @brief Constructs a new Completion Handler object to resolve contextual autocompletion suggestions.
 * @param eng Pointer to the active AngelScript engine.
 * @param mod Pointer to the active AngelScript module.
 * @param context The natively resolved context for autocompletion triggering.
 * @param encClass The name of the enclosing class, if the cursor is within a class scope.
 * @param locals A collection of local variables active at the current cursor position.
 * @param classes A collection of user-defined script classes available in the scope.
 * @param funcs A collection of globally defined script functions.
 * @param globals A collection of globally defined variables.
 */
CompletionHandler::CompletionHandler(asIScriptEngine* eng, asIScriptModule* mod, 
                                     const TokenHarvester::CompletionContext& context,
                                     const std::string& encClass,
                                     const std::vector<TokenHarvester::LocalVariable>& locals,
                                     const std::vector<TokenHarvester::ScriptClass>& classes,
                                     const std::vector<TokenHarvester::GlobalFunction>& funcs,
                                     const std::vector<TokenHarvester::GlobalVariable>& globals)
    : engine(eng), module(mod), ctx(context), enclosingClass(encClass), 
      localVars(locals), customClasses(classes), tokenFuncs(funcs), tokenGlobalVars(globals) {
    itemsArray = json::array();
}

/**
 * @brief Generates an array of JSON autocompletion items based on the current context.
 * @param originalText The raw string representation of the source code being analyzed.
 * @param cursorAbsPos The absolute linear index of the cursor position within the text.
 * @return A JSON array containing the completely resolved autocompletion items.
 */
json CompletionHandler::GenerateItems(const std::string& originalText, size_t cursorAbsPos) {
    if (ctx.isMemberAccess && !ctx.objectChain.empty()) {
        HandleMemberAccess();
    } else {
        HandleGlobalScope(originalText, cursorAbsPos);
    }
    return itemsArray;
}

/**
 * @brief Evaluates a member access chain and populates autocompletion items for the natively resolved object type.
 */
void CompletionHandler::HandleMemberAccess() {
    std::string rootName;
    int rootDeref = 0;
    bool rootIsMethod = false;
    ParseSegment(ctx.objectChain[0], rootName, rootDeref, rootIsMethod);

    std::string inferredTypeName = ResolveRootType(rootName, rootIsMethod);

    for (int d = 0; d < rootDeref; d++) {
        inferredTypeName = TokenHarvester::ExtractInnerType(inferredTypeName);
    }
    inferredTypeName = TokenHarvester::GetBaseType(inferredTypeName);

    inferredTypeName = WalkObjectChain(inferredTypeName);

    if (!inferredTypeName.empty()) {
        PopulateMembers(inferredTypeName);
    }
}

/**
 * @brief Determines the initial data type at the root of a member access chain.
 * @param rootName The identifier name of the root object or function.
 * @param rootIsMethod Flag indicating whether the root identifier represents a function call.
 * @return The resolved data type name of the root object as a string.
 */
std::string CompletionHandler::ResolveRootType(const std::string& rootName, bool rootIsMethod) {
    bool isStaticAccess = false;
    for (const auto& c : customClasses) {
        if (c.name == rootName) { isStaticAccess = true; break; }
    }
    if (!isStaticAccess && GetNativeTypeInfo(rootName) != nullptr) {
        isStaticAccess = true;
    }

    if (rootIsMethod) {
        if (isStaticAccess) {
            return rootName; 
        }

        for (const auto& f : tokenFuncs) {
            if (f.name == rootName) return f.typeName;
        }
        for (asUINT f = 0; f < engine->GetGlobalFunctionCount(); f++) {
            asIScriptFunction* func = engine->GetGlobalFunctionByIndex(f);
            if (func && std::string(func->GetName()) == rootName) {
                int typeId = func->GetReturnTypeId();
                const char* decl = engine->GetTypeDeclaration(typeId, true);
                return decl ? decl : "";
            }
        }
        return "";
    } 
    
    if (isStaticAccess) return rootName;
    if (rootName == "this" && !enclosingClass.empty()) return enclosingClass;

    for (const auto& v : localVars) { if (v.name == rootName) return v.typeName; }
    for (const auto& v : tokenGlobalVars) { if (v.name == rootName) return v.typeName; }
    
    for (asUINT g = 0; g < engine->GetGlobalPropertyCount(); g++) {
        const char* varName = nullptr; 
        int typeId = 0;
        engine->GetGlobalPropertyByIndex(g, &varName, nullptr, &typeId, nullptr);
        if (varName && std::string(varName) == rootName) {
            const char* decl = engine->GetTypeDeclaration(typeId, true);
            return decl ? decl : "";
        }
    }
    return "";
}

/**
 * @brief Traverses a sequence of member accesses to determine the final evaluated type in an object chain.
 * @param inferredTypeName The baseline type name starting the chain evaluation.
 * @return The final evaluated data type name after fully traversing the chain.
 */
std::string CompletionHandler::WalkObjectChain(std::string inferredTypeName) {
    for (size_t i = 1; i < ctx.objectChain.size(); ++i) {
        if (inferredTypeName.empty()) break;
        
        std::string nextName;
        int nextDeref = 0;
        bool nextIsMethod = false;
        ParseSegment(ctx.objectChain[i], nextName, nextDeref, nextIsMethod);

        std::string nextType = "";
        bool foundInScript = false;

        for (const auto& c : customClasses) {
            if (c.name == inferredTypeName) {
                if (!nextIsMethod) {
                    for (const auto& prop : c.properties) {
                        if (prop.name == nextName) { nextType = prop.typeName; foundInScript = true; break; }
                    }
                }
                if (nextType.empty()) {
                    for (const auto& method : c.methods) {
                        if (method.name == nextName) { nextType = method.typeName; foundInScript = true; break; }
                    }
                }
                break;
            }
        }

        if (!foundInScript) {
            asITypeInfo* t = GetNativeTypeInfo(inferredTypeName);
            if (t) {
                if (!nextIsMethod) {
                    for (asUINT p = 0; p < t->GetPropertyCount(); p++) {
                        const char* pName = nullptr; int pTypeId = 0;
                        t->GetProperty(p, &pName, &pTypeId);
                        if (pName && std::string(pName) == nextName) {
                            const char* decl = engine->GetTypeDeclaration(pTypeId, true);
                            if (decl) nextType = decl;
                            break;
                        }
                    }
                }
                if (nextType.empty()) {
                    for (asUINT m = 0; m < t->GetMethodCount(); m++) {
                        asIScriptFunction* func = t->GetMethodByIndex(m);
                        if (func && std::string(func->GetName()) == nextName) {
                            int rTypeId = func->GetReturnTypeId();
                            const char* decl = engine->GetTypeDeclaration(rTypeId, true);
                            if (decl) nextType = decl;
                            break;
                        }
                    }
                }
            }
        }
        
        if (!nextType.empty()) {
            for(int d = 0; d < nextDeref; d++) {
                nextType = TokenHarvester::ExtractInnerType(nextType);
            }
            inferredTypeName = TokenHarvester::GetBaseType(nextType);
        } else {
            inferredTypeName = "";
        }
    }
    return inferredTypeName;
}

/**
 * @brief Extracts class or native members of a specified type and appends them to the autocompletion items list.
 * @param inferredTypeName The fully resolved type name whose members should be retrieved.
 */
void CompletionHandler::PopulateMembers(const std::string& inferredTypeName) {
    bool classFoundInScript = false;
    bool canAccessPrivate = (enclosingClass == inferredTypeName);

    for (const auto& c : customClasses) {
        if (c.name == inferredTypeName) {
            classFoundInScript = true;
            for (const auto& prop : c.properties) {
                if (!canAccessPrivate && (prop.access == "private" || prop.access == "protected")) continue;
                if (ctx.partialMember.empty() || prop.name.rfind(ctx.partialMember, 0) == 0) {
                    itemsArray.push_back({{"label", prop.name}, {"kind", 5}, {"detail", prop.access + " " + prop.typeName}});
                }
            }
            for (const auto& method : c.methods) {
                if (method.isConstructor) continue; 
                if (!canAccessPrivate && (method.access == "private" || method.access == "protected")) continue;
                if (ctx.partialMember.empty() || method.name.rfind(ctx.partialMember, 0) == 0) {
                    itemsArray.push_back({
                        {"label", method.name}, {"kind", 2}, 
                        {"detail", method.access + " " + method.declaration},
                        {"insertText", method.name + "($1)"}, {"insertTextFormat", 2}
                    });
                }
            }
            break;
        }
    }

    if (!classFoundInScript) {
        asITypeInfo* targetType = GetNativeTypeInfo(inferredTypeName);
        if (targetType) {
            if (targetType->GetFlags() & asOBJ_ENUM) {
                for (asUINT v = 0; v < targetType->GetEnumValueCount(); v++) {
                    const char* enumName = targetType->GetEnumValueByIndex(v, nullptr);
                    if (enumName) {
                        if (ctx.partialMember.empty() || std::string(enumName).rfind(ctx.partialMember, 0) == 0) {
                            itemsArray.push_back({{"label", enumName}, {"kind", 12}, {"detail", "enum value"}});
                        }
                    }
                }
            }

            for (asUINT p = 0; p < targetType->GetPropertyCount(); p++) {
                const char* propName = nullptr; int propTypeId = 0;
                targetType->GetProperty(p, &propName, &propTypeId);
                if (ctx.partialMember.empty() || std::string(propName).rfind(ctx.partialMember, 0) == 0) {
                    const char* decl = engine->GetTypeDeclaration(propTypeId, true);
                    itemsArray.push_back({{"label", propName}, {"kind", 5}, {"detail", decl ? decl : "primitive"}});
                }
            }
            for (asUINT m = 0; m < targetType->GetMethodCount(); m++) {
                asIScriptFunction* func = targetType->GetMethodByIndex(m);
                std::string mName = func->GetName();
                
                if (mName == inferredTypeName || (!mName.empty() && mName[0] == '~')) continue;

                if (ctx.partialMember.empty() || mName.rfind(ctx.partialMember, 0) == 0) {
                    itemsArray.push_back({
                        {"label", mName}, {"kind", 2}, 
                        {"detail", func->GetDeclaration(true, false, true)},
                        {"insertText", mName + "($1)"}, {"insertTextFormat", 2}
                    });
                }
            }
        }
    }
}

/**
 * @brief Evaluates variables, functions, and keywords active at the current cursor scope, populating the JSON list.
 * @param originalText The raw string representation of the source code being analyzed.
 * @param cursorAbsPos The absolute linear index of the cursor position within the text.
 */
void CompletionHandler::HandleGlobalScope(const std::string& originalText, size_t cursorAbsPos) {
    if (cursorAbsPos > 0 && originalText[cursorAbsPos - 1] == '@') return;

    std::vector<std::string> keywords = {"class", "interface", "void", "int", "float", "string", "array", "bool", "if", "else", "for", "while", "return", "this", "auto", "const", "private", "protected", "override", "final", "namespace", "enum"};
    for (const auto& kw : keywords) itemsArray.push_back({{"label", kw}, {"kind", 14}, {"detail", "keyword"}});
    
    for (const auto& v : localVars) itemsArray.push_back({{"label", v.name}, {"kind", 6}, {"detail", "local " + v.typeName}});
    
    if (!enclosingClass.empty()) {
        for (const auto& c : customClasses) {
            if (c.name == enclosingClass) {
                for (const auto& prop : c.properties) {
                    itemsArray.push_back({
                        {"label", prop.name}, {"kind", 5}, 
                        {"detail", prop.access + " " + prop.typeName},
                        {"documentation", {{"kind", "markdown"}, {"value", fmt::format("Member property of `{}`", enclosingClass)}}}
                    });
                }
                for (const auto& method : c.methods) {
                    if (method.isConstructor) continue;
                    itemsArray.push_back({
                        {"label", method.name}, {"kind", 2}, 
                        {"detail", method.access + " " + method.declaration},
                        {"documentation", {{"kind", "markdown"}, {"value", fmt::format("Member method of `{}`", enclosingClass)}}}
                    });
                }
                break;
            }
        }
    }

    for (const auto& c : customClasses) {
        itemsArray.push_back({
            {"label", c.name}, {"kind", 7}, {"detail", "class/namespace/enum " + c.name},
            {"documentation", {{"kind", "markdown"}, {"value", "User-defined script type"}}}
        });
    }

    std::unordered_map<std::string, bool> addedFunctions;

    if (module) {
        for (asUINT f = 0; f < module->GetFunctionCount(); f++) {
            asIScriptFunction* func = module->GetFunctionByIndex(f);
            if (func) {
                std::string funcName = func->GetName(); addedFunctions[funcName] = true;
                itemsArray.push_back({
                    {"label", funcName}, {"kind", 3}, 
                    {"detail", func->GetDeclaration(true, false, true)},
                    {"insertText", funcName + "($1)"}, {"insertTextFormat", 2}
                });
            }
        }
        for (asUINT g = 0; g < module->GetGlobalVarCount(); g++) {
            const char* varName = nullptr; int typeId = 0;
            module->GetGlobalVar(g, &varName, nullptr, &typeId);
            if (varName) itemsArray.push_back({ {"label", varName}, {"kind", 6}, {"detail", "Global variable"} });
        }
    }

    for (const auto& tf : tokenFuncs) {
        if (!addedFunctions[tf.name]) {
            addedFunctions[tf.name] = true;
            itemsArray.push_back({
                {"label", tf.name}, {"kind", 3}, {"detail", tf.declaration},
                {"insertText", tf.name + "($1)"}, {"insertTextFormat", 2}
            });
        }
    }

    for (const auto& nativeVar : tokenGlobalVars) {
        bool exists = false;
        for (const auto& item : itemsArray) { if (item.contains("label") && item["label"] == nativeVar.name) { exists = true; break; } }
        if (!exists) itemsArray.push_back({ {"label", nativeVar.name}, {"kind", 6}, {"detail", nativeVar.typeName + " " + nativeVar.name} });
    }

    for (asUINT f = 0; f < engine->GetGlobalFunctionCount(); f++) {
        asIScriptFunction* func = engine->GetGlobalFunctionByIndex(f);
        if (func) {
            std::string funcName = func->GetName();
            if (!addedFunctions[funcName]) {
                itemsArray.push_back({
                    {"label", funcName}, {"kind", 3}, 
                    {"detail", func->GetDeclaration(true, false, true)},
                    {"insertText", funcName + "($1)"}, {"insertTextFormat", 2}
                });
            }
        }
    }

    for (asUINT g = 0; g < engine->GetGlobalPropertyCount(); g++) {
        const char* varName = nullptr;
        engine->GetGlobalPropertyByIndex(g, &varName, nullptr, nullptr, nullptr);
        if (varName) itemsArray.push_back({ {"label", varName}, {"kind", 6}, {"detail", "Native global"} });
    }
}

/**
 * @brief Retrieves the native AngelScript type information object corresponding to a given type name.
 * @param typeName The string representation of the data type to query.
 * @return A pointer to the corresponding asITypeInfo structure, or nullptr if the type cannot be found.
 */
asITypeInfo* CompletionHandler::GetNativeTypeInfo(const std::string& typeName) {
    if (typeName.empty()) return nullptr;
    asITypeInfo* t = engine->GetTypeInfoByName(typeName.c_str());
    if (!t && module) t = module->GetTypeInfoByName(typeName.c_str());
    if (!t) {
        for(asUINT idx = 0; idx < engine->GetObjectTypeCount(); idx++) {
            asITypeInfo* objType = engine->GetObjectTypeByIndex(idx);
            if (objType && std::string(objType->GetName()) == typeName) return objType;
        }
    }
    return t;
}

/**
 * @brief Parses a discrete textual segment of an object chain into its logical base identifier and applied modifiers.
 * @param segment The raw text fragment from the chain to analyze (e.g., "myArray[0]" or "myFunction()").
 * @param outName Output parameter populated with the base identifier string after parsing.
 * @param outDerefCount Output parameter tracking the number of array dereference modifiers found.
 * @param outIsMethod Output boolean flag asserting true if the segment identifies as a method or function call.
 */
void CompletionHandler::ParseSegment(const std::string& segment, std::string& outName, int& outDerefCount, bool& outIsMethod) {
    outName = segment;
    outDerefCount = 0;
    outIsMethod = false;
    
    for (char c : segment) { if (c == '[') outDerefCount++; }
    
    size_t cutoff = outName.find_first_of("([");
    if (cutoff != std::string::npos) {
        if (outName[cutoff] == '(') outIsMethod = true;
        outName = outName.substr(0, cutoff);
    }
}