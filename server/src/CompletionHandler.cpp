/**
 * @file CompletionHandler.cpp
 * @brief Highly-optimized implementation of semantic type resolution for autocompletion.
 */

#include "CompletionHandler.h"
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <ranges>
#include <cctype>
#include <fmt/core.h>

using json = nlohmann::json;

/**
 * @brief Recursively searches for a member within user-defined script classes using O(1) maps.
 * @param initialTypeName Name of the class scope being evaluated.
 * @param memberName Identifier of the member property or method being sought.
 * @param isMethod Assert true to filter tracking specifically for method signatures.
 * @param outType String populated with the discovered identifier type name.
 * @param customClasses Extracted translation unit context containing user class metadata indices.
 * @return True if the member resolution successfully completed within the lineage tree.
 */
static bool SearchCustomClassRecursively(const std::string &initialTypeName,
                                         const std::string &memberName,
                                         bool isMethod,
                                         std::string &outType,
                                         const std::vector<TokenHarvester::ScriptClass> &customClasses)
{
    std::vector<std::string> stack;
    std::unordered_set<std::string> visitedClasses;
    stack.push_back(initialTypeName);

    while (!stack.empty())
    {
        std::string currentClass = std::move(stack.back());
        stack.pop_back();

        if (!visitedClasses.insert(currentClass).second)
        {
            continue;
        }

        for (const auto &c : customClasses)
        {
            if (c.name != currentClass)
                continue;

            if (!isMethod)
            {
                if (auto propIt = c.properties.find(memberName); propIt != c.properties.end())
                {
                    outType = propIt->second.typeName;
                    return true;
                }
            }

            if (outType.empty())
            {
                if (auto methodIt = c.methods.find(memberName); methodIt != c.methods.end() && !methodIt->second.empty())
                {
                    outType = methodIt->second.front().typeName;
                    return true;
                }
            }

            for (const auto &base : c.baseTypes)
            {
                stack.push_back(base);
            }
            break;
        }
    }
    return false;
}

/**
 * @brief Evaluates whether a completion entry with a specific label signature already exists.
 * @param itemsArray Target internal JSON array structure containing populated metadata suggestions.
 * @param label Concrete literal identifier used for the identity mapping evaluation match.
 * @return True if a collision is explicitly identified within the array scope items.
 */
