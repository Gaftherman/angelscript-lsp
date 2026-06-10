/**
 * @file CompletionHandler.cpp
 * @brief Implementation of semantic type resolution for autocompletion.
 */

#include "CompletionHandler.h"
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <cctype>
#include <fmt/core.h>

using json = nlohmann::json;

/**
 * @brief Recursively searches for a member within user-defined script classes, checking base types and mixins.
 * @param typeName Name of the class scope being evaluated.
 * @param memberName Identifier of the member property or method being sought.
 * @param isMethod Assert true to filter tracking specifically for method signatures.
 * @param outType String populated with the discovered identifier type name.
 * @param customClasses Extracted translation unit context containing user class metadata indices.
 * @return True if the member resolution successfully completed within the lineage tree.
 */
static bool SearchCustomClassRecursively(const std::string &typeName,
                                         const std::string &memberName,
                                         bool isMethod,
                                         std::string &outType,
                                         const std::vector<TokenHarvester::ScriptClass> &customClasses)
{
    size_t cIdx;
    size_t pIdx;
    size_t mIdx;
    size_t bIdx;

    for (cIdx = 0; cIdx < customClasses.size(); ++cIdx)
    {
        const auto &c = customClasses[cIdx];
        if (c.name == typeName)
        {
            if (!isMethod)
            {
                for (pIdx = 0; pIdx < c.properties.size(); ++pIdx)
                {
                    if (c.properties[pIdx].name == memberName)
                    {
                        outType = c.properties[pIdx].typeName;
                        return true;
                    }
                }
            }

            if (outType.empty())
            {
                for (mIdx = 0; mIdx < c.methods.size(); ++mIdx)
                {
                    if (c.methods[mIdx].name == memberName)
                    {
                        outType = c.methods[mIdx].typeName;
                        return true;
                    }
                }
            }

            for (bIdx = 0; bIdx < c.baseTypes.size(); ++bIdx)
            {
                if (SearchCustomClassRecursively(c.baseTypes[bIdx], memberName, isMethod, outType, customClasses))
                {
                    return true;
                }
            }

            break;
        }
    }

    return false;
}

/**
 * @brief Evaluates whether a completion entry with a specific label signature already exists within the target payload array.
 * @param itemsArray Target internal JSON array structure containing populated metadata suggestions.
 * @param label Concrete literal identifier used for the identity mapping evaluation match.
 * @return True if a collision is explicitly identified within the array scope items.
 */
static inline bool ContainsItemLabel(const nlohmann::json &itemsArray, const std::string &label)
{
    size_t i;

    for (i = 0; i < itemsArray.size(); ++i)
    {
        if (itemsArray[i].contains("label") && itemsArray[i]["label"] == label)
        {
            return true;
        }
    }

    return false;
}

CompletionHandler::CompletionHandler(asIScriptEngine *eng, asIScriptModule *mod,
                                     const TokenHarvester::CompletionContext &context,
                                     const std::string &encClass,
                                     const std::vector<TokenHarvester::LocalVariable> &locals,
                                     const std::vector<TokenHarvester::ScriptClass> &classes,
                                     const std::vector<TokenHarvester::GlobalFunction> &funcs,
                                     const std::vector<TokenHarvester::GlobalVariable> &globals,
                                     std::function<void(const std::string &)> logFn)
    : engine(eng), module(mod), ctx(context), enclosingClass(encClass),
      localVars(locals), customClasses(classes), tokenFuncs(funcs), tokenGlobalVars(globals), logger(logFn)
{
    itemsArray = json::array();
}

nlohmann::json CompletionHandler::GenerateItems(const std::string &originalText, size_t cursorAbsPos)
{
    std::string precedingToken;
    bool isTypeName;
    size_t cIdx;

    isTypeName = false;

    if (ctx.lastSeparator == ":")
    {
        if (!ctx.objectChain.empty())
        {
            precedingToken = ctx.objectChain[0];

            for (cIdx = 0; cIdx < customClasses.size(); ++cIdx)
            {
                if (customClasses[cIdx].name == precedingToken)
                {
                    isTypeName = true;
                    break;
                }
            }

            if (!isTypeName && GetNativeTypeInfo(precedingToken) != nullptr)
            {
                isTypeName = true;
            }

            if (isTypeName)
            {
                return itemsArray;
            }
        }
    }

    if (ctx.isMemberAccess)
    {
        if (ctx.objectChain.empty() && ctx.lastSeparator == "::")
        {
            ComputeNamespaceScopeCompletions("");
        }
        else if (!ctx.objectChain.empty())
        {
            if (ctx.lastSeparator == "::")
            {
                ComputeNamespaceScopeCompletions(ctx.objectChain[0]);
            }
            else
            {
                ComputeMemberAccessCompletions(ctx.objectChain[0]);
            }
        }
    }
    else
    {
        ComputeLocalScopeCompletions(cursorAbsPos);
        ComputeGlobalScopeCompletions(originalText, cursorAbsPos);
    }

    return itemsArray;
}

void CompletionHandler::ComputeMemberAccessCompletions(std::string_view objectName)
{
    std::string inferredTypeName;

    inferredTypeName = DeduceTypeFromRHS(std::string(objectName));

    if (logger)
    {
        logger(fmt::format("Resolved Type from RHS: '{}'", inferredTypeName));
    }

    inferredTypeName = WalkObjectChain(inferredTypeName);

    if (!inferredTypeName.empty())
    {
        PopulateMembers(inferredTypeName);
    }
}

