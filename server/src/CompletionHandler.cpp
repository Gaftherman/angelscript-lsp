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

/**
 * @brief Generates an array of JSON autocompletion items based on the current context.
 * @param originalText The raw string representation of the source code being analyzed.
 * @param cursorAbsPos The absolute linear index of the cursor position within the text.
 * @return A JSON array containing the completely resolved autocompletion items.
 */
json CompletionHandler::GenerateItems(const std::string &originalText, size_t cursorAbsPos)
{
    if (ctx.isMemberAccess && !ctx.objectChain.empty())
    {
        HandleMemberAccess();
    }
    else
    {
        HandleGlobalScope(originalText, cursorAbsPos);
    }
    return itemsArray;
}

/**
 * @brief Evaluates a member access chain and populates autocompletion items for the natively resolved object type.
 */
void CompletionHandler::HandleMemberAccess()
{
    std::string rootName;
    int rootDeref = 0;
    bool rootIsMethod = false;
    ParseSegment(ctx.objectChain[0], rootName, rootDeref, rootIsMethod);

    // 1. Buscamos qué tipo de variable es la raíz (Ej. g_Global3DMatrix)
    std::string inferredTypeName = ResolveRootType(rootName, rootIsMethod);

    if (logger)
        logger(fmt::format("ResolveRootType encontró el tipo original: '{}'", inferredTypeName));

    // 2. Si el usuario usó corchetes en la raíz (Ej. matrix[0].), extraemos
    for (int d = 0; d < rootDeref; d++)
    {
        inferredTypeName = TokenHarvester::ExtractInnerType(inferredTypeName);
    }

    // ==============================================================
    // ¡LA LÍNEA CRÍTICA DEL BUG!
    // Aquí debemos usar GetInstantiatedType para NO perder los < >.
    // Si usas GetBaseType aquí, destruirás el array antes de evaluarlo.
    // ==============================================================
    inferredTypeName = TokenHarvester::GetInstantiatedType(inferredTypeName);

    if (logger)
        logger(fmt::format("Tipo listo para WalkObjectChain: '{}'", inferredTypeName));

    // 3. Pasamos el tipo completo y con sus plantillas intactas al evaluador
    inferredTypeName = WalkObjectChain(inferredTypeName);

    // 4. Poblamos el IntelliSense con el resultado
    if (!inferredTypeName.empty())
    {
        PopulateMembers(inferredTypeName);
    }
}

bool SearchCustomClassRecursively(const std::string &typeName, const std::string &memberName, bool isMethod, std::string &outType, const std::vector<TokenHarvester::ScriptClass> &customClasses)
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
            // Buscar recursivamente en Mixins y Herencias
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
 * @brief Determines the initial data type at the root of a member access chain.
 * @param rootName The identifier name of the root object or function.
 * @param rootIsMethod Flag indicating whether the root identifier represents a function call.
 * @return The resolved data type name of the root object as a string.
 */