static inline bool ContainsItemLabel(const nlohmann::json &itemsArray, std::string_view label) noexcept
{
    for (const auto &item : itemsArray)
    {
        if (item.contains("label"))
        {
            const auto &labelNode = item["label"];
            if (labelNode.is_string())
            {
                if (labelNode.get_ref<const std::string &>().compare(label) == 0)
                {
                    return true;
                }
            }
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
    if (ctx.lastSeparator == ":" && !ctx.objectChain.empty())
    {
        std::string_view precedingToken = ctx.objectChain[0];
        bool isTypeName = std::ranges::any_of(customClasses, [precedingToken](const auto &c)
                                              { return c.name == precedingToken; }) ||
                          GetNativeTypeInfo(std::string(precedingToken)) != nullptr;
        if (isTypeName)
            return itemsArray;
    }

    if (!ctx.isMemberAccess)
    {
        ComputeLocalScopeCompletions(cursorAbsPos);
        ComputeGlobalScopeCompletions(originalText, cursorAbsPos);
        return itemsArray;
    }

    if (ctx.objectChain.empty())
    {
        if (ctx.lastSeparator == "::")
            ComputeNamespaceScopeCompletions("");
        return itemsArray;
    }

    if (ctx.lastSeparator != "::")
    {
        ComputeMemberAccessCompletions(ctx.objectChain[0]);
        return itemsArray;
    }

    std::string scopeName = ctx.objectChain[0];
    bool isTypeName = std::ranges::any_of(customClasses, [&scopeName](const auto &c)
                                          { return c.name == scopeName; }) ||
                      GetNativeTypeInfo(scopeName) != nullptr;

    if (isTypeName)
    {
        PopulateMembers(scopeName);
    }
    else
    {
        ComputeNamespaceScopeCompletions(ctx.objectChain[0]);
    }

    return itemsArray;
}

void CompletionHandler::ExtractClassMembers(const std::string &targetClass, std::unordered_set<std::string> &addedMembers, bool canAccessPrivate, bool canAccessProtected)
{
    std::vector<std::string> stack;
    std::unordered_set<std::string> visitedClasses;
    stack.push_back(targetClass);

    while (!stack.empty())
    {
        std::string currentClass = std::move(stack.back());
        stack.pop_back();

        if (!visitedClasses.insert(currentClass).second)
        {
            continue;
        }

        for (const auto &c : customClasses)
        {
            if (c.name != currentClass)
                continue;

            for (const auto &[propName, prop] : c.properties)
            {
                if (ctx.lastSeparator == "::" && !c.methods.empty())
                    continue;
                if (!canAccessPrivate && (prop.access == "private" || prop.access == "protected"))
                    continue;
                if (!addedMembers.insert(prop.name).second)
                    continue;

                if (ctx.partialMember.empty() || prop.name.starts_with(ctx.partialMember))
                {
                    std::string subbedDetail = prop.access + " " + prop.typeName;
                    subbedDetail = EnhanceIfFuncdef(subbedDetail);
                    itemsArray.push_back({{"label", prop.name}, {"kind", 5}, {"detail", subbedDetail}});
                }
            }

            for (const auto &[methodName, overloads] : c.methods)
            {
                for (const auto &method : overloads)
                {
                    if (method.isConstructor)
                        continue;
                    if (ctx.lastSeparator == "::" && !canAccessProtected)
                        continue;
                    if (method.access == "private" && !canAccessPrivate)
                        continue;
                    if (method.access == "protected" && !canAccessProtected)
                        continue;
                    if (ctx.lastSeparator != "::" && (method.name.starts_with("get_") || method.name.starts_with("set_")))
                        continue;
                    if (!addedMembers.insert(method.name).second)
                        continue;

                    if (ctx.partialMember.empty() || method.name.starts_with(ctx.partialMember))
                    {
                        std::string subbedDetail = method.access + " " + method.declaration;
                        subbedDetail = EnhanceIfFuncdef(subbedDetail);
                        itemsArray.push_back({{"label", method.name},
                                              {"kind", 2},
                                              {"detail", subbedDetail},
                                              {"insertText", method.name + "($1)"},
                                              {"insertTextFormat", 2}});
                    }
                }
            }

            for (const auto &base : c.baseTypes)
            {
                stack.push_back(base);
            }
            break;
        }
    }
}

void CompletionHandler::ComputeMemberAccessCompletions(std::string_view objectName)
{
    std::string inferredTypeName = DeduceTypeFromRHS(std::string(objectName));
    if (logger)
        logger(fmt::format("Resolved Type from RHS: '{}'", inferredTypeName));

    inferredTypeName = WalkObjectChain(inferredTypeName);
    if (!inferredTypeName.empty())
    {
        PopulateMembers(inferredTypeName);
    }
}

void CompletionHandler::ComputeNamespaceScopeCompletions(std::string_view namespacePrefix)
{
    std::unordered_map<std::string, bool> addedFunctions;

    for (const auto &c : customClasses)
    {
        if (ctx.partialMember.empty() || c.name.starts_with(ctx.partialMember))
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
            if (asIScriptFunction *func = module->GetFunctionByIndex(f))
            {
                std::string funcName = func->GetName();
                if (IsInternalCompilerFunction(funcName))
                    continue;

                addedFunctions[funcName] = true;
                if (ctx.partialMember.empty() || funcName.starts_with(ctx.partialMember))
                {
                    itemsArray.push_back({{"label", funcName},
                                          {"kind", 3},
                                          {"detail", EnhanceIfFuncdef(CleanSignature(func->GetDeclaration(true, false, true)))},
                                          {"insertText", funcName + "($1)"},
                                          {"insertTextFormat", 2}});
                }
            }
        }

        for (asUINT g = 0; g < module->GetGlobalVarCount(); g++)
        {
            const char *varName = nullptr;
            int typeId = 0;
            module->GetGlobalVar(g, &varName, nullptr, &typeId);
            if (varName && (ctx.partialMember.empty() || std::string_view(varName).starts_with(ctx.partialMember)))
            {
                itemsArray.push_back({{"label", varName}, {"kind", 6}, {"detail", "Global variable"}});
            }
        }
    }

    for (const auto &tf : tokenFuncs)
    {
        if (IsInternalCompilerFunction(tf.name) || addedFunctions[tf.name])
            continue;
        addedFunctions[tf.name] = true;

        if (ctx.partialMember.empty() || tf.name.starts_with(ctx.partialMember))
        {
            itemsArray.push_back({{"label", tf.name},
                                  {"kind", 3},
                                  {"detail", EnhanceIfFuncdef(CleanSignature(tf.declaration))},
                                  {"insertText", tf.name + "($1)"},
                                  {"insertTextFormat", 2}});
        }
    }

    for (const auto &nativeVar : tokenGlobalVars)
    {
        if (ctx.partialMember.empty() || nativeVar.name.starts_with(ctx.partialMember))
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
        if (asIScriptFunction *func = engine->GetGlobalFunctionByIndex(f))
        {
            std::string funcName = func->GetName();
            if (IsInternalCompilerFunction(funcName) || addedFunctions[funcName])
                continue;

            if (ctx.partialMember.empty() || funcName.starts_with(ctx.partialMember))
            {
                itemsArray.push_back({{"label", funcName},
                                      {"kind", 3},
                                      {"detail", EnhanceIfFuncdef(CleanSignature(func->GetDeclaration(true, false, true)))},
                                      {"insertText", funcName + "($1)"},
                                      {"insertTextFormat", 2}});
            }
        }
    }

    for (asUINT g = 0; g < engine->GetGlobalPropertyCount(); g++)
    {
        const char *varName = nullptr;
        engine->GetGlobalPropertyByIndex(g, &varName, nullptr, nullptr, nullptr);
        if (varName && (ctx.partialMember.empty() || std::string_view(varName).starts_with(ctx.partialMember)))
        {
            itemsArray.push_back({{"label", varName}, {"kind", 6}, {"detail", "Native global"}});
        }
    }
}

void CompletionHandler::ComputeLocalScopeCompletions(size_t activeOffset)
{
    for (const auto &v : localVars)
    {
        if (ctx.partialMember.empty() || v.name.starts_with(ctx.partialMember))
        {
            itemsArray.push_back({{"label", v.name}, {"kind", 6}, {"detail", "local " + v.typeName}});
        }
    }
}

void CompletionHandler::ComputeGlobalScopeCompletions(const std::string &originalText, size_t cursorAbsPos)
{
    if (cursorAbsPos > 0 && originalText[cursorAbsPos - 1] == '@')
        return;

    static const std::array<std::string_view, 39> keywords = {
        "if", "else", "switch", "case", "default", "break", "continue",
        "while", "for", "foreach", "return", "class", "interface", "mixin",
        "enum", "shared", "external", "private", "protected", "import",
        "from", "cast", "is", "super", "this", "get", "set", "property",
        "const", "override", "final", "null", "true", "false", "try",
        "catch", "auto", "typedef", "funcdef"};

    for (std::string_view kw : keywords)
    {
        if (ctx.partialMember.empty() || kw.starts_with(ctx.partialMember))
        {
            itemsArray.push_back({{"label", std::string(kw)}, {"kind", 14}, {"detail", "keyword"}});
        }
    }

    static const std::array<std::string_view, 14> primitives = {
        "void", "int", "int8", "int16", "int32", "int64",
        "uint", "uint8", "uint16", "uint32", "uint64",
        "float", "double", "bool"};

    std::unordered_set<std::string_view> addedTypes;
    for (std::string_view prim : primitives)
    {
        if (ctx.partialMember.empty() || prim.starts_with(ctx.partialMember))
        {
            itemsArray.push_back({{"label", std::string(prim)}, {"kind", 6}, {"detail", "primitive type"}});
            addedTypes.insert(prim);
        }
    }

    if (engine)
    {
        for (asUINT i = 0; i < engine->GetObjectTypeCount(); i++)
        {
            if (asITypeInfo *ti = engine->GetObjectTypeByIndex(i))
            {
                std::string_view name = ti->GetName();
                if ((ctx.partialMember.empty() || name.starts_with(ctx.partialMember)) && addedTypes.insert(name).second)
                {
                    itemsArray.push_back({{"label", std::string(name)}, {"kind", 7}, {"detail", "native class"}});
                }
            }
        }
        for (asUINT i = 0; i < engine->GetEnumCount(); i++)
        {
            if (asITypeInfo *ti = engine->GetEnumByIndex(i))
            {
                std::string_view name = ti->GetName();
                if ((ctx.partialMember.empty() || name.starts_with(ctx.partialMember)) && addedTypes.insert(name).second)
                {
                    itemsArray.push_back({{"label", std::string(name)}, {"kind", 13}, {"detail", "native enum"}});
                }
            }
        }
        for (asUINT i = 0; i < engine->GetTypedefCount(); i++)
        {
            if (asITypeInfo *ti = engine->GetTypedefByIndex(i))
            {
                std::string_view name = ti->GetName();
                if ((ctx.partialMember.empty() || name.starts_with(ctx.partialMember)) && addedTypes.insert(name).second)
                {
                    itemsArray.push_back({{"label", std::string(name)}, {"kind", 6}, {"detail", "native typedef"}});
                }
            }
        }
        for (asUINT i = 0; i < engine->GetFuncdefCount(); i++)
        {
            if (asITypeInfo *ti = engine->GetFuncdefByIndex(i))
            {
                std::string_view name = ti->GetName();
                if ((ctx.partialMember.empty() || name.starts_with(ctx.partialMember)) && addedTypes.insert(name).second)
                {
                    itemsArray.push_back({{"label", std::string(name)}, {"kind", 11}, {"detail", "native funcdef"}});
                }
            }
        }
    }

    std::unordered_set<std::string> addedImplicitMembers;
    if (!enclosingClass.empty())
    {
        AddImplicitMembersRecursive(enclosingClass, addedImplicitMembers);
    }

    std::unordered_map<std::string, bool> addedFunctions;
    for (const auto &c : customClasses)
    {
        if (ctx.partialMember.empty() || c.name.starts_with(ctx.partialMember))
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
            if (asIScriptFunction *func = module->GetFunctionByIndex(f))
            {
                std::string funcName = func->GetName();
                if (IsInternalCompilerFunction(funcName))
                    continue;
                addedFunctions[funcName] = true;

                if (ctx.partialMember.empty() || funcName.starts_with(ctx.partialMember))
                {
                    itemsArray.push_back({{"label", funcName},
                                          {"kind", 3},
                                          {"detail", EnhanceIfFuncdef(CleanSignature(func->GetDeclaration(true, false, true)))},
                                          {"insertText", funcName + "($1)"},
                                          {"insertTextFormat", 2}});
                }
            }
        }
        for (asUINT g = 0; g < module->GetGlobalVarCount(); g++)
        {
            const char *varName = nullptr;
            module->GetGlobalVar(g, &varName, nullptr, nullptr);
            if (varName && (ctx.partialMember.empty() || std::string_view(varName).starts_with(ctx.partialMember)))
            {
                itemsArray.push_back({{"label", varName}, {"kind", 6}, {"detail", "Global variable"}});
            }
        }
    }

    for (const auto &tf : tokenFuncs)
    {
        if (IsInternalCompilerFunction(tf.name) || addedFunctions[tf.name])
            continue;
        addedFunctions[tf.name] = true;

        if (ctx.partialMember.empty() || tf.name.starts_with(ctx.partialMember))
        {
            itemsArray.push_back({{"label", tf.name},
                                  {"kind", 3},
                                  {"detail", EnhanceIfFuncdef(CleanSignature(tf.declaration))},
                                  {"insertText", tf.name + "($1)"},
                                  {"insertTextFormat", 2}});
        }
    }

    for (const auto &nativeVar : tokenGlobalVars)
    {
        if (ctx.partialMember.empty() || nativeVar.name.starts_with(ctx.partialMember))
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
        if (asIScriptFunction *func = engine->GetGlobalFunctionByIndex(f))
        {
            std::string funcName = func->GetName();
            if (IsInternalCompilerFunction(funcName) || addedFunctions[funcName])
                continue;

            if (ctx.partialMember.empty() || funcName.starts_with(ctx.partialMember))
            {
                itemsArray.push_back({{"label", funcName},
                                      {"kind", 3},
                                      {"detail", EnhanceIfFuncdef(CleanSignature(func->GetDeclaration(true, false, true)))},
                                      {"insertText", funcName + "($1)"},
                                      {"insertTextFormat", 2}});
            }
        }
    }

    for (asUINT g = 0; g < engine->GetGlobalPropertyCount(); g++)
    {
        const char *varName = nullptr;
        engine->GetGlobalPropertyByIndex(g, &varName, nullptr, nullptr, nullptr);
        if (varName && (ctx.partialMember.empty() || std::string_view(varName).starts_with(ctx.partialMember)))
        {
            itemsArray.push_back({{"label", varName}, {"kind", 6}, {"detail", "Native global"}});
        }
    }
}