void CompletionHandler::ComputeNamespaceScopeCompletions(std::string_view namespacePrefix)
{
    std::unordered_map<std::string, bool> addedFunctions;
    asIScriptFunction *func;
    const char *varName;
    int typeId;
    std::string funcName;
    size_t cIdx;
    asUINT f;
    asUINT g;
    size_t tfIdx;
    size_t nvIdx;

    func = nullptr;
    varName = nullptr;
    typeId = 0;

    for (cIdx = 0; cIdx < customClasses.size(); ++cIdx)
    {
        const auto &c = customClasses[cIdx];
        if (ctx.partialMember.empty() || c.name.rfind(ctx.partialMember, 0) == 0)
        {
            itemsArray.push_back({{"label", c.name},
                                  {"kind", 7},
                                  {"detail", "class/namespace/enum " + c.name},
                                  {"documentation", {{"kind", "markdown"}, {"value", "User-defined script type"}}}});
        }
    }

    if (module)
    {
        for (f = 0; f < module->GetFunctionCount(); f++)
        {
            func = module->GetFunctionByIndex(f);
            if (func)
            {
                funcName = func->GetName();
                addedFunctions[funcName] = true;

                if (ctx.partialMember.empty() || funcName.rfind(ctx.partialMember, 0) == 0)
                {
                    itemsArray.push_back({{"label", funcName},
                                          {"kind", 3},
                                          {"detail", EnhanceIfFuncdef(CleanSignature(func->GetDeclaration(true, false, true)))},
                                          {"insertText", funcName + "($1)"},
                                          {"insertTextFormat", 2}});
                }
            }
        }

        for (g = 0; g < module->GetGlobalVarCount(); g++)
        {
            varName = nullptr;
            typeId = 0;
            module->GetGlobalVar(g, &varName, nullptr, &typeId);

            if (varName && (ctx.partialMember.empty() || std::string(varName).rfind(ctx.partialMember, 0) == 0))
            {
                itemsArray.push_back({{"label", varName},
                                      {"kind", 6},
                                      {"detail", "Global variable"}});
            }
        }
    }

    for (tfIdx = 0; tfIdx < tokenFuncs.size(); ++tfIdx)
    {
        const auto &tf = tokenFuncs[tfIdx];
        if (!addedFunctions[tf.name])
        {
            addedFunctions[tf.name] = true;

            if (ctx.partialMember.empty() || tf.name.rfind(ctx.partialMember, 0) == 0)
            {
                itemsArray.push_back({{"label", tf.name},
                                      {"kind", 3},
                                      {"detail", EnhanceIfFuncdef(CleanSignature(tf.declaration))},
                                      {"insertText", tf.name + "($1)"},
                                      {"insertTextFormat", 2}});
            }
        }
    }

    for (nvIdx = 0; nvIdx < tokenGlobalVars.size(); ++nvIdx)
    {
        const auto &nativeVar = tokenGlobalVars[nvIdx];
        if (ctx.partialMember.empty() || nativeVar.name.rfind(ctx.partialMember, 0) == 0)
        {
            if (!ContainsItemLabel(itemsArray, nativeVar.name))
            {
                itemsArray.push_back({{"label", nativeVar.name},
                                      {"kind", 6},
                                      {"detail", nativeVar.typeName + " " + nativeVar.name}});
            }
        }
    }

    for (f = 0; f < engine->GetGlobalFunctionCount(); f++)
    {
        func = engine->GetGlobalFunctionByIndex(f);
        if (func)
        {
            funcName = func->GetName();

            if (!addedFunctions[funcName])
            {
                if (ctx.partialMember.empty() || funcName.rfind(ctx.partialMember, 0) == 0)
                {
                    itemsArray.push_back({{"label", funcName},
                                          {"kind", 3},
                                          {"detail", EnhanceIfFuncdef(CleanSignature(func->GetDeclaration(true, false, true)))},
                                          {"insertText", funcName + "($1)"},
                                          {"insertTextFormat", 2}});
                }
            }
        }
    }

    for (g = 0; g < engine->GetGlobalPropertyCount(); g++)
    {
        varName = nullptr;
        engine->GetGlobalPropertyByIndex(g, &varName, nullptr, nullptr, nullptr);

        if (varName && (ctx.partialMember.empty() || std::string(varName).rfind(ctx.partialMember, 0) == 0))
        {
            itemsArray.push_back({{"label", varName},
                                  {"kind", 6},
                                  {"detail", "Native global"}});
        }
    }
}

void CompletionHandler::ComputeLocalScopeCompletions(size_t activeOffset)
{
    size_t vIdx;

    for (vIdx = 0; vIdx < localVars.size(); ++vIdx)
    {
        const auto &v = localVars[vIdx];
        if (ctx.partialMember.empty() || v.name.rfind(ctx.partialMember, 0) == 0)
        {
            itemsArray.push_back({{"label", v.name}, {"kind", 6}, {"detail", "local " + v.typeName}});
        }
    }
}