std::string CompletionHandler::ResolveRootType(const std::string &rootName, bool rootIsMethod)
{
    bool isStaticAccess = false;

    if (rootName == "super" && !enclosingClass.empty())
    {
        for (const auto &c : customClasses)
        {
            // Si estamos en esta clase y tiene un padre, devolvemos al padre
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
            return rootName;

        // FIX 2: Búsqueda implícita de métodos de clase (Ej: si llamas a "Mover()" sin "this.")
        if (!enclosingClass.empty())
        {
            std::string outType;
            if (SearchCustomClassRecursively(enclosingClass, rootName, true, outType, customClasses))
            {
                return outType;
            }
        }

        for (const auto &f : tokenFuncs)
        {
            if (f.name == rootName)
                return f.typeName;
        }
        for (asUINT f = 0; f < engine->GetGlobalFunctionCount(); f++)
        {
            asIScriptFunction *func = engine->GetGlobalFunctionByIndex(f);
            if (func && std::string(func->GetName()) == rootName)
            {
                int typeId = func->GetReturnTypeId();
                const char *decl = engine->GetTypeDeclaration(typeId, true);
                return decl ? decl : "";
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

    // FIX 2: Búsqueda implícita de variables de clase (Ej: "gridData." sin "this.")
    if (!enclosingClass.empty())
    {
        std::string outType;
        if (SearchCustomClassRecursively(enclosingClass, rootName, false, outType, customClasses))
        {
            return outType;
        }
    }

    for (asUINT g = 0; g < engine->GetGlobalPropertyCount(); g++)
    {
        const char *varName = nullptr;
        int typeId = 0;
        engine->GetGlobalPropertyByIndex(g, &varName, nullptr, &typeId, nullptr);
        if (varName && std::string(varName) == rootName)
        {
            const char *decl = engine->GetTypeDeclaration(typeId, true);
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
std::string CompletionHandler::WalkObjectChain(std::string inferredTypeName)
{
    if (logger)
        logger(fmt::format("Iniciando WalkObjectChain con tipo base: '{}'", inferredTypeName));

    for (size_t i = 1; i < ctx.objectChain.size(); ++i)
    {
        if (inferredTypeName.empty())
            break;

        std::string nextName;
        int nextDeref = 0;
        bool nextIsMethod = false;
        ParseSegment(ctx.objectChain[i], nextName, nextDeref, nextIsMethod);

        if (logger)
            logger(fmt::format("-> Evaluando segmento: '{}' (Es Método: {}, Deref: {})", nextName, nextIsMethod, nextDeref));

        std::string nextType = "";
        bool foundInScript = false;

        std::string baseTypeName = TokenHarvester::GetBaseType(inferredTypeName);

        // ==============================================================
        // FIX: Búsqueda Recursiva con Mixins y Herencia
        // ==============================================================
        foundInScript = SearchCustomClassRecursively(baseTypeName, nextName, nextIsMethod, nextType, customClasses);

        if (!foundInScript)
        {
            asITypeInfo *t = GetNativeTypeInfo(inferredTypeName);

            if (t)
            {
                if (!nextIsMethod)
                {
                    for (asUINT p = 0; p < t->GetPropertyCount(); p++)
                    {
                        const char *pName = nullptr;
                        int pTypeId = 0;
                        t->GetProperty(p, &pName, &pTypeId);
                        if (pName && std::string(pName) == nextName)
                        {
                            const char *decl = engine->GetTypeDeclaration(pTypeId, true);
                            if (decl)
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
                        if (func && std::string(func->GetName()) == nextName)
                        {
                            int rTypeId = func->GetReturnTypeId();
                            const char *decl = engine->GetTypeDeclaration(rTypeId, true);
                            if (decl)
                                nextType = decl;
                            break;
                        }
                    }
                }
            }
            else
            {
                if (logger)
                    logger(fmt::format("ERROR: No se encontró asITypeInfo para '{}'", inferredTypeName));
            }
        }

        if (!nextType.empty())
        {
            if (logger)
                logger(fmt::format("AngelScript devolvió el tipo: '{}'", nextType));

            std::string cleanNext = TokenHarvester::GetBaseType(nextType);

            if (cleanNext.length() <= 2)
            {
                if (logger)
                    logger(fmt::format("Genérico detectado ('{}'). Forzando extracción matemática de la plantilla.", cleanNext));
                nextType = inferredTypeName;
                nextDeref++;
            }

            for (int d = 0; d < nextDeref; d++)
            {
                nextType = TokenHarvester::ExtractInnerType(nextType);
            }

            inferredTypeName = TokenHarvester::GetInstantiatedType(nextType);
            if (logger)
                logger(fmt::format("Resultado tras extracción: '{}'", inferredTypeName));
        }
        else
        {
            inferredTypeName = "";
            if (logger)
                logger("Resultado: Vacío (Cadena rota).");
        }
    }

    return inferredTypeName;
}

/**
 * @brief Extracts class or native members of a specified type and appends them to the autocompletion items list.
 * @param inferredTypeName The fully resolved type name whose members should be retrieved.
 */
void CompletionHandler::PopulateMembers(const std::string &inferredTypeName)
{
    std::string baseTypeName = TokenHarvester::GetBaseType(inferredTypeName);
    bool classFoundInScript = false;

    // ==============================================================
    // FIX: Búsqueda de linaje para permitir acceso a 'protected'
    // ==============================================================
    std::function<bool(const std::string &, const std::string &)> IsBaseClass = [&](const std::string &child, const std::string &potentialBase)
    {
        if (child == potentialBase)
            return true;
        for (const auto &c : customClasses)
        {
            if (c.name == child)
            {
                for (const auto &b : c.baseTypes)
                {
                    if (IsBaseClass(b, potentialBase))
                        return true;
                }
                break;
            }
        }
        return false;
    };

    bool canAccessPrivate = (enclosingClass == baseTypeName);
    bool canAccessProtected = IsBaseClass(enclosingClass, baseTypeName); // ¡Permiso otorgado al hijo!

    std::unordered_set<std::string> addedMembers;

    std::function<void(const std::string &)> ExtractClassMembers = [&](const std::string &targetClass)
    {
        for (const auto &c : customClasses)
        {
            if (c.name == targetClass)
            {
                classFoundInScript = true;
                for (const auto &prop : c.properties)
                {
                    // =======================================================
                    // FIX: Bloquear propiedades (miembros dinámicos) si se usó "::"
                    // =======================================================
                    if (ctx.lastSeparator == "::")
                        continue;

                    if (!canAccessPrivate && (prop.access == "private" || prop.access == "protected"))
                        continue;
                    if (addedMembers.find(prop.name) != addedMembers.end())
                        continue;

                    if (ctx.partialMember.empty() || prop.name.rfind(ctx.partialMember, 0) == 0)
                    {
                        itemsArray.push_back({{"label", prop.name}, {"kind", 5}, {"detail", prop.access + " " + prop.typeName}});
                        addedMembers.insert(prop.name);
                    }
                }
                for (const auto &method : c.methods)
                {
                    if (method.isConstructor)
                        continue;

                    // Aplicar las nuevas reglas de visibilidad
                    if (method.access == "private" && !canAccessPrivate)
                        continue;
                    if (method.access == "protected" && !canAccessProtected)
                        continue;

                    // Ocultar métodos virtuales autogenerados (get_ y set_)
                    if (method.name.find("get_") == 0 || method.name.find("set_") == 0)
                        continue;

                    // Si el hijo ya hizo "override", el método del padre se ignora
                    if (addedMembers.find(method.name) != addedMembers.end())
                        continue;

                    if (ctx.partialMember.empty() || method.name.rfind(ctx.partialMember, 0) == 0)
                    {
                        itemsArray.push_back({{"label", method.name}, {"kind", 2}, {"detail", method.access + " " + method.declaration}, {"insertText", method.name + "($1)"}, {"insertTextFormat", 2}});
                        addedMembers.insert(method.name); // Registrar para bloquear al padre
                    }
                }

                // Recorrer las Clases Base y Mixins
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
        asITypeInfo *targetType = GetNativeTypeInfo(inferredTypeName);
        if (targetType)
        {
            // =========================================================
            // LÓGICA ESTRICTA PARA ENUMS
            // =========================================================
            if (targetType->GetFlags() & asOBJ_ENUM)
            {
                // Solo permitimos mostrar miembros si el usuario escribió "::"
                if (ctx.lastSeparator == "::")
                {
                    for (asUINT v = 0; v < targetType->GetEnumValueCount(); v++)
                    {
                        const char *enumName = targetType->GetEnumValueByIndex(v, nullptr);
                        if (enumName)
                        {
                            if (addedMembers.find(enumName) != addedMembers.end())
                                continue;

                            if (ctx.partialMember.empty() || std::string(enumName).rfind(ctx.partialMember, 0) == 0)
                            {
                                // Ícono oficial de C++ para Enums (20 = EnumMember)
                                itemsArray.push_back({{"label", enumName}, {"kind", 20}, {"detail", "enum value"}});
                                addedMembers.insert(enumName);
                            }
                        }
                    }
                }
                // Los Enums no tienen métodos ni propiedades, detenemos la ejecución aquí
                return;
            }

            // =========================================================
            // LÓGICA PARA CLASES NORMALES
            // =========================================================
            for (asUINT p = 0; p < targetType->GetPropertyCount(); p++)
            {
                // =======================================================
                // FIX: Bloquear propiedades dinámicas nativas si se usó "::"
                // =======================================================
                if (ctx.lastSeparator == "::")
                    continue;

                const char *propName = nullptr;
                int propTypeId = 0;
                targetType->GetProperty(p, &propName, &propTypeId);

                if (addedMembers.find(propName) != addedMembers.end())
                    continue;

                if (ctx.partialMember.empty() || std::string(propName).rfind(ctx.partialMember, 0) == 0)
                {
                    const char *decl = engine->GetTypeDeclaration(propTypeId, true);
                    itemsArray.push_back({{"label", propName}, {"kind", 5}, {"detail", decl ? decl : "primitive"}});
                    addedMembers.insert(propName);
                }
            }
            for (asUINT m = 0; m < targetType->GetMethodCount(); m++)
            {
                asIScriptFunction *func = targetType->GetMethodByIndex(m);
                std::string mName = func->GetName();

                if (mName == baseTypeName || (!mName.empty() && mName[0] == '~'))
                    continue;

                if (addedMembers.find(mName) != addedMembers.end())
                    continue;

                if (ctx.partialMember.empty() || mName.rfind(ctx.partialMember, 0) == 0)
                {
                    itemsArray.push_back({{"label", mName}, {"kind", 2}, {"detail", func->GetDeclaration(true, false, true)}, {"insertText", mName + "($1)"}, {"insertTextFormat", 2}});
                    addedMembers.insert(mName);
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
void CompletionHandler::HandleGlobalScope(const std::string &originalText, size_t cursorAbsPos)
{
    if (cursorAbsPos > 0 && originalText[cursorAbsPos - 1] == '@')
        return;

    // 1. Añadimos todas las Keywords (incluyendo booleanos y null)
    std::vector<std::string> keywords = {
        "super", "this", "get", "set", "property", "const",
        "override", "final", "mixin", "class", "interface", "enum",
        "void", "int", "float", "bool", "string", "array", "auto", "return",
        "null", "true", "false"};

    for (const auto &kw : keywords)
    {
        if (ctx.partialMember.empty() || kw.rfind(ctx.partialMember, 0) == 0)
        {
            itemsArray.push_back({{"label", kw}, {"kind", 14}, {"detail", "keyword"}});
        }
    }

    // 2. Variables Locales
    for (const auto &v : localVars)
    {
        if (ctx.partialMember.empty() || v.name.rfind(ctx.partialMember, 0) == 0)
        {
            itemsArray.push_back({{"label", v.name}, {"kind", 6}, {"detail", "local " + v.typeName}});
        }
    }

    // =====================================================================
    // 3. FIX: Miembros implícitos de la clase actual (Equivalente a "this.")
    // Ahora busca de forma recursiva en clases Padre y Mixins
    // =====================================================================
    if (!enclosingClass.empty())
    {
        std::unordered_set<std::string> addedImplicitMembers;

        std::function<void(const std::string &)> AddImplicitMembers = [&](const std::string &targetClass)
        {
            for (const auto &c : customClasses)
            {
                if (c.name == targetClass)
                {
                    for (const auto &prop : c.properties)
                    {
                        if (addedImplicitMembers.find(prop.name) != addedImplicitMembers.end())
                            continue;

                        if (ctx.partialMember.empty() || prop.name.rfind(ctx.partialMember, 0) == 0)
                        {
                            itemsArray.push_back({{"label", prop.name}, {"kind", 5}, {"detail", prop.access + " " + prop.typeName}, {"documentation", {{"kind", "markdown"}, {"value", fmt::format("Member property of `{}`", targetClass)}}}});
                            addedImplicitMembers.insert(prop.name);
                        }
                    }
                    for (const auto &method : c.methods)
                    {
                        if (method.isConstructor)
                            continue;
                        // Ocultamos las virtual properties generadas automáticamente
                        if (method.name.find("get_") == 0 || method.name.find("set_") == 0)
                            continue;
                        if (addedImplicitMembers.find(method.name) != addedImplicitMembers.end())
                            continue;

                        if (ctx.partialMember.empty() || method.name.rfind(ctx.partialMember, 0) == 0)
                        {
                            itemsArray.push_back({{"label", method.name}, {"kind", 2}, {"detail", method.access + " " + method.declaration}, {"documentation", {{"kind", "markdown"}, {"value", fmt::format("Member method of `{}`", targetClass)}}}});
                            addedImplicitMembers.insert(method.name);
                        }
                    }

                    // Magia recursiva: Cargar también lo que haya en Padres y Mixins
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

    // 4. Clases y Tipos del Script
    for (const auto &c : customClasses)
    {
        if (ctx.partialMember.empty() || c.name.rfind(ctx.partialMember, 0) == 0)
        {
            itemsArray.push_back({{"label", c.name}, {"kind", 7}, {"detail", "class/namespace/enum " + c.name}, {"documentation", {{"kind", "markdown"}, {"value", "User-defined script type"}}}});
        }
    }

    // 5. Funciones Globales y Nativas
    std::unordered_map<std::string, bool> addedFunctions;

    if (module)
    {
        for (asUINT f = 0; f < module->GetFunctionCount(); f++)
        {
            asIScriptFunction *func = module->GetFunctionByIndex(f);
            if (func)
            {
                std::string funcName = func->GetName();
                addedFunctions[funcName] = true;
                if (ctx.partialMember.empty() || funcName.rfind(ctx.partialMember, 0) == 0)
                {
                    itemsArray.push_back({{"label", funcName}, {"kind", 3}, {"detail", func->GetDeclaration(true, false, true)}, {"insertText", funcName + "($1)"}, {"insertTextFormat", 2}});
                }
            }
        }
        for (asUINT g = 0; g < module->GetGlobalVarCount(); g++)
        {
            const char *varName = nullptr;
            int typeId = 0;
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
                itemsArray.push_back({{"label", tf.name}, {"kind", 3}, {"detail", tf.declaration}, {"insertText", tf.name + "($1)"}, {"insertTextFormat", 2}});
            }
        }
    }

    for (const auto &nativeVar : tokenGlobalVars)
    {
        if (ctx.partialMember.empty() || nativeVar.name.rfind(ctx.partialMember, 0) == 0)
        {
            bool exists = false;
            for (const auto &item : itemsArray)
            {
                if (item.contains("label") && item["label"] == nativeVar.name)
                {
                    exists = true;
                    break;
                }
            }
            if (!exists)
                itemsArray.push_back({{"label", nativeVar.name}, {"kind", 6}, {"detail", nativeVar.typeName + " " + nativeVar.name}});
        }
    }

    for (asUINT f = 0; f < engine->GetGlobalFunctionCount(); f++)
    {
        asIScriptFunction *func = engine->GetGlobalFunctionByIndex(f);
        if (func)
        {
            std::string funcName = func->GetName();
            if (!addedFunctions[funcName])
            {
                if (ctx.partialMember.empty() || funcName.rfind(ctx.partialMember, 0) == 0)
                {
                    itemsArray.push_back({{"label", funcName}, {"kind", 3}, {"detail", func->GetDeclaration(true, false, true)}, {"insertText", funcName + "($1)"}, {"insertTextFormat", 2}});
                }
            }
        }
    }

    for (asUINT g = 0; g < engine->GetGlobalPropertyCount(); g++)
    {
        const char *varName = nullptr;
        engine->GetGlobalPropertyByIndex(g, &varName, nullptr, nullptr, nullptr);
        if (varName && (ctx.partialMember.empty() || std::string(varName).rfind(ctx.partialMember, 0) == 0))
        {
            itemsArray.push_back({{"label", varName}, {"kind", 6}, {"detail", "Native global"}});
        }
    }
}

/**
 * @brief Retrieves the native AngelScript type information object corresponding to a given type name.
 * @param typeName The string representation of the data type to query.
 * @return A pointer to the corresponding asITypeInfo structure, or nullptr if the type cannot be found.
 */
asITypeInfo *CompletionHandler::GetNativeTypeInfo(const std::string &typeName)
{
    if (typeName.empty())
        return nullptr;

    // FIX: Preguntar SIEMPRE al Module primero. El Engine no conoce las clases de script (ComplexActor).
    if (module)
    {
        asITypeInfo *t = module->GetTypeInfoByDecl(typeName.c_str());
        if (t)
            return t;
    }

    asITypeInfo *t = engine->GetTypeInfoByDecl(typeName.c_str());
    if (t)
        return t;

    if (module)
    {
        t = module->GetTypeInfoByName(typeName.c_str());
        if (t)
            return t;
    }

    t = engine->GetTypeInfoByName(typeName.c_str());
    if (t)
        return t;

    std::string base = TokenHarvester::GetBaseType(typeName);

    if (module)
    {
        t = module->GetTypeInfoByName(base.c_str());
        if (t)
            return t;
    }

    t = engine->GetTypeInfoByName(base.c_str());
    if (t)
        return t;

    for (asUINT idx = 0; idx < engine->GetObjectTypeCount(); idx++)
    {
        asITypeInfo *objType = engine->GetObjectTypeByIndex(idx);
        if (objType && std::string(objType->GetName()) == base)
            return objType;
    }

    return nullptr;
}

/**
 * @brief Parses a discrete textual segment of an object chain into its logical base identifier and applied modifiers.
 * @param segment The raw text fragment from the chain to analyze (e.g., "myArray[0]" or "myFunction()").
 * @param outName Output parameter populated with the base identifier string after parsing.
 * @param outDerefCount Output parameter tracking the number of array dereference modifiers found.
 * @param outIsMethod Output boolean flag asserting true if the segment identifies as a method or function call.
 */
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