bool CompletionHandler::IsBaseClass(const std::string &child, const std::string &potentialBase)
{
    if (child == potentialBase)
        return true;

    std::vector<std::string> stack;
    std::unordered_set<std::string> visitedClasses;
    stack.push_back(child);

    while (!stack.empty())
    {
        std::string currentClass = std::move(stack.back());
        stack.pop_back();

        if (!visitedClasses.insert(currentClass).second)
            continue;

        for (const auto &c : customClasses)
        {
            if (c.name != currentClass)
                continue;
            for (const auto &base : c.baseTypes)
            {
                if (base == potentialBase)
                    return true;
                stack.push_back(base);
            }
            break;
        }
    }
    return false;
}

void CompletionHandler::AddImplicitMembersRecursive(const std::string &targetClass, std::unordered_set<std::string> &addedImplicitMembers)
{
    std::vector<std::string> stack;
    std::unordered_set<std::string> visitedClasses;
    stack.push_back(targetClass);

    while (!stack.empty())
    {
        std::string currentClass = std::move(stack.back());
        stack.pop_back();

        if (!visitedClasses.insert(currentClass).second)
            continue;

        for (const auto &c : customClasses)
        {
            if (c.name != currentClass)
                continue;

            for (const auto &[propName, prop] : c.properties)
            {
                if (!addedImplicitMembers.insert(prop.name).second)
                    continue;
                if (ctx.partialMember.empty() || prop.name.starts_with(ctx.partialMember))
                {
                    itemsArray.push_back({{"label", prop.name},
                                          {"kind", 5},
                                          {"detail", prop.access + " " + prop.typeName},
                                          {"documentation", {{"kind", "markdown"}, {"value", fmt::format("Member property of `{}`", currentClass)}}}});
                }
            }

            for (const auto &[methodName, overloads] : c.methods)
            {
                for (const auto &method : overloads)
                {
                    if (method.isConstructor)
                        continue;
                    if (method.name.starts_with("get_") || method.name.starts_with("set_"))
                        continue;
                    if (!addedImplicitMembers.insert(method.name).second)
                        continue;

                    if (ctx.partialMember.empty() || method.name.starts_with(ctx.partialMember))
                    {
                        itemsArray.push_back({{"label", method.name},
                                              {"kind", 2},
                                              {"detail", method.access + " " + method.declaration},
                                              {"documentation", {{"kind", "markdown"}, {"value", fmt::format("Member method of `{}`", currentClass)}}}});
                    }
                }
            }

            for (const auto &base : c.baseTypes)
            {
                stack.push_back(base);
            }
            break;
        }
    }
}