void CompletionHandler::ComputeGlobalScopeCompletions(const std::string &originalText, size_t cursorAbsPos)
{
    std::unordered_set<std::string> addedImplicitMembers;
    std::unordered_map<std::string, bool> addedFunctions;
    std::unordered_set<std::string> addedTypes;
    asIScriptFunction *func;
    std::string funcName;
    const char *varName;
    int typeId;
    std::vector<std::string> keywords;
    std::vector<std::string> primitives;
    size_t kwIdx;
    size_t primIdx;
    asUINT i;
    asUINT f;
    asUINT g;
    size_t cIdx;
    size_t tfIdx;
    size_t nvIdx;
    std::string name;

    func = nullptr;
    varName = nullptr;
    typeId = 0;

    if (cursorAbsPos > 0 && originalText[cursorAbsPos - 1] == '@')
    {
        return;
    }

    keywords = {
        "if", "else", "switch", "case", "default", "break", "continue",
        "while", "for", "foreach", "return", "class", "interface", "mixin",
        "enum", "shared", "external", "private", "protected", "import",
        "from", "cast", "is", "super", "this", "get", "set", "property",
        "const", "override", "final", "null", "true", "false", "try",
        "catch", "auto", "typedef", "funcdef"};

    for (kwIdx = 0; kwIdx < keywords.size(); ++kwIdx)
    {
        if (ctx.partialMember.empty() || keywords[kwIdx].rfind(ctx.partialMember, 0) == 0)
        {
            itemsArray.push_back({{"label", keywords[kwIdx]}, {"kind", 14}, {"detail", "keyword"}});
        }
    }

    primitives = {
        "void", "int", "int8", "int16", "int32", "int64",
        "uint", "uint8", "uint16", "uint32", "uint64",
        "float", "double", "bool"};

    for (primIdx = 0; primIdx < primitives.size(); ++primIdx)
    {
        if (ctx.partialMember.empty() || primitives[primIdx].rfind(ctx.partialMember, 0) == 0)
        {
            itemsArray.push_back({{"label", primitives[primIdx]}, {"kind", 6}, {"detail", "primitive type"}});
            addedTypes.insert(primitives[primIdx]);
        }
    }

    if (engine)
    {
        for (i = 0; i < engine->GetObjectTypeCount(); i++)
        {
            if (asITypeInfo *ti = engine->GetObjectTypeByIndex(i))
            {
                name = ti->GetName();
                if ((ctx.partialMember.empty() || name.rfind(ctx.partialMember, 0) == 0) && addedTypes.insert(name).second)
                {
                    itemsArray.push_back({{"label", name}, {"kind", 7}, {"detail", "native class"}});
                }
            }
        }

        for (i = 0; i < engine->GetEnumCount(); i++)
        {
            if (asITypeInfo *ti = engine->GetEnumByIndex(i))
            {
                name = ti->GetName();
                if ((ctx.partialMember.empty() || name.rfind(ctx.partialMember, 0) == 0) && addedTypes.insert(name).second)
                {
                    itemsArray.push_back({{"label", name}, {"kind", 13}, {"detail", "native enum"}});
                }
            }
        }

        for (i = 0; i < engine->GetTypedefCount(); i++)
        {
            if (asITypeInfo *ti = engine->GetTypedefByIndex(i))
            {
                name = ti->GetName();
                if ((ctx.partialMember.empty() || name.rfind(ctx.partialMember, 0) == 0) && addedTypes.insert(name).second)
                {
                    itemsArray.push_back({{"label", name}, {"kind", 6}, {"detail", "native typedef"}});
                }
            }
        }

        for (i = 0; i < engine->GetFuncdefCount(); i++)
        {
            if (asITypeInfo *ti = engine->GetFuncdefByIndex(i))
            {
                name = ti->GetName();
                if ((ctx.partialMember.empty() || name.rfind(ctx.partialMember, 0) == 0) && addedTypes.insert(name).second)
                {
                    itemsArray.push_back({{"label", name}, {"kind", 11}, {"detail", "native funcdef"}});
                }
            }
        }
    }

    if (!enclosingClass.empty())
    {
        AddImplicitMembersRecursive(enclosingClass, addedImplicitMembers);
    }

    for (cIdx = 0; cIdx < customClasses.size(); ++cIdx)
    {
        if (ctx.partialMember.empty() || customClasses[cIdx].name.rfind(ctx.partialMember, 0) == 0)
        {
            itemsArray.push_back({{"label", customClasses[cIdx].name},
                                  {"kind", 7},
                                  {"detail", "class/namespace/enum " + customClasses[cIdx].name},
                                  {"documentation", {{"kind", "markdown"}, {"value", "User-defined script type"}}}});
        }
    }

    if (module)
    {
        for (f = 0; f < module->GetFunctionCount(); f++)
        {
            func = module->GetFunctionByIndex(f);
            if (func)
            {
                funcName = func->GetName();
                addedFunctions[funcName] = true;

                if (ctx.partialMember.empty() || funcName.rfind(ctx.partialMember, 0) == 0)
                {
                    itemsArray.push_back({{"label", funcName},
                                          {"kind", 3},
                                          {"detail", EnhanceIfFuncdef(CleanSignature(func->GetDeclaration(true, false, true)))},
                                          {"insertText", funcName + "($1)"},
                                          {"insertTextFormat", 2}});
                }
            }
        }

        for (g = 0; g < module->GetGlobalVarCount(); g++)
        {
            varName = nullptr;
            typeId = 0;
            module->GetGlobalVar(g, &varName, nullptr, &typeId);

            if (varName && (ctx.partialMember.empty() || std::string(varName).rfind(ctx.partialMember, 0) == 0))
            {
                itemsArray.push_back({{"label", varName}, {"kind", 6}, {"detail", "Global variable"}});
            }
        }
    }

    for (tfIdx = 0; tfIdx < tokenFuncs.size(); ++tfIdx)
    {
        if (!addedFunctions[tokenFuncs[tfIdx].name])
        {
            addedFunctions[tokenFuncs[tfIdx].name] = true;

            if (ctx.partialMember.empty() || tokenFuncs[tfIdx].name.rfind(ctx.partialMember, 0) == 0)
            {
                itemsArray.push_back({{"label", tokenFuncs[tfIdx].name},
                                      {"kind", 3},
                                      {"detail", EnhanceIfFuncdef(CleanSignature(tokenFuncs[tfIdx].declaration))},
                                      {"insertText", tokenFuncs[tfIdx].name + "($1)"},
                                      {"insertTextFormat", 2}});
            }
        }
    }

    for (nvIdx = 0; nvIdx < tokenGlobalVars.size(); ++nvIdx)
    {
        if (ctx.partialMember.empty() || tokenGlobalVars[nvIdx].name.rfind(ctx.partialMember, 0) == 0)
        {
            if (!ContainsItemLabel(itemsArray, tokenGlobalVars[nvIdx].name))
            {
                itemsArray.push_back({{"label", tokenGlobalVars[nvIdx].name},
                                      {"kind", 6},
                                      {"detail", tokenGlobalVars[nvIdx].typeName + " " + tokenGlobalVars[nvIdx].name}});
            }
        }
    }

    for (f = 0; f < engine->GetGlobalFunctionCount(); f++)
    {
        func = engine->GetGlobalFunctionByIndex(f);
        if (func)
        {
            funcName = func->GetName();

            if (!addedFunctions[funcName])
            {
                if (ctx.partialMember.empty() || funcName.rfind(ctx.partialMember, 0) == 0)
                {
                    itemsArray.push_back({{"label", funcName},
                                          {"kind", 3},
                                          {"detail", EnhanceIfFuncdef(CleanSignature(func->GetDeclaration(true, false, true)))},
                                          {"insertText", funcName + "($1)"},
                                          {"insertTextFormat", 2}});
                }
            }
        }
    }

    for (g = 0; g < engine->GetGlobalPropertyCount(); g++)
    {
        varName = nullptr;
        engine->GetGlobalPropertyByIndex(g, &varName, nullptr, nullptr, nullptr);

        if (varName && (ctx.partialMember.empty() || std::string(varName).rfind(ctx.partialMember, 0) == 0))
        {
            itemsArray.push_back({{"label", varName}, {"kind", 6}, {"detail", "Native global"}});
        }
    }
}

