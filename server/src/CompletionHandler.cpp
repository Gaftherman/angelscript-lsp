/**
 * @file CompletionHandler.cpp
 * @brief Implementation of semantic type resolution for autocompletion.
 */

#include "CompletionHandler.h"
#include <unordered_map>
#include <unordered_set>
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
    for (const auto &c : customClasses)
    {
        if (c.name == typeName)
        {
            if (!isMethod)
            {
                for (const auto &prop : c.properties)
                {
                    if (prop.name == memberName)
                    {
                        outType = prop.typeName;
                        return true;
                    }
                }
            }

            if (outType.empty())
            {
                for (const auto &method : c.methods)
                {
                    if (method.name == memberName)
                    {
                        outType = method.typeName;
                        return true;
                    }
                }
            }

            for (const auto &baseType : c.baseTypes)
            {
                if (SearchCustomClassRecursively(baseType, memberName, isMethod, outType, customClasses))
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
    for (const auto &item : itemsArray)
    {
        if (item.contains("label") && item["label"] == label)
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
    bool isTypeName = false;

    if (ctx.lastSeparator == ":")
    {
        if (!ctx.objectChain.empty())
        {
            precedingToken = ctx.objectChain[0];

            for (const auto &c : customClasses)
            {
                if (c.name == precedingToken)
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
            HandleGlobalScopeResolution();
        }
        else if (!ctx.objectChain.empty())
        {
            HandleMemberAccess();
        }
    }
    else
    {
        HandleGlobalScope(originalText, cursorAbsPos);
    }

    return itemsArray;
}

void CompletionHandler::HandleMemberAccess()
{
    std::string rootName;
    std::string inferredTypeName;
    int rootDeref = 0;
    bool rootIsMethod = false;

    ParseSegment(ctx.objectChain[0], rootName, rootDeref, rootIsMethod);

    inferredTypeName = ResolveRootType(rootName, rootIsMethod);

    if (logger)
    {
        logger(fmt::format("ResolveRootType found original type: '{}'", inferredTypeName));
    }

    for (int d = 0; d < rootDeref; d++)
    {
        inferredTypeName = TokenHarvester::ExtractInnerType(inferredTypeName);
    }

    inferredTypeName = TokenHarvester::GetInstantiatedType(inferredTypeName);

    if (logger)
    {
        logger(fmt::format("Type ready for WalkObjectChain: '{}'", inferredTypeName));
    }

    inferredTypeName = WalkObjectChain(inferredTypeName);

    if (!inferredTypeName.empty())
    {
        PopulateMembers(inferredTypeName);
    }
}

void CompletionHandler::HandleGlobalScopeResolution()
{
    std::unordered_map<std::string, bool> addedFunctions;
    asIScriptFunction *func = nullptr;
    const char *varName = nullptr;
    int typeId = 0;
    std::string funcName;

    for (const auto &c : customClasses)
    {
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
        for (asUINT f = 0; f < module->GetFunctionCount(); f++)
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
                                          {"detail", func->GetDeclaration(true, false, true)},
                                          {"insertText", funcName + "($1)"},
                                          {"insertTextFormat", 2}});
                }
            }
        }

        for (asUINT g = 0; g < module->GetGlobalVarCount(); g++)
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

    for (const auto &tf : tokenFuncs)
    {
        if (!addedFunctions[tf.name])
        {
            addedFunctions[tf.name] = true;

            if (ctx.partialMember.empty() || tf.name.rfind(ctx.partialMember, 0) == 0)
            {
                itemsArray.push_back({{"label", tf.name},
                                      {"kind", 3},
                                      {"detail", tf.declaration},
                                      {"insertText", tf.name + "($1)"},
                                      {"insertTextFormat", 2}});
            }
        }
    }

    for (const auto &nativeVar : tokenGlobalVars)
    {
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

    for (asUINT f = 0; f < engine->GetGlobalFunctionCount(); f++)
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
                                          {"detail", func->GetDeclaration(true, false, true)},
                                          {"insertText", funcName + "($1)"},
                                          {"insertTextFormat", 2}});
                }
            }
        }
    }

    for (asUINT g = 0; g < engine->GetGlobalPropertyCount(); g++)
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