std::string CompletionHandler::ResolveRootType(const std::string &rootName, bool rootIsMethod)
{
    if (rootName == "super" && !enclosingClass.empty())
    {
        for (const auto &c : customClasses)
        {
            if (c.name == enclosingClass && !c.baseTypes.empty())
                return c.baseTypes[0];
        }
    }

    bool isStaticAccess = std::ranges::any_of(customClasses, [&rootName](const auto &c)
                                              { return c.name == rootName; }) ||
                          GetNativeTypeInfo(rootName) != nullptr;

    if (rootIsMethod)
    {
        if (isStaticAccess)
            return rootName;

        std::string outType;
        if (!enclosingClass.empty() && SearchCustomClassRecursively(enclosingClass, rootName, true, outType, customClasses))
            return outType;

        for (const auto &tf : tokenFuncs)
        {
            if (tf.name == rootName)
                return tf.typeName;
        }

        for (asUINT f = 0; f < engine->GetGlobalFunctionCount(); f++)
        {
            if (asIScriptFunction *func = engine->GetGlobalFunctionByIndex(f))
            {
                if (func->GetName() == rootName)
                {
                    if (const char *decl = engine->GetTypeDeclaration(func->GetReturnTypeId(), true))
                        return decl;
                }
            }
        }
        return "";
    }

    if (isStaticAccess)
        return rootName;
    if (rootName == "this" && !enclosingClass.empty())
        return enclosingClass;

    for (const auto &v : localVars)
    {
        if (v.name == rootName)
            return v.typeName;
    }
    for (const auto &v : tokenGlobalVars)
    {
        if (v.name == rootName)
            return v.typeName;
    }

    std::string outType;
    if (!enclosingClass.empty() && SearchCustomClassRecursively(enclosingClass, rootName, false, outType, customClasses))
        return outType;

    for (asUINT g = 0; g < engine->GetGlobalPropertyCount(); g++)
    {
        const char *varName = nullptr;
        int typeId = 0;
        engine->GetGlobalPropertyByIndex(g, &varName, nullptr, &typeId, nullptr);
        if (varName && rootName == varName)
        {
            if (const char *decl = engine->GetTypeDeclaration(typeId, true))
                return decl;
        }
    }
    return "";
}