bool CompletionHandler::IsBaseClass(const std::string &child, const std::string &potentialBase)
{
    size_t cIdx;
    size_t bIdx;

    if (child == potentialBase)
    {
        return true;
    }

    for (cIdx = 0; cIdx < customClasses.size(); ++cIdx)
    {
        if (customClasses[cIdx].name == child)
        {
            for (bIdx = 0; bIdx < customClasses[cIdx].baseTypes.size(); ++bIdx)
            {
                if (IsBaseClass(customClasses[cIdx].baseTypes[bIdx], potentialBase))
                {
                    return true;
                }
            }
            break;
        }
    }
    return false;
}

void CompletionHandler::ExtractClassMembers(const std::string &targetClass, std::unordered_set<std::string> &addedMembers, bool canAccessPrivate, bool canAccessProtected)
{
    size_t cIdx;
    size_t pIdx;
    size_t mIdx;
    size_t bIdx;
    std::string subbedDetail;

    for (cIdx = 0; cIdx < customClasses.size(); ++cIdx)
    {
        if (customClasses[cIdx].name == targetClass)
        {
            for (pIdx = 0; pIdx < customClasses[cIdx].properties.size(); ++pIdx)
            {
                const auto &prop = customClasses[cIdx].properties[pIdx];
                if (ctx.lastSeparator == "::")
                {
                    continue;
                }
                if (!canAccessPrivate && (prop.access == "private" || prop.access == "protected"))
                {
                    continue;
                }
                if (addedMembers.find(prop.name) != addedMembers.end())
                {
                    continue;
                }
                if (ctx.partialMember.empty() || prop.name.rfind(ctx.partialMember, 0) == 0)
                {
                    subbedDetail = prop.access + " " + prop.typeName;
                    subbedDetail = EnhanceIfFuncdef(subbedDetail);
                    itemsArray.push_back({{"label", prop.name}, {"kind", 5}, {"detail", subbedDetail}});
                    addedMembers.insert(prop.name);
                }
            }
            for (mIdx = 0; mIdx < customClasses[cIdx].methods.size(); ++mIdx)
            {
                const auto &method = customClasses[cIdx].methods[mIdx];
                if (method.isConstructor)
                {
                    continue;
                }
                if (ctx.lastSeparator == "::" && !canAccessProtected)
                {
                    continue;
                }
                if (method.access == "private" && !canAccessPrivate)
                {
                    continue;
                }
                if (method.access == "protected" && !canAccessProtected)
                {
                    continue;
                }
                if (ctx.lastSeparator != "::")
                {
                    if (method.name.find("get_") == 0 || method.name.find("set_") == 0)
                    {
                        continue;
                    }
                }
                if (addedMembers.find(method.name) != addedMembers.end())
                {
                    continue;
                }
                if (ctx.partialMember.empty() || method.name.rfind(ctx.partialMember, 0) == 0)
                {
                    subbedDetail = method.access + " " + method.declaration;
                    subbedDetail = EnhanceIfFuncdef(subbedDetail);
                    itemsArray.push_back({{"label", method.name},
                                          {"kind", 2},
                                          {"detail", subbedDetail},
                                          {"insertText", method.name + "($1)"},
                                          {"insertTextFormat", 2}});
                    addedMembers.insert(method.name);
                }
            }
            for (bIdx = 0; bIdx < customClasses[cIdx].baseTypes.size(); ++bIdx)
            {
                ExtractClassMembers(customClasses[cIdx].baseTypes[bIdx], addedMembers, canAccessPrivate, canAccessProtected);
            }
            break;
        }
    }
}

