#pragma once

#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include <angelscript.h>

// Introspects an initialized AngelScript engine and emits a predefined stub file.
//
// The language server needs to know every type, method, property, and function that the host
// application exposes to scripts so its semantic analyzer and completion engine can resolve symbols
// without access to a running engine instance. Previously, these declarations had to be maintained
// by hand in fixture files like sdk-addons.as.predefined, but manual descriptions inevitably drift
// from what the engine was actually configured to accept. When an oracle and a predefined stub
// disagree, test suites surface phantom discrepancies about the fixture rather than about the analyzer.
//
// This header provides the engine reflection pass for `angelscript_oracle --dump-registry`. It walks
// the engine's internal registration tables directly and writes out syntactically valid AngelScript
// declarations. Emitting the engine's actual configuration guarantees that the resulting stub matches
// ground truth without human intervention.

/**
 * @brief Walks everything the AngelScript engine has registered and writes an AngelScript stub.
 *
 * This function reflects over all enums, typedefs, funcdefs, object types, global properties,
 * and global functions currently registered in the engine. It partitions all declarations by their
 * enclosing namespace so that each namespace is emitted as a single consolidated block, with the
 * global namespace emitted at the top level without a wrapper. Internal symbols containing '$'
 * are filtered out because they represent synthetic engine behaviours rather than valid script syntax.
 *
 * @param engine The initialized AngelScript engine whose registration table should be dumped.
 * @param out The output stream where the predefined AngelScript stub declarations will be written.
 */