std::string CompletionHandler::WalkObjectChain(std::string inferredTypeName)
{
    if (logger)
        logger(fmt::format("Starting WalkObjectChain with base type: '{}'", inferredTypeName));

    for (size_t i = 1; i < ctx.objectChain.size(); ++i)
    {
        if (inferredTypeName.empty())
            break;

        std::string nextName;
        int nextDeref = 0;
        bool nextIsMethod = false;
        ParseSegment(ctx.objectChain[i], nextName, nextDeref, nextIsMethod);

        if (logger)
            logger(fmt::format("-> Evaluating segment: '{}' (Is Method: {}, Deref: {})", nextName, nextIsMethod, nextDeref));

        std::string nextType;
        std::string_view baseTypeName = TokenHarvester::GetBaseType(inferredTypeName);
        bool foundInScript = SearchCustomClassRecursively(std::string(baseTypeName), nextName, nextIsMethod, nextType, customClasses);

        if (!foundInScript)
        {
            if (asITypeInfo *t = GetNativeTypeInfo(inferredTypeName))
            {
                if (!nextIsMethod)
                {
                    for (asUINT p = 0; p < t->GetPropertyCount(); p++)
                    {
                        const char *pName = nullptr;
                        int pTypeId = 0;
                        t->GetProperty(p, &pName, &pTypeId);
                        if (pName && std::string_view(pName) == nextName)
                        {
                            if (const char *decl = engine->GetTypeDeclaration(pTypeId, true))
                                nextType = decl;
                            break;
                        }
                    }
                }
                if (nextType.empty())
                {
                    for (asUINT m = 0; m < t->GetMethodCount(); m++)
                    {
                        asIScriptFunction *func = t->GetMethodByIndex(m);
                        if (func && std::string_view(func->GetName()) == nextName)
                        {
                            if (const char *decl = engine->GetTypeDeclaration(func->GetReturnTypeId(), true))
                                nextType = decl;
                            break;
                        }
                    }
                }
            }
        }

        if (nextType.empty())
        {
            inferredTypeName.clear();
            continue;
        }

        std::string_view cleanNext = TokenHarvester::GetBaseType(nextType);
        if (cleanNext.length() <= 2)
        {
            nextType = inferredTypeName;
            nextDeref++;
        }

        for (int d = 0; d < nextDeref; d++)
        {
            nextType = std::string(TokenHarvester::ExtractInnerType(nextType));
        }
        inferredTypeName = std::string(TokenHarvester::GetInstantiatedType(nextType));
    }
    return inferredTypeName;
}