void CompletionHandler::AddImplicitMembersRecursive(const std::string &targetClass, std::unordered_set<std::string> &addedImplicitMembers)
{
    size_t cIdx;
    size_t pIdx;
    size_t mIdx;
    size_t bIdx;

    for (cIdx = 0; cIdx < customClasses.size(); ++cIdx)
    {
        if (customClasses[cIdx].name == targetClass)
        {
            for (pIdx = 0; pIdx < customClasses[cIdx].properties.size(); ++pIdx)
            {
                const auto &prop = customClasses[cIdx].properties[pIdx];
                if (addedImplicitMembers.find(prop.name) != addedImplicitMembers.end())
                {
                    continue;
                }

                if (ctx.partialMember.empty() || prop.name.rfind(ctx.partialMember, 0) == 0)
                {
                    itemsArray.push_back({{"label", prop.name},
                                          {"kind", 5},
                                          {"detail", prop.access + " " + prop.typeName},
                                          {"documentation", {{"kind", "markdown"}, {"value", fmt::format("Member property of `{}`", targetClass)}}}});
                    addedImplicitMembers.insert(prop.name);
                }
            }

            for (mIdx = 0; mIdx < customClasses[cIdx].methods.size(); ++mIdx)
            {
                const auto &method = customClasses[cIdx].methods[mIdx];
                if (method.isConstructor)
                {
                    continue;
                }

                if (method.name.find("get_") == 0 || method.name.find("set_") == 0)
                {
                    continue;
                }

                if (addedImplicitMembers.find(method.name) != addedImplicitMembers.end())
                {
                    continue;
                }

                if (ctx.partialMember.empty() || method.name.rfind(ctx.partialMember, 0) == 0)
                {
                    itemsArray.push_back({{"label", method.name},
                                          {"kind", 2},
                                          {"detail", method.access + " " + method.declaration},
                                          {"documentation", {{"kind", "markdown"}, {"value", fmt::format("Member method of `{}`", targetClass)}}}});
                    addedImplicitMembers.insert(method.name);
                }
            }

            for (bIdx = 0; bIdx < customClasses[cIdx].baseTypes.size(); ++bIdx)
            {
                AddImplicitMembersRecursive(customClasses[cIdx].baseTypes[bIdx], addedImplicitMembers);
            }

            break;
        }
    }
}

std::string CompletionHandler::ResolveRootType(const std::string &rootName, bool rootIsMethod)
{
    bool isStaticAccess;
    std::string outType;
    asIScriptFunction *func;
    int typeId;
    const char *decl;
    const char *varName;
    size_t cIdx;
    asUINT f;
    asUINT g;

    isStaticAccess = false;
    func = nullptr;
    typeId = 0;
    decl = nullptr;
    varName = nullptr;

    if (rootName == "super" && !enclosingClass.empty())
    {
        for (cIdx = 0; cIdx < customClasses.size(); ++cIdx)
        {
            if (customClasses[cIdx].name == enclosingClass && !customClasses[cIdx].baseTypes.empty())
            {
                return customClasses[cIdx].baseTypes[0];
            }
        }
    }

    for (cIdx = 0; cIdx < customClasses.size(); ++cIdx)
    {
        if (customClasses[cIdx].name == rootName)
        {
            isStaticAccess = true;
            break;
        }
    }

    if (!isStaticAccess && GetNativeTypeInfo(rootName) != nullptr)
    {
        isStaticAccess = true;
    }

    if (rootIsMethod)
    {
        if (isStaticAccess)
        {
            return rootName;
        }

        if (!enclosingClass.empty())
        {
            if (SearchCustomClassRecursively(enclosingClass, rootName, true, outType, customClasses))
            {
                return outType;
            }
        }

        for (cIdx = 0; cIdx < tokenFuncs.size(); ++cIdx)
        {
            if (tokenFuncs[cIdx].name == rootName)
            {
                return tokenFuncs[cIdx].typeName;
            }
        }

        for (f = 0; f < engine->GetGlobalFunctionCount(); f++)
        {
            func = engine->GetGlobalFunctionByIndex(f);

            if (func && std::string(func->GetName()) == rootName)
            {
                typeId = func->GetReturnTypeId();
                decl = engine->GetTypeDeclaration(typeId, true);
                return decl ? decl : "";
            }
        }

        return "";
    }

    if (isStaticAccess)
    {
        return rootName;
    }

    if (rootName == "this" && !enclosingClass.empty())
    {
        return enclosingClass;
    }

    for (cIdx = 0; cIdx < localVars.size(); ++cIdx)
    {
        if (localVars[cIdx].name == rootName)
        {
            return localVars[cIdx].typeName;
        }
    }

    for (cIdx = 0; cIdx < tokenGlobalVars.size(); ++cIdx)
    {
        if (tokenGlobalVars[cIdx].name == rootName)
        {
            return tokenGlobalVars[cIdx].typeName;
        }
    }

    if (!enclosingClass.empty())
    {
        if (SearchCustomClassRecursively(enclosingClass, rootName, false, outType, customClasses))
        {
            return outType;
        }
    }

    for (g = 0; g < engine->GetGlobalPropertyCount(); g++)
    {
        varName = nullptr;
        typeId = 0;
        engine->GetGlobalPropertyByIndex(g, &varName, nullptr, &typeId, nullptr);

        if (varName && std::string(varName) == rootName)
        {
            decl = engine->GetTypeDeclaration(typeId, true);
            return decl ? decl : "";
        }
    }

    return "";
}