std::string CompletionHandler::ResolveRootType(const std::string &rootName, bool rootIsMethod)
{
    bool isStaticAccess = false;
    std::string outType;
    asIScriptFunction *func = nullptr;
    int typeId = 0;
    const char *decl = nullptr;
    const char *varName = nullptr;

    if (rootName == "super" && !enclosingClass.empty())
    {
        for (const auto &c : customClasses)
        {
            if (c.name == enclosingClass && !c.baseTypes.empty())
            {
                return c.baseTypes[0];
            }
        }
    }

    for (const auto &c : customClasses)
    {
        if (c.name == rootName)
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

        for (const auto &f : tokenFuncs)
        {
            if (f.name == rootName)
            {
                return f.typeName;
            }
        }

        for (asUINT f = 0; f < engine->GetGlobalFunctionCount(); f++)
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

    for (const auto &v : localVars)
    {
        if (v.name == rootName)
        {
            return v.typeName;
        }
    }

    for (const auto &v : tokenGlobalVars)
    {
        if (v.name == rootName)
        {
            return v.typeName;
        }
    }

    if (!enclosingClass.empty())
    {
        if (SearchCustomClassRecursively(enclosingClass, rootName, false, outType, customClasses))
        {
            return outType;
        }
    }

    for (asUINT g = 0; g < engine->GetGlobalPropertyCount(); g++)
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
    int nextDeref = 0;
    bool nextIsMethod = false;
    bool foundInScript = false;
    asITypeInfo *t = nullptr;
    const char *pName = nullptr;
    int pTypeId = 0;
    asIScriptFunction *func = nullptr;
    int rTypeId = 0;
    const char *decl = nullptr;

    if (logger)
    {
        logger(fmt::format("Starting WalkObjectChain with base type: '{}'", inferredTypeName));
    }

    for (size_t i = 1; i < ctx.objectChain.size(); ++i)
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
                    for (asUINT p = 0; p < t->GetPropertyCount(); p++)
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
                    for (asUINT m = 0; m < t->GetMethodCount(); m++)
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

            for (int d = 0; d < nextDeref; d++)
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
    std::string baseTypeName = TokenHarvester::GetBaseType(inferredTypeName);
    std::unordered_set<std::string> addedMembers;
    asITypeInfo *targetType = GetNativeTypeInfo(inferredTypeName);
    const char *enumName = nullptr;

    if (targetType && (targetType->GetFlags() & asOBJ_ENUM))
    {
        if (ctx.lastSeparator == "::")
        {
            for (asUINT v = 0; v < targetType->GetEnumValueCount(); v++)
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

    bool classFoundInScript = false;
    bool canAccessPrivate = (enclosingClass == baseTypeName);
    bool canAccessProtected = false;
    const char *propName = nullptr;
    int propTypeId = 0;
    const char *decl = nullptr;
    asIScriptFunction *func = nullptr;
    std::string mName;

    std::function<bool(const std::string &, const std::string &)> IsBaseClass = [&](const std::string &child, const std::string &potentialBase)
    {
        if (child == potentialBase)
        {
            return true;
        }
        for (const auto &c : customClasses)
        {
            if (c.name == child)
            {
                for (const auto &b : c.baseTypes)
                {
                    if (IsBaseClass(b, potentialBase))
                    {
                        return true;
                    }
                }
                break;
            }
        }
        return false;
    };

    canAccessProtected = IsBaseClass(enclosingClass, baseTypeName);

    std::function<void(const std::string &)> ExtractClassMembers = [&](const std::string &targetClass)
    {
        for (const auto &c : customClasses)
        {
            if (c.name == targetClass)
            {
                classFoundInScript = true;
                for (const auto &prop : c.properties)
                {
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
                        itemsArray.push_back({{"label", prop.name}, {"kind", 5}, {"detail", prop.access + " " + prop.typeName}});
                        addedMembers.insert(prop.name);
                    }
                }
                for (const auto &method : c.methods)
                {
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
                        itemsArray.push_back({{"label", method.name},
                                              {"kind", 2},
                                              {"detail", method.access + " " + method.declaration},
                                              {"insertText", method.name + "($1)"},
                                              {"insertTextFormat", 2}});
                        addedMembers.insert(method.name);
                    }
                }
                for (const auto &baseType : c.baseTypes)
                {
                    ExtractClassMembers(baseType);
                }
                break;
            }
        }
    };

    ExtractClassMembers(baseTypeName);

    if (!classFoundInScript)
    {
        targetType = GetNativeTypeInfo(inferredTypeName);
        if (targetType)
        {
            for (asUINT p = 0; p < targetType->GetPropertyCount(); p++)
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
                    itemsArray.push_back({{"label", propName}, {"kind", 5}, {"detail", decl ? decl : "primitive"}});
                    addedMembers.insert(propName);
                }
            }
            for (asUINT m = 0; m < targetType->GetMethodCount(); m++)
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
                    itemsArray.push_back({{"label", mName},
                                          {"kind", 2},
                                          {"detail", func->GetDeclaration(true, false, true)},
                                          {"insertText", mName + "($1)"},
                                          {"insertTextFormat", 2}});
                    addedMembers.insert(mName);
                }
            }
        }
    }
}

