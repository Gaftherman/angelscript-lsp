#include "analysis/rules/TypeRules.h"
#include "analysis/SemanticHelpers.h"
#include "utils/Utils.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace angel_lsp::analysis::rules
{
    namespace
    {
        /** @brief True when the name collides with a keyword or a built-in type name. */
        bool IsUnusableName(const std::string &name, const DiagnosticContext &ctx)
        {
            const auto strType = ctx.request.GetStringTypeName();
            const auto arrType = ctx.request.GetArrayTypeName();
            const std::string_view effectiveStrType = strType.empty() ? std::string_view("string") : strType;
            const std::string_view effectiveArrType = arrType.empty() ? std::string_view("array") : arrType;
            return IsReservedKeyword(name) || IsPrimitiveTypeName(name) ||
                   name == effectiveStrType || name == effectiveArrType;
        }

        /** @brief True when the text is an integer literal, decimal or hexadecimal, sign included. */
        bool IsIntegerLiteral(std::string_view text)
        {
            while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
            {
                text.remove_prefix(1);
            }
            while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
            {
                text.remove_suffix(1);
            }
            if (!text.empty() && (text.front() == '-' || text.front() == '+'))
            {
                text.remove_prefix(1);
            }
            if (text.empty())
            {
                return false;
            }

            if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
            {
                return std::all_of(text.begin() + 2, text.end(),
                                   [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; });
            }
            return std::all_of(text.begin(), text.end(),
                               [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; });
        }

        /** @brief Renders a symbol kind the way the name-conflict message reads. */
        std::string KindWord(SymbolType type)
        {
            switch (type)
            {
            case SymbolType::Function:  return "function";
            case SymbolType::Class:     return "class";
            case SymbolType::Interface: return "interface";
            case SymbolType::Enum:      return "enum";
            case SymbolType::Typedef:   return "typedef";
            case SymbolType::Funcdef:   return "funcdef";
            case SymbolType::Namespace: return "namespace";
            default:                    return "variable";
            }
        }
    }

    void ValidateTypedef(const Symbol &sym, const DiagnosticContext &ctx)
    {
        if (sym.type != SymbolType::Typedef)
        {
            return;
        }

        if (IsUnusableName(sym.name, ctx))
        {
            ctx.LogRule("ValidateTypedef", "as-err-reserved-keyword-name", sym);
            ctx.Emit(sym, "as-err-reserved-keyword-name", sym.name);
            return;
        }

        const auto &sig = sym.GetTypedef();
        const std::string baseType = CleanBaseType(sig.baseType);

        if (!sig.hasSemicolon)
        {
            ctx.LogRule("ValidateTypedef", "as-syntax-error-missing", sym);
            ctx.Emit(sym, "as-syntax-error-missing", ";");
        }

        // AngelScript only typedefs a primitive. Its own parser refuses anything else outright -
        // `typedef Entity Alias;` answers "Unexpected token '<identifier>'" - so this is not a
        // question of whether the named type exists: a class, an enum and a name that resolves to
        // nothing are equally invalid here, and none of them needs looking up.
        //
        // NOT IMPLEMENTED: as-err-typedef-unresolved, deleted with this commit. It described a
        // second failure mode - "typedef base type is not defined" - that AngelScript does not
        // have, since the type never gets far enough to be resolved.
        if (!baseType.empty() && !IsPrimitiveTypeName(baseType))
        {
            ctx.LogRule("ValidateTypedef", "as-err-typedef-non-primitive", sym);
            if (sig.baseTypeEndCharacter > sig.baseTypeStartCharacter || sig.baseTypeEndLine > sig.baseTypeStartLine)
            {
                ctx.EmitAtRange(sig.baseTypeStartLine, sig.baseTypeStartCharacter,
                                sig.baseTypeEndLine, sig.baseTypeEndCharacter,
                                "as-err-typedef-non-primitive", baseType);
            }
            else
            {
                ctx.Emit(sym, "as-err-typedef-non-primitive", baseType);
            }
        }
    }

    void ValidateFuncdef(const Symbol &sym, const DiagnosticContext &ctx)
    {
        if (sym.type != SymbolType::Funcdef)
        {
            return;
        }

        if (IsUnusableName(sym.name, ctx))
        {
            ctx.LogRule("ValidateFuncdef", "as-err-reserved-keyword-name", sym);
            ctx.Emit(sym, "as-err-reserved-keyword-name", sym.name);
            return;
        }

        const auto &sig = sym.GetFuncdef();

        // A funcdef names a signature; it has no body, no class and nothing to override, so none of
        // the five function attributes means anything on one and the engine rejects all five. The
        // grammar parses them so this can name the offender rather than leaving a syntax error on
        // the token.
        const std::string_view attribute = FirstAttributeName(sig.modifiers);
        if (!attribute.empty())
        {
            ctx.LogRule("ValidateFuncdef", "as-err-funcdef-attribute", sym);
            ctx.Emit(sym, "as-err-funcdef-attribute", attribute, sym.name);
        }

        if (sig.returnHasPrimitiveHandle)
        {
            ctx.LogRule("ValidateFuncdef", "as-err-handle-on-primitive", sym);
            ctx.Emit(sym, "as-err-handle-on-primitive", sig.returnBaseTypeName);
        }

        const std::string retBase = CleanBaseType(sig.returnBaseTypeName.empty() ? sig.returnType : sig.returnBaseTypeName);
        if (!retBase.empty() && retBase != "void" && retBase != "auto" &&
            !IsKnownType(retBase, ctx))
        {
            ctx.LogRule("ValidateFuncdef", "as-err-unresolved-type", sym);
            if (sig.returnTypeEndCharacter > sig.returnTypeStartCharacter || sig.returnTypeEndLine > sig.returnTypeStartLine)
            {
                ctx.EmitAtRange(sig.returnTypeStartLine, sig.returnTypeStartCharacter,
                                sig.returnTypeEndLine, sig.returnTypeEndCharacter,
                                "as-err-unresolved-type", retBase);
            }
            else
            {
                ctx.Emit(sym, "as-err-unresolved-type", retBase);
            }
        }

        for (const auto &param : sig.parameters)
        {
            if (param.hasPrimitiveHandle)
            {
                ctx.LogRule("ValidateFuncdef", "as-err-handle-on-primitive", sym);
                ctx.Emit(sym, "as-err-handle-on-primitive", param.baseTypeName);
            }

            const std::string paramBase = CleanBaseType(param.baseTypeName.empty() ? param.typeName : param.baseTypeName);
            if (!paramBase.empty() && paramBase != "void" && paramBase != "auto" &&
                !IsKnownType(paramBase, ctx))
            {
                ctx.LogRule("ValidateFuncdef", "as-err-unresolved-type", sym);
                if (param.endCharacter > param.startCharacter || param.endLine > param.startLine)
                {
                    ctx.EmitAtRange(param.startLine, param.startCharacter,
                                    param.endLine, param.endCharacter,
                                    "as-err-unresolved-type", paramBase);
                }
                else
                {
                    ctx.Emit(sym, "as-err-unresolved-type", paramBase);
                }
            }
        }
    }

    void ValidateEnum(const Symbol &sym, const DiagnosticContext &ctx)
    {
        if (sym.type != SymbolType::Enum || IsFromPredefinedStub(sym, ctx))
        {
            return;
        }

        if (IsUnusableName(sym.name, ctx))
        {
            ctx.LogRule("ValidateEnum", "as-err-reserved-keyword-name", sym);
            ctx.Emit(sym, "as-err-reserved-keyword-name", sym.name);
            return;
        }

        const auto &sig = sym.GetEnum();

        // 'external shared enum X;' is the one bodyless form, same as for classes. Reported with
        // the declaration code rather than as-syntax-error: the parser accepted this, so telling
        // the user "syntax error" points them at the wrong thing entirely.
        if (!sig.hasBraces && !sig.modifiers.isExternal)
        {
            ctx.LogRule("ValidateEnum", "as-err-declaration-missing-body", sym);
            ctx.Emit(sym, "as-err-declaration-missing-body", sym.name);
        }

        if (sig.modifiers.isExternal && !sig.modifiers.isShared)
        {
            ctx.LogRule("ValidateEnum", "as-err-external-not-shared", sym);
            ctx.Emit(sym, "as-err-external-not-shared", sym.name);
        }

        if (sig.modifiers.isExternal && sig.modifiers.isShared)
        {
            bool hasFullSharedDefinition = false;
            if (auto symsPtr = ctx.request.symbolTable.FindSymbolsPtr(sym.name))
            {
                for (const auto &s : *symsPtr)
                {
                    if (s.type == SymbolType::Enum && s.GetEnum().hasBraces &&
                        s.GetEnum().modifiers.isShared && !s.GetEnum().modifiers.isExternal)
                    {
                        hasFullSharedDefinition = true;
                        break;
                    }
                }
            }
            if (!hasFullSharedDefinition)
            {
                ctx.LogRule("ValidateEnum", "as-err-external-not-found", sym);
                ctx.Emit(sym, "as-err-external-not-found", sym.name);
            }
        }

        for (const auto &member : sig.members)
        {
            if (member.value.empty())
            {
                continue;
            }

            // Only an outright literal is judged. A member initialised from another enum member,
            // a constant, or an arithmetic expression is perfectly legal and this pass does not
            // evaluate expressions - so anything that is not plainly a non-integer literal passes.
            const bool isLiteralNode = member.valueNodeType == std::string(node_types::StringLiteral) ||
                                       member.valueNodeType == std::string(node_types::BooleanLiteral) ||
                                       member.valueNodeType == std::string(node_types::NullLiteral);
            const bool isNonIntegerNumber = member.valueNodeType == std::string(node_types::NumberLiteral) &&
                                            !IsIntegerLiteral(member.value);

            if (isLiteralNode || isNonIntegerNumber)
            {
                ctx.LogRule("ValidateEnum", "as-err-enum-invalid-initializer", sym);
                ctx.Emit(sym, "as-err-enum-invalid-initializer", member.value);
            }
        }
    }

    void ValidateInterfaceMembers(const Symbol &sym, const DiagnosticContext &ctx)
    {
        if (sym.type != SymbolType::Interface || IsFromPredefinedStub(sym, ctx))
        {
            return;
        }

        // An interface declares a contract, never construction or destruction, and the engine
        // refuses both: `IThing();` answers "Expected identifier / Instead found '('" and
        // `~IThing();` answers "Expected data type / Instead found '~'". The grammar parses them
        // now, which is what lets this say so in those terms.
        //
        // Both forms are spelled with the interface's own name and are the only members that reach
        // the table with no return type at all - an ordinary method always carries one, even
        // `void`. So the empty return type is what identifies them, and the name match is what
        // keeps an unrelated member out.
        const std::string qualified = sym.containerName.empty()
                                          ? sym.name
                                          : sym.containerName + "::" + sym.name;

        const auto members = ctx.request.symbolTable.FindSymbolsPtr(qualified + "::" + sym.name);
        if (!members)
        {
            return;
        }

        for (const auto &member : *members)
        {
            if (member.type != SymbolType::Function || member.fileUri != ctx.request.fileUri)
            {
                continue;
            }
            if (!std::holds_alternative<FunctionSignature>(member.signature) ||
                !member.GetFunction().returnType.empty())
            {
                continue;
            }

            ctx.LogRule("ValidateInterfaceMembers", "as-err-interface-constructor", member);
            ctx.Emit(member, "as-err-interface-constructor", sym.name);
        }
    }

    void ValidateDuplicates(const std::vector<Symbol> &symbols, const DiagnosticContext &ctx)
    {
        if (symbols.size() < 2)
        {
            return;
        }

        // Only declarations from the document under analysis are judged, and only against each
        // other. The same name legitimately appears once per file across a module - that is what
        // an #include of a shared header looks like once every member file has been indexed.
        std::vector<const Symbol *> local;
        std::vector<const Symbol *> moduleMates;
        for (const auto &sym : symbols)
        {
            // CallReference entries are not declarations at all: SymbolCollector records them for
            // calls that appear outside any function body, such as `g_EngineFuncs.CVarGetFloat(...)`
            // in a global initializer. Treating them as redeclarations is what made this rule report
            // twenty thousand times over the corpus - once per call site of every popular helper.
            if (sym.type == SymbolType::CallReference)
            {
                continue;
            }

            // A namespace shares a name with a type deliberately and often - `namespace Type` next
            // to `enum Type`, `namespace AF2Menu` next to `class AF2Menu` - because they are looked
            // up in different domains. Enum members are not declarations in this sense either: an
            // enum whose member repeats its own name is ordinary AngelScript.
            if (sym.type == SymbolType::Namespace || sym.type == SymbolType::Enum)
            {
                continue;
            }
            if (IsDestructorDeclaration(sym, ctx) || ctx.request.GetRuleIndex().enumMemberNames.contains(sym.name))
            {
                continue;
            }

            if (sym.fileUri == ctx.request.fileUri)
            {
                local.push_back(&sym);
                continue;
            }

            // A declaration in another file of the SAME module is a redeclaration; one in another
            // module is not. Skipping every foreign file unconditionally - which is what this did -
            // is right for the case the comment above names, a shared header indexed once per
            // module, and wrong for the one a user hits: the same namespace reopened in two files
            // that reach each other, where the compiler says "A function with the same name and
            // parameters already exists" and this said nothing at all.
            //
            // Stubs are left out on purpose. An as.predefined describes what the host registered in
            // C++, and a script declaring the same signature is a different question from two
            // script sections colliding - not one this rule is the place to answer.
            if (!ctx.request.moduleFileUris.empty() &&
                ctx.request.moduleFileUris.contains(sym.fileUri) &&
                !IsFromPredefinedStub(sym, ctx) &&
                !utils::IsPredefinedFile(ctx.request.fileUri, ctx.request.predefinedFileExtension))
            {
                moduleMates.push_back(&sym);
            }
        }

        // At least one declaration here to report on, and at least two in total to be a duplicate
        // of anything.
        if (local.empty() || local.size() + moduleMates.size() < 2)
        {
            return;
        }

        // The module's other files first, so every pair the loop forms has the later index on this
        // document - which is where the diagnostic has to land.
        std::vector<const Symbol *> candidates = moduleMates;
        candidates.insert(candidates.end(), local.begin(), local.end());

        for (size_t i = 1; i < candidates.size(); ++i)
        {
            const Symbol &other = *candidates[i];

            // Two declarations that both live elsewhere are that file's business; this document
            // publishes diagnostics for itself only, and a range in another file would be nonsense
            // here.
            if (other.fileUri != ctx.request.fileUri)
            {
                continue;
            }

            for (size_t j = 0; j < i; ++j)
            {
                const Symbol &first = *candidates[j];

                // The same declaration reached twice through different index paths is not a
                // redeclaration; only distinct source positions are.
                if (first.startLine == other.startLine && first.startCharacter == other.startCharacter)
                {
                    continue;
                }

                if (first.type != other.type)
                {
                    ctx.LogRule("ValidateDuplicates", "as-err-name-conflict", other);
                    ctx.EmitAtRange(other.selectionRange.startLine, other.selectionRange.startCharacter,
                                    other.selectionRange.endLine, other.selectionRange.endCharacter,
                                    "as-err-name-conflict", other.name, KindWord(first.type));
                    break;
                }

                // Functions may repeat a name as long as the parameter lists differ - or, for a
                // conversion operator, as long as the return types do. `float opConv()` beside
                // `int opConv()` and `string opConv()` is the idiomatic way to write conversions in
                // AngelScript, and comparing parameters alone reports every one of them.
                if (first.type == SymbolType::Function)
                {
                    const auto &firstParams = first.GetFunction().parameters;
                    const auto &otherParams = other.GetFunction().parameters;
                    if (firstParams.size() != otherParams.size())
                    {
                        continue;
                    }
                    if (first.GetFunction().returnType != other.GetFunction().returnType)
                    {
                        if (first.name == "opConv" || first.name == "opImplConv" ||
                            first.name == "opCast" || first.name == "opImplCast")
                        {
                            continue;
                        }
                    }

                    // A const overload is a distinct overload.
                    if (first.GetFunction().modifiers.isConst != other.GetFunction().modifiers.isConst)
                    {
                        continue;
                    }

                    // Compared raw, not through CleanBaseType: that helper strips handles and unwraps
                    // array<T> to T, so it reports `array<string>` and `string` as the same parameter -
                    // which turns two genuine overloads into a redeclaration.
                    bool sameSignature = true;
                    for (size_t p = 0; p < firstParams.size(); ++p)
                    {
                        if (firstParams[p].typeName != otherParams[p].typeName ||
                            firstParams[p].modifier != otherParams[p].modifier ||
                            firstParams[p].isReference != otherParams[p].isReference ||
                            firstParams[p].isHandle != otherParams[p].isHandle)
                        {
                            sameSignature = false;
                            break;
                        }
                    }
                    if (!sameSignature)
                    {
                        continue;
                    }

                    ctx.LogRule("ValidateDuplicates", "as-err-duplicate-symbol", other);
                    ctx.EmitAtRange(other.selectionRange.startLine, other.selectionRange.startCharacter,
                                    other.selectionRange.endLine, other.selectionRange.endCharacter,
                                    "as-err-duplicate-symbol", other.name);
                    break;
                }

                // A virtual property's accessors share one name by design.
                if (first.type == SymbolType::Variable &&
                    (first.GetVariable().isVirtualProperty || other.GetVariable().isVirtualProperty))
                {
                    continue;
                }

                // A forward class declaration completed by a full definition is not a duplicate.
                if (first.type == SymbolType::Class && other.type == SymbolType::Class)
                {
                    if (!first.GetClass().hasBraces || !other.GetClass().hasBraces)
                    {
                        continue;
                    }
                }

                // asEP_IGNORE_DUPLICATE_SHARED_INTF. A host that sets it accepts the same shared
                // interface declared twice, and this rule was reporting it as an error - a false
                // positive on code that compiles, found by measuring the property rather than by
                // any test.
                //
                // Narrow, because the measurement is: a duplicate PLAIN interface is rejected under
                // both settings, and only `shared` on both declarations is what the property
                // forgives. Two probes, both directions.
                if (ctx.request.IgnoresDuplicateSharedInterface() &&
                    first.type == SymbolType::Interface && other.type == SymbolType::Interface &&
                    first.GetInterface().modifiers.isShared && other.GetInterface().modifiers.isShared)
                {
                    continue;
                }

                ctx.LogRule("ValidateDuplicates", "as-err-duplicate-symbol", other);
                ctx.EmitAtRange(other.selectionRange.startLine, other.selectionRange.startCharacter,
                                other.selectionRange.endLine, other.selectionRange.endCharacter,
                                "as-err-duplicate-symbol", other.name);
                break;
            }
        }
    }
}