std::string CompletionHandler::WalkObjectChain(std::string inferredTypeName)
{
    std::string nextName;
    std::string nextType;
    std::string baseTypeName;
    std::string cleanNext;
    int nextDeref;
    bool nextIsMethod;
    bool foundInScript;
    asITypeInfo *t;
    const char *pName;
    int pTypeId;
    asIScriptFunction *func;
    int rTypeId;
    const char *decl;
    size_t i;
    asUINT p;
    asUINT m;
    int d;

    nextDeref = 0;
    nextIsMethod = false;
    foundInScript = false;
    t = nullptr;
    pName = nullptr;
    pTypeId = 0;
    func = nullptr;
    rTypeId = 0;
    decl = nullptr;

    if (logger)
    {
        logger(fmt::format("Starting WalkObjectChain with base type: '{}'", inferredTypeName));
    }

    for (i = 1; i < ctx.objectChain.size(); ++i)
    {
        if (inferredTypeName.empty())
        {
            break;
        }

        nextDeref = 0;
        nextIsMethod = false;
        ParseSegment(ctx.objectChain[i], nextName, nextDeref, nextIsMethod);

        if (logger)
        {
            logger(fmt::format("-> Evaluating segment: '{}' (Is Method: {}, Deref: {})", nextName, nextIsMethod, nextDeref));
        }

        nextType = "";
        baseTypeName = TokenHarvester::GetBaseType(inferredTypeName);
        foundInScript = SearchCustomClassRecursively(baseTypeName, nextName, nextIsMethod, nextType, customClasses);

        if (!foundInScript)
        {
            t = GetNativeTypeInfo(inferredTypeName);

            if (t)
            {
                if (!nextIsMethod)
                {
                    for (p = 0; p < t->GetPropertyCount(); p++)
                    {
                        pName = nullptr;
                        pTypeId = 0;
                        t->GetProperty(p, &pName, &pTypeId);

                        if (pName && std::string(pName) == nextName)
                        {
                            decl = engine->GetTypeDeclaration(pTypeId, true);

                            if (decl)
                            {
                                nextType = decl;
                            }

                            break;
                        }
                    }
                }

                if (nextType.empty())
                {
                    for (m = 0; m < t->GetMethodCount(); m++)
                    {
                        func = t->GetMethodByIndex(m);

                        if (func && std::string(func->GetName()) == nextName)
                        {
                            rTypeId = func->GetReturnTypeId();
                            decl = engine->GetTypeDeclaration(rTypeId, true);

                            if (decl)
                            {
                                nextType = decl;
                            }

                            break;
                        }
                    }
                }
            }
            else
            {
                if (logger)
                {
                    logger(fmt::format("ERROR: asITypeInfo not found for '{}'", inferredTypeName));
                }
            }
        }

        if (!nextType.empty())
        {
            if (logger)
            {
                logger(fmt::format("AngelScript returned type: '{}'", nextType));
            }

            cleanNext = TokenHarvester::GetBaseType(nextType);

            if (cleanNext.length() <= 2)
            {
                if (logger)
                {
                    logger(fmt::format("Generic detected ('{}'). Forcing template inner type extraction.", cleanNext));
                }

                nextType = inferredTypeName;
                nextDeref++;
            }

            for (d = 0; d < nextDeref; d++)
            {
                nextType = TokenHarvester::ExtractInnerType(nextType);
            }

            inferredTypeName = TokenHarvester::GetInstantiatedType(nextType);

            if (logger)
            {
                logger(fmt::format("Result after extraction: '{}'", inferredTypeName));
            }
        }
        else
        {
            inferredTypeName = "";

            if (logger)
            {
                logger("Result: Empty (Broken chain).");
            }
        }
    }

    return inferredTypeName;
}

void CompletionHandler::PopulateMembers(const std::string &inferredTypeName)
{
    std::string baseTypeName;
    std::unordered_set<std::string> addedMembers;
    asITypeInfo *targetType;
    const char *enumName;
    bool classFoundInScript;
    bool canAccessPrivate;
    bool canAccessProtected;
    const char *propName;
    int propTypeId;
    const char *decl;
    asIScriptFunction *func;
    std::string mName;
    asUINT v;
    asUINT p;
    asUINT m;
    std::string subbedDetail;
    size_t cIdx;

    baseTypeName = TokenHarvester::GetBaseType(inferredTypeName);
    targetType = GetNativeTypeInfo(inferredTypeName);
    enumName = nullptr;
    classFoundInScript = false;
    canAccessPrivate = (enclosingClass == baseTypeName);
    canAccessProtected = IsBaseClass(enclosingClass, baseTypeName);
    propName = nullptr;
    propTypeId = 0;
    decl = nullptr;
    func = nullptr;

    if (targetType && (targetType->GetFlags() & asOBJ_ENUM))
    {
        if (ctx.lastSeparator == "::")
        {
            for (v = 0; v < targetType->GetEnumValueCount(); v++)
            {
                enumName = targetType->GetEnumValueByIndex(v, nullptr);
                if (enumName)
                {
                    if (addedMembers.find(enumName) != addedMembers.end())
                    {
                        continue;
                    }
                    if (ctx.partialMember.empty() || std::string(enumName).rfind(ctx.partialMember, 0) == 0)
                    {
                        itemsArray.push_back({{"label", enumName}, {"kind", 20}, {"detail", "enum value"}});
                        addedMembers.insert(enumName);
                    }
                }
            }
        }
        return;
    }

    ExtractClassMembers(baseTypeName, addedMembers, canAccessPrivate, canAccessProtected);

    for (cIdx = 0; cIdx < customClasses.size(); ++cIdx)
    {
        if (customClasses[cIdx].name == baseTypeName)
        {
            classFoundInScript = true;
            break;
        }
    }

    if (!classFoundInScript)
    {
        targetType = GetNativeTypeInfo(inferredTypeName);
        if (targetType)
        {
            for (p = 0; p < targetType->GetPropertyCount(); p++)
            {
                if (ctx.lastSeparator == "::")
                {
                    continue;
                }
                propName = nullptr;
                propTypeId = 0;
                targetType->GetProperty(p, &propName, &propTypeId);
                if (addedMembers.find(propName) != addedMembers.end())
                {
                    continue;
                }
                if (ctx.partialMember.empty() || std::string(propName).rfind(ctx.partialMember, 0) == 0)
                {
                    decl = engine->GetTypeDeclaration(propTypeId, true);
                    subbedDetail = decl ? decl : "primitive";
                    SubstituteTemplateArguments(subbedDetail, inferredTypeName);
                    subbedDetail = EnhanceIfFuncdef(subbedDetail);
                    itemsArray.push_back({{"label", propName}, {"kind", 5}, {"detail", subbedDetail}});
                    addedMembers.insert(propName);
                }
            }
            for (m = 0; m < targetType->GetMethodCount(); m++)
            {
                func = targetType->GetMethodByIndex(m);
                mName = func->GetName();
                if (mName == baseTypeName || (!mName.empty() && mName[0] == '~'))
                {
                    continue;
                }
                if (ctx.lastSeparator == "::" && !canAccessProtected)
                {
                    continue;
                }
                if (ctx.lastSeparator != "::")
                {
                    if (mName.find("get_") == 0 || mName.find("set_") == 0)
                    {
                        continue;
                    }
                }
                if (addedMembers.find(mName) != addedMembers.end())
                {
                    continue;
                }
                if (ctx.partialMember.empty() || mName.rfind(ctx.partialMember, 0) == 0)
                {
                    subbedDetail = func->GetDeclaration(true, false, true);
                    SubstituteTemplateArguments(subbedDetail, inferredTypeName);
                    subbedDetail = CleanSignature(subbedDetail);
                    subbedDetail = EnhanceIfFuncdef(subbedDetail);
                    itemsArray.push_back({{"label", mName},
                                          {"kind", 2},
                                          {"detail", subbedDetail},
                                          {"insertText", mName + "($1)"},
                                          {"insertTextFormat", 2}});
                    addedMembers.insert(mName);
                }
            }
        }
    }
}