inline void DumpRegistry(asIScriptEngine *engine, std::FILE *out)
{
    if (engine == nullptr || out == nullptr)
    {
        return;
    }

    // A helper to test whether a string contains the internal engine symbol marker '$'.
    // AngelScript internally invents identifier names containing '$' for compiler-generated helper
    // behaviours and list factories, such as '$list' constructors for array templates. These exist
    // only to satisfy internal engine mechanisms and are not legal AngelScript identifiers in user
    // source code, so emitting them would produce syntax errors when the stub is parsed.
    auto containsInternalMarker = [](const char *str) -> bool {
        if (str == nullptr)
        {
            return false;
        }
        for (const char *p = str; *p != '\0'; ++p)
        {
            if (*p == '$')
            {
                return true;
            }
        }
        return false;
    };

    struct EnumValue
    {
        std::string name;
        long long value = 0;
    };

    struct EnumDef
    {
        std::string name;
        std::vector<EnumValue> values;
    };

    struct ClassDef
    {
        std::string header;
        std::vector<std::string> properties;
        std::vector<std::string> methods;
    };

    struct NamespaceContent
    {
        std::vector<EnumDef> enums;
        std::vector<std::string> typedefs;
        std::vector<std::string> funcdefs;
        std::vector<ClassDef> classes;
        std::vector<std::string> properties;
        std::vector<std::string> functions;
    };

    // Declarations grouped by their namespace name. The global namespace is keyed by the empty
    // string ("") and will be iterated first because std::map sorts lexicographically.
    std::map<std::string, NamespaceContent> namespaces;

    // 1. Enums
    // Each enum reports its name and namespace through asITypeInfo, and its values through
    // GetEnumValueByIndex. Values are emitted in numeric assignment form.
    const asUINT enumCount = engine->GetEnumCount();
    for (asUINT i = 0; i < enumCount; ++i)
    {
        asITypeInfo *t = engine->GetEnumByIndex(i);
        if (t == nullptr)
        {
            continue;
        }
        const char *name = t->GetName();
        if (name == nullptr || containsInternalMarker(name))
        {
            continue;
        }

        EnumDef enumDef;
        enumDef.name = name;

        const asUINT valCount = t->GetEnumValueCount();
        for (asUINT v = 0; v < valCount; ++v)
        {
            asINT64 val = 0;
            const char *valName = t->GetEnumValueByIndex(v, &val);
            if (valName == nullptr || containsInternalMarker(valName))
            {
                continue;
            }
            enumDef.values.push_back(EnumValue{valName, static_cast<long long>(val)});
        }

        const char *rawNs = t->GetNamespace();
        const std::string ns = (rawNs != nullptr) ? rawNs : "";
        namespaces[ns].enums.push_back(enumDef);
    }

    // 2. Typedefs
    // Typedefs report their underlying type ID through GetTypedefTypeId, which we convert back to
    // a type declaration string via the engine.
    const asUINT typedefCount = engine->GetTypedefCount();
    for (asUINT i = 0; i < typedefCount; ++i)
    {
        asITypeInfo *t = engine->GetTypedefByIndex(i);
        if (t == nullptr)
        {
            continue;
        }
        const char *name = t->GetName();
        if (name == nullptr || containsInternalMarker(name))
        {
            continue;
        }

        // GetUnderlyingTypeId, not GetTypedefTypeId: the latter was deprecated in 2.39.0 and is
        // compiled only under AS_DEPRECATED, which this build does not define.
        const int typeId = t->GetUnderlyingTypeId();
        const char *underlying = engine->GetTypeDeclaration(typeId, true);
        if (underlying == nullptr || containsInternalMarker(underlying))
        {
            continue;
        }

        const std::string decl = "typedef " + std::string(underlying) + " " + std::string(name) + ";";
        const char *rawNs = t->GetNamespace();
        const std::string ns = (rawNs != nullptr) ? rawNs : "";
        namespaces[ns].typedefs.push_back(decl);
    }

    // 3. Funcdefs
    // Funcdefs represent script function callback signatures. The declaration obtained from the
    // signature is prefixed with "funcdef " unless the SDK already included the keyword. If the
    // funcdef belongs to a namespace, any namespace qualification on the funcdef name itself is
    // stripped so the declaration remains valid inside the enclosing namespace block.
    const asUINT funcdefCount = engine->GetFuncdefCount();
    for (asUINT i = 0; i < funcdefCount; ++i)
    {
        asITypeInfo *t = engine->GetFuncdefByIndex(i);
        if (t == nullptr)
        {
            continue;
        }
        const char *name = t->GetName();
        if (name != nullptr && containsInternalMarker(name))
        {
            continue;
        }
        // A funcdef declared INSIDE a template is skipped, and this is not tidiness - asking the
        // SDK to render one segfaults. `less` is the case: scriptarray registers
        // `funcdef bool less(const T&in, const T&in)` as a member of `array<T>`, and outside an
        // instantiation its parameter types name a subtype that does not exist yet, so
        // GetDeclaration walks into a null object type.
        //
        // Skipping it loses nothing a stub wants. It is not a global funcdef; it belongs to
        // `array<T>`, and a stub that declared a bare `funcdef bool less(...)` at namespace level
        // would be describing something the engine does not have.
        if (asITypeInfo *parent = t->GetParentType(); parent != nullptr)
        {
            continue;
        }

        asIScriptFunction *sig = t->GetFuncdefSignature();
        if (sig == nullptr)
        {
            continue;
        }
        const char *decl = sig->GetDeclaration(false, true, true);
        if (decl == nullptr || containsInternalMarker(decl))
        {
            continue;
        }

        std::string declStr = decl;
        const char *rawNs = t->GetNamespace();
        const std::string ns = (rawNs != nullptr) ? rawNs : "";

        if (!ns.empty())
        {
            const size_t parenPos = declStr.find('(');
            if (parenPos != std::string::npos)
            {
                size_t nameEnd = parenPos;
                while (nameEnd > 0 && (declStr[nameEnd - 1] == ' ' || declStr[nameEnd - 1] == '\t'))
                {
                    --nameEnd;
                }
                size_t nameStart = nameEnd;
                while (nameStart > 0 && declStr[nameStart - 1] != ' ' && declStr[nameStart - 1] != '\t' &&
                       declStr[nameStart - 1] != '*' && declStr[nameStart - 1] != '&')
                {
                    --nameStart;
                }
                const std::string qualifiedName = declStr.substr(nameStart, nameEnd - nameStart);
                const std::string expectedPrefix = ns + "::";
                if (qualifiedName.rfind(expectedPrefix, 0) == 0)
                {
                    declStr.erase(nameStart, expectedPrefix.size());
                }
            }
        }

        if (declStr.rfind("funcdef ", 0) != 0)
        {
            declStr = "funcdef " + declStr;
        }

        if (declStr.empty() || declStr.back() != ';')
        {
            declStr += ';';
        }

        namespaces[ns].funcdefs.push_back(declStr);
    }

    // 4. Object Types
    // Registered object types represent classes and interfaces. The template types `array<T>` and
    // `grid<T>` report their registered names as "array" and "grid" without their type parameters;
    // we expand them into `class array<class T>` and `class grid<class T>` so the language server
    // recognizes them as templates. Base classes and implemented interfaces are appended as a
    // comma-separated inheritance list.
    const asUINT objCount = engine->GetObjectTypeCount();
    for (asUINT i = 0; i < objCount; ++i)
    {
        asITypeInfo *t = engine->GetObjectTypeByIndex(i);
        if (t == nullptr)
        {
            continue;
        }
        const char *name = t->GetName();
        if (name == nullptr || containsInternalMarker(name))
        {
            continue;
        }

        std::string className = name;
        if (className == "array")
        {
            className = "array<class T>";
        }
        else if (className == "grid")
        {
            className = "grid<class T>";
        }

        std::vector<std::string> bases;
        asITypeInfo *baseType = t->GetBaseType();
        if (baseType != nullptr)
        {
            const char *baseName = baseType->GetName();
            if (baseName != nullptr && !containsInternalMarker(baseName))
            {
                const char *baseNs = baseType->GetNamespace();
                const char *typeNs = t->GetNamespace();
                const std::string baseNsStr = (baseNs != nullptr) ? baseNs : "";
                const std::string typeNsStr = (typeNs != nullptr) ? typeNs : "";
                if (!baseNsStr.empty() && baseNsStr != typeNsStr)
                {
                    bases.push_back(baseNsStr + "::" + baseName);
                }
                else
                {
                    bases.push_back(baseName);
                }
            }
        }

        const asUINT ifaceCount = t->GetInterfaceCount();
        for (asUINT j = 0; j < ifaceCount; ++j)
        {
            asITypeInfo *iface = t->GetInterface(j);
            if (iface != nullptr)
            {
                const char *ifaceName = iface->GetName();
                if (ifaceName != nullptr && !containsInternalMarker(ifaceName))
                {
                    const char *ifaceNs = iface->GetNamespace();
                    const char *typeNs = t->GetNamespace();
                    const std::string ifaceNsStr = (ifaceNs != nullptr) ? ifaceNs : "";
                    const std::string typeNsStr = (typeNs != nullptr) ? typeNs : "";
                    if (!ifaceNsStr.empty() && ifaceNsStr != typeNsStr)
                    {
                        bases.push_back(ifaceNsStr + "::" + ifaceName);
                    }
                    else
                    {
                        bases.push_back(ifaceName);
                    }
                }
            }
        }

        ClassDef classDef;
        classDef.header = "class " + className;
        if (!bases.empty())
        {
            classDef.header += " : ";
            for (size_t k = 0; k < bases.size(); ++k)
            {
                classDef.header += (k > 0 ? ", " : "") + bases[k];
            }
        }

        const asUINT propCount = t->GetPropertyCount();
        for (asUINT p = 0; p < propCount; ++p)
        {
            const char *propDecl = t->GetPropertyDeclaration(p, false);
            if (propDecl == nullptr || containsInternalMarker(propDecl))
            {
                continue;
            }
            std::string propStr = propDecl;
            if (propStr.empty() || propStr.back() != ';')
            {
                propStr += ';';
            }
            classDef.properties.push_back(propStr);
        }

        // Constructors and destructors are BEHAVIOURS, not methods, and leaving them out was the
        // first thing this dump got wrong: compared against the hand-written stub it looked as
        // though the stub had invented `any()`, `ref()`, `~ref()` and every other constructor,
        // when what had actually happened was that GetMethodByIndex does not report them.
        //
        // A value type registers asBEHAVE_CONSTRUCT and asBEHAVE_DESTRUCT; a reference type
        // registers asBEHAVE_FACTORY instead, whose declaration returns a handle rather than
        // naming the type. Both read as a constructor in a stub, which is what a stub is for -
        // it describes what a script may write, and a script writes `ref r;` either way.
        //
        // The LIST_ variants are skipped: their declarations name the internal `$list` type, which
        // is not script syntax. The initializer list a type accepts is described by @listpattern
        // in a stub instead - see analysis/ListPattern.h.
        const asUINT behaviourCount = t->GetBehaviourCount();
        for (asUINT b = 0; b < behaviourCount; ++b)
        {
            asEBehaviours behaviour = asBEHAVE_CONSTRUCT;
            asIScriptFunction *fn = t->GetBehaviourByIndex(b, &behaviour);
            if (fn == nullptr)
            {
                continue;
            }
            if (behaviour != asBEHAVE_CONSTRUCT && behaviour != asBEHAVE_DESTRUCT &&
                behaviour != asBEHAVE_FACTORY)
            {
                continue;
            }

            const char *behaviourDecl = fn->GetDeclaration(false, false, true);
            if (behaviourDecl == nullptr || containsInternalMarker(behaviourDecl))
            {
                continue;
            }

            std::string text = behaviourDecl;
            if (text.empty() || text.back() != ';')
            {
                text += ';';
            }
            classDef.methods.push_back(text);
        }

        const asUINT methodCount = t->GetMethodCount();
        for (asUINT m = 0; m < methodCount; ++m)
        {
            asIScriptFunction *method = t->GetMethodByIndex(m, true);
            if (method == nullptr)
            {
                continue;
            }
            const char *methodDecl = method->GetDeclaration(false, false, true);
            if (methodDecl == nullptr || containsInternalMarker(methodDecl))
            {
                continue;
            }
            std::string methodStr = methodDecl;
            if (methodStr.empty() || methodStr.back() != ';')
            {
                methodStr += ';';
            }
            classDef.methods.push_back(methodStr);
        }

        const char *rawNs = t->GetNamespace();
        const std::string ns = (rawNs != nullptr) ? rawNs : "";
        namespaces[ns].classes.push_back(classDef);
    }

    // 5. Global Properties
    // Global properties represent variables bound at script scope. We retrieve the type declaration
    // with namespaces included and prefix "const " when the property was registered as constant.
    const asUINT globalPropCount = engine->GetGlobalPropertyCount();
    for (asUINT i = 0; i < globalPropCount; ++i)
    {
        const char *propName = nullptr;
        const char *propNs = nullptr;
        int typeId = 0;
        bool isConst = false;
        const int r = engine->GetGlobalPropertyByIndex(i, &propName, &propNs, &typeId, &isConst,
                                                       nullptr, nullptr, nullptr);
        if (r < 0 || propName == nullptr || containsInternalMarker(propName))
        {
            continue;
        }

        const char *typeDecl = engine->GetTypeDeclaration(typeId, true);
        if (typeDecl == nullptr || containsInternalMarker(typeDecl))
        {
            continue;
        }

        const std::string decl = (isConst ? "const " : "") + std::string(typeDecl) + " " +
                                 std::string(propName) + ";";
        const std::string ns = (propNs != nullptr) ? propNs : "";
        namespaces[ns].properties.push_back(decl);
    }

    // 6. Global Functions
    // Global functions are retrieved with `includeNamespace = true` so parameter and return types
    // resolve correctly. The function name itself may be prefixed with its namespace by the SDK,
    // which we extract to determine the enclosing namespace and strip from the declaration so the
    // resulting stub syntax remains valid within its namespace block.
    const asUINT globalFuncCount = engine->GetGlobalFunctionCount();
    for (asUINT i = 0; i < globalFuncCount; ++i)
    {
        asIScriptFunction *func = engine->GetGlobalFunctionByIndex(i);
        if (func == nullptr)
        {
            continue;
        }
        const char *decl = func->GetDeclaration(false, true, true);
        if (decl == nullptr || containsInternalMarker(decl))
        {
            continue;
        }

        std::string declStr = decl;
        std::string ns;

        const size_t parenPos = declStr.find('(');
        if (parenPos != std::string::npos)
        {
            size_t nameEnd = parenPos;
            while (nameEnd > 0 && (declStr[nameEnd - 1] == ' ' || declStr[nameEnd - 1] == '\t'))
            {
                --nameEnd;
            }
            size_t nameStart = nameEnd;
            while (nameStart > 0 && declStr[nameStart - 1] != ' ' && declStr[nameStart - 1] != '\t' &&
                   declStr[nameStart - 1] != '*' && declStr[nameStart - 1] != '&')
            {
                --nameStart;
            }
            const std::string qualifiedName = declStr.substr(nameStart, nameEnd - nameStart);
            const size_t colonPos = qualifiedName.rfind("::");
            if (colonPos != std::string::npos)
            {
                ns = qualifiedName.substr(0, colonPos);
                declStr.erase(nameStart, colonPos + 2);
            }
        }

        if (declStr.empty() || declStr.back() != ';')
        {
            declStr += ';';
        }

        namespaces[ns].functions.push_back(declStr);
    }

    // Emission Pass
    // 1. Output the header comment block explaining the origin and purpose of this stub.
    std::fprintf(out,
                 "// This file was generated by angelscript_oracle --dump-registry.\n"
                 "//\n"
                 "// It represents the AngelScript engine's own registration table dumped directly from the\n"
                 "// runtime, rather than an approximate or hand-written description of it. It reflects every\n"
                 "// type, property, method, and function that the engine has actually registered.\n"
                 "// Editing it by hand defeats the point: any manual changes will diverge from what the engine\n"
                 "// really accepts, and will be overwritten whenever the registry is regenerated.\n\n");

    // 2. Output each namespace block. The global namespace has an empty name ("") and is emitted
    // without any wrapping block, while named namespaces are enclosed in `namespace <ns> { ... }`.
    for (const auto &[ns, content] : namespaces)
    {
        const bool isEmpty = content.enums.empty() && content.typedefs.empty() &&
                             content.funcdefs.empty() && content.classes.empty() &&
                             content.properties.empty() && content.functions.empty();
        if (isEmpty)
        {
            continue;
        }

        const bool isGlobal = ns.empty();
        const std::string indent = isGlobal ? "" : "    ";
        const std::string innerIndent = isGlobal ? "    " : "        ";

        if (!isGlobal)
        {
            std::fprintf(out, "namespace %s\n{\n", ns.c_str());
        }

        // Enums
        for (const EnumDef &enm : content.enums)
        {
            std::fprintf(out, "%senum %s\n", indent.c_str(), enm.name.c_str());
            std::fprintf(out, "%s{\n", indent.c_str());
            for (const EnumValue &val : enm.values)
            {
                std::fprintf(out, "%s%s = %lld,\n", innerIndent.c_str(), val.name.c_str(), val.value);
            }
            std::fprintf(out, "%s}\n\n", indent.c_str());
        }

        // Typedefs
        for (const std::string &td : content.typedefs)
        {
            std::fprintf(out, "%s%s\n", indent.c_str(), td.c_str());
        }
        if (!content.typedefs.empty())
        {
            std::fprintf(out, "\n");
        }

        // Funcdefs
        for (const std::string &fd : content.funcdefs)
        {
            std::fprintf(out, "%s%s\n", indent.c_str(), fd.c_str());
        }
        if (!content.funcdefs.empty())
        {
            std::fprintf(out, "\n");
        }

        // Object Types (Classes)
        for (const ClassDef &cls : content.classes)
        {
            std::fprintf(out, "%s%s\n", indent.c_str(), cls.header.c_str());
            std::fprintf(out, "%s{\n", indent.c_str());
            for (const std::string &prop : cls.properties)
            {
                std::fprintf(out, "%s%s\n", innerIndent.c_str(), prop.c_str());
            }
            for (const std::string &method : cls.methods)
            {
                std::fprintf(out, "%s%s\n", innerIndent.c_str(), method.c_str());
            }
            std::fprintf(out, "%s}\n\n", indent.c_str());
        }

        // Global Properties
        for (const std::string &prop : content.properties)
        {
            std::fprintf(out, "%s%s\n", indent.c_str(), prop.c_str());
        }
        if (!content.properties.empty())
        {
            std::fprintf(out, "\n");
        }

        // Global Functions
        for (const std::string &func : content.functions)
        {
            std::fprintf(out, "%s%s\n", indent.c_str(), func.c_str());
        }
        if (!content.functions.empty())
        {
            std::fprintf(out, "\n");
        }

        if (!isGlobal)
        {
            std::fprintf(out, "}\n\n");
        }
    }
}