void CompletionHandler::HandleGlobalScope(const std::string &originalText, size_t cursorAbsPos)
{
    std::unordered_set<std::string> addedImplicitMembers;
    std::unordered_map<std::string, bool> addedFunctions;
    std::unordered_set<std::string> addedTypes;
    asIScriptFunction *func = nullptr;
    std::string funcName;
    const char *varName = nullptr;
    int typeId = 0;

    if (cursorAbsPos > 0 && originalText[cursorAbsPos - 1] == '@')
    {
        return;
    }

    // Pure language structural keywords
    std::vector<std::string> keywords = {
        "if", "else", "switch", "case", "default", "break", "continue",
        "while", "for", "foreach", "return", "class", "interface", "mixin",
        "enum", "shared", "external", "private", "protected", "import",
        "from", "cast", "is", "super", "this", "get", "set", "property",
        "const", "override", "final", "null", "true", "false", "try",
        "catch", "auto", "typedef", "funcdef"};

    for (const auto &kw : keywords)
    {
        if (ctx.partialMember.empty() || kw.rfind(ctx.partialMember, 0) == 0)
        {
            itemsArray.push_back({{"label", kw}, {"kind", 14}, {"detail", "keyword"}});
        }
    }

    // Fundamental primitive data types
    std::vector<std::string> primitives = {
        "void", "int", "int8", "int16", "int32", "int64",
        "uint", "uint8", "uint16", "uint32", "uint64",
        "float", "double", "bool"};

    for (const auto &prim : primitives)
    {
        if (ctx.partialMember.empty() || prim.rfind(ctx.partialMember, 0) == 0)
        {
            itemsArray.push_back({{"label", prim}, {"kind", 6}, {"detail", "primitive type"}});
            addedTypes.insert(prim);
        }
    }

    // Introspect native engine-registered types dynamically
    if (engine)
    {
        for (asUINT i = 0; i < engine->GetObjectTypeCount(); i++)
        {
            if (asITypeInfo *ti = engine->GetObjectTypeByIndex(i))
            {
                std::string name = ti->GetName();
                if ((ctx.partialMember.empty() || name.rfind(ctx.partialMember, 0) == 0) && addedTypes.insert(name).second)
                {
                    itemsArray.push_back({{"label", name}, {"kind", 7}, {"detail", "native class"}});
                }
            }
        }

        for (asUINT i = 0; i < engine->GetEnumCount(); i++)
        {
            if (asITypeInfo *ti = engine->GetEnumByIndex(i))
            {
                std::string name = ti->GetName();
                if ((ctx.partialMember.empty() || name.rfind(ctx.partialMember, 0) == 0) && addedTypes.insert(name).second)
                {
                    itemsArray.push_back({{"label", name}, {"kind", 13}, {"detail", "native enum"}});
                }
            }
        }

        for (asUINT i = 0; i < engine->GetTypedefCount(); i++)
        {
            if (asITypeInfo *ti = engine->GetTypedefByIndex(i))
            {
                std::string name = ti->GetName();
                if ((ctx.partialMember.empty() || name.rfind(ctx.partialMember, 0) == 0) && addedTypes.insert(name).second)
                {
                    itemsArray.push_back({{"label", name}, {"kind", 6}, {"detail", "native typedef"}});
                }
            }
        }

        for (asUINT i = 0; i < engine->GetFuncdefCount(); i++)
        {
            if (asITypeInfo *ti = engine->GetFuncdefByIndex(i))
            {
                std::string name = ti->GetName();
                if ((ctx.partialMember.empty() || name.rfind(ctx.partialMember, 0) == 0) && addedTypes.insert(name).second)
                {
                    itemsArray.push_back({{"label", name}, {"kind", 11}, {"detail", "native funcdef"}});
                }
            }
        }
    }

    for (const auto &v : localVars)
    {
        if (ctx.partialMember.empty() || v.name.rfind(ctx.partialMember, 0) == 0)
        {
            itemsArray.push_back({{"label", v.name}, {"kind", 6}, {"detail", "local " + v.typeName}});
        }
    }

    if (!enclosingClass.empty())
    {
        std::function<void(const std::string &)> AddImplicitMembers = [&](const std::string &targetClass)
        {
            for (const auto &c : customClasses)
            {
                if (c.name == targetClass)
                {
                    for (const auto &prop : c.properties)
                    {
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

                    for (const auto &method : c.methods)
                    {
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

                    for (const auto &baseType : c.baseTypes)
                    {
                        AddImplicitMembers(baseType);
                    }

                    break;
                }
            }
        };

        AddImplicitMembers(enclosingClass);
    }

    for (const auto &c : customClasses)
    {
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
        for (asUINT f = 0; f < module->GetFunctionCount(); f++)
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
                                          {"detail", func->GetDeclaration(true, false, true)},
                                          {"insertText", funcName + "($1)"},
                                          {"insertTextFormat", 2}});
                }
            }
        }

        for (asUINT g = 0; g < module->GetGlobalVarCount(); g++)
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

    for (const auto &tf : tokenFuncs)
    {
        if (!addedFunctions[tf.name])
        {
            addedFunctions[tf.name] = true;

            if (ctx.partialMember.empty() || tf.name.rfind(ctx.partialMember, 0) == 0)
            {
                itemsArray.push_back({{"label", tf.name},
                                      {"kind", 3},
                                      {"detail", tf.declaration},
                                      {"insertText", tf.name + "($1)"},
                                      {"insertTextFormat", 2}});
            }
        }
    }

    for (const auto &nativeVar : tokenGlobalVars)
    {
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

    for (asUINT f = 0; f < engine->GetGlobalFunctionCount(); f++)
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
                                          {"detail", func->GetDeclaration(true, false, true)},
                                          {"insertText", funcName + "($1)"},
                                          {"insertTextFormat", 2}});
                }
            }
        }
    }

    for (asUINT g = 0; g < engine->GetGlobalPropertyCount(); g++)
    {
        varName = nullptr;
        engine->GetGlobalPropertyByIndex(g, &varName, nullptr, nullptr, nullptr);

        if (varName && (ctx.partialMember.empty() || std::string(varName).rfind(ctx.partialMember, 0) == 0))
        {
            itemsArray.push_back({{"label", varName}, {"kind", 6}, {"detail", "Native global"}});
        }
    }
}

asITypeInfo *CompletionHandler::GetNativeTypeInfo(const std::string &typeName)
{
    asITypeInfo *t = nullptr;
    std::string base;

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

    for (asUINT idx = 0; idx < engine->GetObjectTypeCount(); idx++)
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
    size_t cutoff = 0;

    outName = segment;
    outDerefCount = 0;
    outIsMethod = false;

    for (char c : segment)
    {
        if (c == '[')
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