asITypeInfo *CompletionHandler::GetNativeTypeInfo(const std::string &typeName)
{
    asITypeInfo *t;
    std::string base;
    asUINT idx;

    t = nullptr;

    if (typeName.empty())
    {
        return nullptr;
    }

    if (module)
    {
        t = module->GetTypeInfoByDecl(typeName.c_str());

        if (t)
        {
            return t;
        }
    }

    t = engine->GetTypeInfoByDecl(typeName.c_str());

    if (t)
    {
        return t;
    }

    if (module)
    {
        t = module->GetTypeInfoByName(typeName.c_str());

        if (t)
        {
            return t;
        }
    }

    t = engine->GetTypeInfoByName(typeName.c_str());

    if (t)
    {
        return t;
    }

    base = TokenHarvester::GetBaseType(typeName);

    if (module)
    {
        t = module->GetTypeInfoByName(base.c_str());

        if (t)
        {
            return t;
        }
    }

    t = engine->GetTypeInfoByName(base.c_str());

    if (t)
    {
        return t;
    }

    for (idx = 0; idx < engine->GetObjectTypeCount(); idx++)
    {
        t = engine->GetObjectTypeByIndex(idx);

        if (t && std::string(t->GetName()) == base)
        {
            return t;
        }
    }

    return nullptr;
}

void CompletionHandler::ParseSegment(const std::string &segment, std::string &outName, int &outDerefCount, bool &outIsMethod)
{
    size_t cutoff;
    size_t charIdx;

    cutoff = 0;
    outName = segment;
    outDerefCount = 0;
    outIsMethod = false;

    for (charIdx = 0; charIdx < segment.size(); ++charIdx)
    {
        if (segment[charIdx] == '[')
        {
            outDerefCount++;
        }
    }

    cutoff = outName.find_first_of("([");

    if (cutoff != std::string::npos)
    {
        if (outName[cutoff] == '(')
        {
            outIsMethod = true;
        }

        outName = outName.substr(0, cutoff);
    }
}

bool CompletionHandler::IsStructureDeclarationKeyword(std::string_view text) const noexcept
{
    return text == "class" || text == "interface" || text == "namespace" || text == "enum" || text == "mixin" || text == "abstract";
}

bool CompletionHandler::IsStatementKeyword(std::string_view text) const noexcept
{
    return text == "if" || text == "else" || text == "for" || text == "foreach" || text == "while" || text == "return" ||
           text == "break" || text == "continue" || text == "switch" || text == "case" || text == "default" ||
           text == "cast" || text == "try" || text == "catch" || text == "delete" || text == "throw";
}

bool CompletionHandler::IsStorageModifierKeyword(std::string_view text) const noexcept
{
    return text == "private" || text == "protected" || text == "public" || text == "shared" || text == "external";
}

bool CompletionHandler::IsPrimitiveType(std::string_view text) const noexcept
{
    return text == "void" || text == "int" || text == "int8" || text == "int16" || text == "int32" || text == "int64" ||
           text == "uint" || text == "uint8" || text == "uint16" || text == "uint32" || text == "uint64" ||
           text == "float" || text == "double" || text == "bool" || text == "auto";
}

void CompletionHandler::NormalizeSignatureSpacing(std::string &signature) const
{
    size_t p;
    p = 0;

    while ((p = signature.find(" ::")) != std::string::npos)
        signature.erase(p, 1);
    while ((p = signature.find(":: ")) != std::string::npos)
        signature.erase(p + 2, 1);

    while ((p = signature.find("& in")) != std::string::npos)
        signature.replace(p, 4, "&in");
    while ((p = signature.find("& out")) != std::string::npos)
        signature.replace(p, 5, "&out");
    while ((p = signature.find("& inout")) != std::string::npos)
        signature.replace(p, 7, "&inout");
}