void CompletionHandler::PopulateMembers(const std::string &inferredTypeName)
{
    std::string_view baseTypeName = TokenHarvester::GetBaseType(inferredTypeName);
    asITypeInfo *targetType = GetNativeTypeInfo(inferredTypeName);
    std::unordered_set<std::string> addedMembers;

    bool canAccessPrivate = (enclosingClass == baseTypeName);
    bool canAccessProtected = IsBaseClass(enclosingClass, std::string(baseTypeName));

    if (targetType && (targetType->GetFlags() & asOBJ_ENUM))
    {
        if (ctx.lastSeparator == "::")
        {
            for (asUINT v = 0; v < targetType->GetEnumValueCount(); v++)
            {
                if (const char *enumName = targetType->GetEnumValueByIndex(v, nullptr))
                {
                    if (!addedMembers.insert(enumName).second)
                        continue;
                    if (ctx.partialMember.empty() || std::string_view(enumName).starts_with(ctx.partialMember))
                    {
                        itemsArray.push_back({{"label", enumName}, {"kind", 20}, {"detail", "enum value"}});
                    }
                }
            }
        }
        return;
    }

    ExtractClassMembers(std::string(baseTypeName), addedMembers, canAccessPrivate, canAccessProtected);
    bool classFoundInScript = std::ranges::any_of(customClasses, [baseTypeName](const auto &c)
                                                  { return c.name == baseTypeName; });

    if (!classFoundInScript && targetType)
    {
        for (asUINT p = 0; p < targetType->GetPropertyCount(); p++)
        {
            if (ctx.lastSeparator == "::")
                continue;
            const char *propName = nullptr;
            int propTypeId = 0;
            targetType->GetProperty(p, &propName, &propTypeId);
            if (!addedMembers.insert(propName).second)
                continue;

            if (ctx.partialMember.empty() || std::string_view(propName).starts_with(ctx.partialMember))
            {
                const char *decl = engine->GetTypeDeclaration(propTypeId, true);
                std::string subbedDetail = decl ? decl : "primitive";
                SubstituteTemplateArguments(subbedDetail, inferredTypeName);
                subbedDetail = EnhanceIfFuncdef(subbedDetail);
                itemsArray.push_back({{"label", propName}, {"kind", 5}, {"detail", subbedDetail}});
            }
        }
        for (asUINT m = 0; m < targetType->GetMethodCount(); m++)
        {
            asIScriptFunction *func = targetType->GetMethodByIndex(m);
            std::string mName = func->GetName();
            if (mName == baseTypeName || (!mName.empty() && mName[0] == '~'))
                continue;
            if (ctx.lastSeparator == "::" && !canAccessProtected)
                continue;
            if (ctx.lastSeparator != "::" && (mName.starts_with("get_") || mName.starts_with("set_")))
                continue;
            if (!addedMembers.insert(mName).second)
                continue;

            if (ctx.partialMember.empty() || mName.starts_with(ctx.partialMember))
            {
                std::string subbedDetail = func->GetDeclaration(true, false, true);
                SubstituteTemplateArguments(subbedDetail, inferredTypeName);
                subbedDetail = CleanSignature(subbedDetail);
                subbedDetail = EnhanceIfFuncdef(subbedDetail);
                itemsArray.push_back({{"label", mName},
                                      {"kind", 2},
                                      {"detail", subbedDetail},
                                      {"insertText", mName + "($1)"},
                                      {"insertTextFormat", 2}});
            }
        }
    }
}

