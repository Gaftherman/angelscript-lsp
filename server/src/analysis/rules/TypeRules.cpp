#include "analysis/rules/TypeRules.h"
#include "analysis/SemanticHelpers.h"

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
            return IsReservedKeyword(name) || IsPrimitiveTypeName(name) ||
                   name == ctx.request.GetStringTypeName() || name == ctx.request.GetArrayTypeName();
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
        if (sym.type != SymbolType::Funcdef || IsFromPredefinedStub(sym, ctx))
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

        for (const auto &param : sig.parameters)
        {
            if (param.hasPrimitiveHandle)
            {
                ctx.LogRule("ValidateFuncdef", "as-err-handle-on-primitive", sym);
                ctx.Emit(sym, "as-err-handle-on-primitive", param.baseTypeName);
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
            if (sym.fileUri != ctx.request.fileUri)
            {
                continue;
            }
            if (IsDestructorDeclaration(sym, ctx) || ctx.request.GetRuleIndex().enumMemberNames.contains(sym.name))
            {
                continue;
            }
            local.push_back(&sym);
        }

        if (local.size() < 2)
        {
            return;
        }

        for (size_t i = 1; i < local.size(); ++i)
        {
            const Symbol &other = *local[i];

            for (size_t j = 0; j < i; ++j)
            {
                const Symbol &first = *local[j];

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

                ctx.LogRule("ValidateDuplicates", "as-err-duplicate-symbol", other);
                ctx.EmitAtRange(other.selectionRange.startLine, other.selectionRange.startCharacter,
                                other.selectionRange.endLine, other.selectionRange.endCharacter,
                                "as-err-duplicate-symbol", other.name);
                break;
            }
        }
    }
}