std::string CompletionHandler::CleanSignature(std::string str)
{
    std::string res;
    std::string finalRes;
    size_t idx;
    char c;

    res = "";
    finalRes = "";

    for (idx = 0; idx < str.size(); ++idx)
    {
        c = str[idx];
        if (c == ' ')
        {
            if (!res.empty() && (res.back() == '<' || res.back() == '>' || res.back() == '@' || res.back() == '&' || res.back() == '('))
                continue;
            if (idx + 1 < str.size() && (str[idx + 1] == '<' || str[idx + 1] == '>' || str[idx + 1] == '@' || str[idx + 1] == '&' || str[idx + 1] == '(' || str[idx + 1] == ',' || str[idx + 1] == ')'))
                continue;
        }
        res += c;
    }

    for (idx = 0; idx < res.size(); ++idx)
    {
        finalRes += res[idx];
        if (res[idx] == '@' || res[idx] == '&' || res[idx] == ',')
        {
            if (idx + 1 < res.size() && res[idx + 1] != ' ' && res[idx + 1] != ',' && res[idx + 1] != ')' && res[idx + 1] != '>')
                finalRes += ' ';
        }
        if (res[idx] == '>')
        {
            if (idx + 1 < res.size() && res[idx + 1] != ' ' && res[idx + 1] != ',' && res[idx + 1] != ')' && res[idx + 1] != '>' && res[idx + 1] != '@' && res[idx + 1] != '&' && res[idx + 1] != ':')
                finalRes += ' ';
        }
    }

    NormalizeSignatureSpacing(finalRes);
    return finalRes;
}

std::string_view CompletionHandler::StripAccessModifiers(std::string_view typeStr) noexcept
{
    if (typeStr.starts_with("protected "))
        return typeStr.substr(10);
    if (typeStr.starts_with("private "))
        return typeStr.substr(8);
    if (typeStr.starts_with("external "))
        return typeStr.substr(9);
    if (typeStr.starts_with("public "))
        return typeStr.substr(7);
    if (typeStr.starts_with("shared "))
        return typeStr.substr(7);
    return typeStr;
}

std::string CompletionHandler::ExtractBaseTypeName(std::string_view typeStr)
{
    std::string baseName;
    size_t i;
    char c;

    baseName = "";
    for (i = 0; i < typeStr.length(); ++i)
    {
        c = typeStr[i];
        if (c == '@' || c == '&' || c == ' ' || c == '<')
        {
            break;
        }
        baseName += c;
    }
    return baseName;
}

std::string CompletionHandler::DeduceTypeFromRHS(const std::string &objectName)
{
    std::string name;
    std::string resolvedType;
    int derefCount;
    bool isMethod;
    size_t vIdx;

    derefCount = 0;
    isMethod = false;

    ParseSegment(objectName, name, derefCount, isMethod);
    resolvedType = ResolveRootType(name, isMethod);

    for (vIdx = 0; vIdx < static_cast<size_t>(derefCount); ++vIdx)
    {
        resolvedType = TokenHarvester::ExtractInnerType(resolvedType);
    }
    resolvedType = TokenHarvester::GetInstantiatedType(resolvedType);

    return resolvedType;
}

void CompletionHandler::SubstituteTemplateArguments(std::string &targetStr, std::string_view templateType)
{
    size_t startAngle;
    size_t endAngle;
    std::string templateArg;
    size_t tPos;
    bool leftOk;
    bool rightOk;

    startAngle = templateType.find('<');
    endAngle = templateType.rfind('>');

    if (startAngle != std::string::npos && endAngle != std::string::npos && endAngle > startAngle)
    {
        templateArg = templateType.substr(startAngle + 1, endAngle - startAngle - 1);
        tPos = 0;
        while ((tPos = targetStr.find('T', tPos)) != std::string::npos)
        {
            leftOk = (tPos == 0 || (!std::isalnum(static_cast<unsigned char>(targetStr[tPos - 1])) && targetStr[tPos - 1] != '_'));
            rightOk = (tPos + 1 == targetStr.length() || (!std::isalnum(static_cast<unsigned char>(targetStr[tPos + 1])) && targetStr[tPos + 1] != '_'));

            if (leftOk && rightOk)
            {
                targetStr.replace(tPos, 1, templateArg);
                tPos += templateArg.length();
            }
            else
            {
                tPos++;
            }
        }
    }
}

std::string CompletionHandler::EnhanceIfFuncdef(const std::string &hoverText)
{
    std::string baseHover;
    std::string_view cleanedView;
    size_t firstSpace;
    std::string typeName;
    std::string paramsSuffix;
    asIScriptModule *mod;
    asITypeInfo *fdefInfo;
    asIScriptFunction *fdefFunc;
    std::string fullFuncDecl;
    size_t parenPos;

    baseHover = hoverText;
    if (!engine)
    {
        return baseHover;
    }

    cleanedView = StripAccessModifiers(baseHover);
    firstSpace = cleanedView.find(' ');
    if (firstSpace != std::string_view::npos)
    {
        typeName = std::string(cleanedView.substr(0, firstSpace));
        if (!typeName.empty() && typeName.back() == '@')
        {
            typeName.pop_back();
        }

        paramsSuffix = "";
        mod = engine->GetModule("LSPModule");
        fdefInfo = mod ? mod->GetTypeInfoByName(typeName.c_str()) : nullptr;
        if (!fdefInfo)
        {
            fdefInfo = engine->GetTypeInfoByName(typeName.c_str());
        }

        if (fdefInfo && (fdefInfo->GetFlags() & asOBJ_FUNCDEF))
        {
            fdefFunc = fdefInfo->GetFuncdefSignature();
            if (fdefFunc)
            {
                fullFuncDecl = fdefFunc->GetDeclaration(true, true);
                parenPos = fullFuncDecl.find('(');
                if (parenPos != std::string::npos)
                {
                    paramsSuffix = fullFuncDecl.substr(parenPos);
                }
            }
        }

        if (!paramsSuffix.empty())
        {
            baseHover += paramsSuffix;
        }
    }
    return baseHover;
}