asITypeInfo *CompletionHandler::GetNativeTypeInfo(const std::string &typeName)
{
    if (typeName.empty())
        return nullptr;

    asITypeInfo *t = module ? module->GetTypeInfoByDecl(typeName.c_str()) : nullptr;
    if (t)
        return t;

    t = engine->GetTypeInfoByDecl(typeName.c_str());
    if (t)
        return t;

    t = module ? module->GetTypeInfoByName(typeName.c_str()) : nullptr;
    if (t)
        return t;

    t = engine->GetTypeInfoByName(typeName.c_str());
    if (t)
        return t;

    std::string_view base = TokenHarvester::GetBaseType(typeName);
    t = module ? module->GetTypeInfoByName(std::string(base).c_str()) : nullptr;
    if (t)
        return t;

    t = engine->GetTypeInfoByName(std::string(base).c_str());
    if (t)
        return t;

    for (asUINT idx = 0; idx < engine->GetObjectTypeCount(); idx++)
    {
        t = engine->GetObjectTypeByIndex(idx);
        if (t && t->GetName() == base)
            return t;
    }
    return nullptr;
}

void CompletionHandler::ParseSegment(const std::string &segment, std::string &outName, int &outDerefCount, bool &outIsMethod)
{
    outName = segment;
    outDerefCount = 0;
    outIsMethod = false;

    for (char c : segment)
    {
        if (c == '[')
            outDerefCount++;
    }

    size_t cutoff = outName.find_first_of("([");
    if (cutoff != std::string::npos)
    {
        if (outName[cutoff] == '(')
            outIsMethod = true;
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
    size_t p = 0;
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
    res.reserve(str.size());
    finalRes.reserve(str.size());

    for (size_t idx = 0; idx < str.size(); ++idx)
    {
        char c = str[idx];
        if (c == ' ')
        {
            if (!res.empty() && (res.back() == '<' || res.back() == '>' || res.back() == '@' || res.back() == '&' || res.back() == '('))
                continue;
            if (idx + 1 < str.size() && (str[idx + 1] == '<' || str[idx + 1] == '>' || str[idx + 1] == '@' || str[idx + 1] == '&' || str[idx + 1] == '(' || str[idx + 1] == ',' || str[idx + 1] == ')'))
                continue;
        }
        res += c;
    }

    for (size_t idx = 0; idx < res.size(); ++idx)
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
    for (char c : typeStr)
    {
        if (c == '@' || c == '&' || c == ' ' || c == '<')
            break;
        baseName += c;
    }
    return baseName;
}

std::string CompletionHandler::DeduceTypeFromRHS(const std::string &objectName)
{
    std::string name;
    int derefCount = 0;
    bool isMethod = false;

    ParseSegment(objectName, name, derefCount, isMethod);
    std::string resolvedType = ResolveRootType(name, isMethod);

    for (int vIdx = 0; vIdx < derefCount; ++vIdx)
    {
        resolvedType = std::string(TokenHarvester::ExtractInnerType(resolvedType));
    }
    return std::string(TokenHarvester::GetInstantiatedType(resolvedType));
}

void CompletionHandler::SubstituteTemplateArguments(std::string &targetStr, std::string_view templateType)
{
    size_t startAngle = templateType.find('<');
    size_t endAngle = templateType.rfind('>');

    if (startAngle != std::string::npos && endAngle != std::string::npos && endAngle > startAngle)
    {
        std::string_view templateArg = templateType.substr(startAngle + 1, endAngle - startAngle - 1);
        size_t tPos = 0;
        while ((tPos = targetStr.find('T', tPos)) != std::string::npos)
        {
            bool leftOk = (tPos == 0 || (!std::isalnum(static_cast<unsigned char>(targetStr[tPos - 1])) && targetStr[tPos - 1] != '_'));
            bool rightOk = (tPos + 1 == targetStr.length() || (!std::isalnum(static_cast<unsigned char>(targetStr[tPos + 1])) && targetStr[tPos + 1] != '_'));

            if (leftOk && rightOk)
            {
                targetStr.replace(tPos, 1, templateArg);
                tPos += templateArg.length();
            }
            else
                tPos++;
        }
    }
}

std::string CompletionHandler::EnhanceIfFuncdef(const std::string &hoverText)
{
    std::string baseHover = hoverText;
    if (!engine)
        return baseHover;

    std::string_view cleanedView = StripAccessModifiers(baseHover);
    size_t firstSpace = cleanedView.find(' ');
    if (firstSpace != std::string_view::npos)
    {
        std::string typeName = std::string(cleanedView.substr(0, firstSpace));
        if (!typeName.empty() && typeName.back() == '@')
            typeName.pop_back();

        std::string paramsSuffix;
        asIScriptModule *mod = engine->GetModule("LSPModule");
        asITypeInfo *fdefInfo = mod ? mod->GetTypeInfoByName(typeName.c_str()) : nullptr;
        if (!fdefInfo)
            fdefInfo = engine->GetTypeInfoByName(typeName.c_str());

        if (fdefInfo && (fdefInfo->GetFlags() & asOBJ_FUNCDEF))
        {
            if (asIScriptFunction *fdefFunc = fdefInfo->GetFuncdefSignature())
            {
                std::string fullFuncDecl = fdefFunc->GetDeclaration(true, true);
                size_t parenPos = fullFuncDecl.find('(');
                if (parenPos != std::string::npos)
                    paramsSuffix = fullFuncDecl.substr(parenPos);
            }
        }

        if (!paramsSuffix.empty())
            baseHover += paramsSuffix;
    }
    return baseHover;
}

bool CompletionHandler::IsInternalCompilerFunction(std::string_view name) const noexcept
{
    return name.find('$') != std::string_view::npos;
}