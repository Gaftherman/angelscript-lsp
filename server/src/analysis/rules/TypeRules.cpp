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
        /** @brief True when the symbol was declared in a predefined stub, which the rules exempt. */
        bool IsFromPredefinedStub(const Symbol &sym, const DiagnosticContext &ctx)
        {
            const std::string &extension = ctx.request.predefinedFileExtension;
            return !extension.empty() && sym.fileUri.ends_with(extension);
        }

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

        /**
         * @brief True when the declaration is a destructor.
         *
         * SymbolCollector records `~Foo()` under the bare name "Foo", so a class with both a
         * constructor and a destructor puts two zero-parameter functions in one bucket. Nothing on
         * the signature distinguishes them, so the tilde is read back from the source.
         */
        bool IsDestructor(const Symbol &sym, const DiagnosticContext &ctx)
        {
            const std::string_view source = ctx.request.sourceCode;
            if (source.empty() || sym.fileUri != ctx.request.fileUri)
            {
                return false;
            }

            // Walk to the start of the declaration's line, then to the identifier.
            size_t offset = 0;
            for (uint32_t line = 0; line < sym.selectionRange.startLine; ++line)
            {
                offset = source.find('\n', offset);
                if (offset == std::string_view::npos)
                {
                    return false;
                }
                ++offset;
            }

            offset += sym.selectionRange.startCharacter;
            if (offset == 0 || offset > source.size())
            {
                return false;
            }

            size_t back = offset;
            while (back > 0 && (source[back - 1] == ' ' || source[back - 1] == '\t'))
            {
                --back;
            }
            return back > 0 && source[back - 1] == '~';
        }

        /** @brief True when the name belongs to some enum's member list.
         *  @note Enum members land in the table under their own name, and two enums in one scope
         *        declaring the same member name is ordinary AngelScript rather than a redeclaration.
         *        Matched against every enum's member list rather than through containerName, which
         *        for an enum nested in a namespace names the namespace, not the enum. */
        bool IsEnumMemberName(const std::string &name, const SymbolTable &table)
        {
            bool found = false;
            table.ForEachSymbol(
                [&](const std::string &, const std::vector<Symbol> &symbols)
                {
                    if (found)
                    {
                        return;
                    }
                    for (const auto &sym : symbols)
                    {
                        if (sym.type != SymbolType::Enum)
                        {
                            continue;
                        }
                        for (const auto &member : sym.GetEnum().members)
                        {
                            if (member.name == name)
                            {
                                found = true;
                                return;
                            }
                        }
                    }
                });
            return found;
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
        if (sym.type != SymbolType::Typedef || IsFromPredefinedStub(sym, ctx))
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

        // NOT IMPLEMENTED: as-err-typedef-non-primitive and as-err-typedef-unresolved.
        //
        // The grammar's typedef_declaration is `"typedef" primitive_type identifier ";"`, so a
        // typedef of anything else never parses as a typedef at all - `typedef Entity Alias;` is
        // recovered as a variable declaration and the parser pass reports the syntax error. There
        // is no TypedefSignature for these rules to inspect, and the constraint is already enforced
        // structurally. Re-enable only if the grammar widens base_type.
        (void)baseType;
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

        // 'external shared enum X;' is the one bodyless form, same as for classes.
        if (!sig.hasBraces && !sig.modifiers.isExternal)
        {
            ctx.LogRule("ValidateEnum", "as-syntax-error", sym);
            ctx.Emit(sym, "as-syntax-error", sym.name);
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
        // NOT IMPLEMENTED: as-err-interface-constructor.
        //
        // The grammar's interface_method requires a return_type, so `IThing();` inside an interface
        // never parses as a member and is not collected at all - there is nothing in the symbol
        // table for this rule to find. The parser pass reports the construct as a syntax error,
        // which is the diagnostic the user actually sees today. Re-enable if interface_method ever
        // accepts a constructor form.
        (void)sym;
        (void)ctx;
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
            if (sym.fileUri != ctx.request.fileUri || IsFromPredefinedStub(sym, ctx))
            {
                continue;
            }
            if (IsDestructor(sym, ctx) || IsEnumMemberName(sym.name, ctx.request.symbolTable))
            {
                continue;
            }
            local.push_back(&sym);
        }

        if (local.size() < 2)
        {
            return;
        }

        // Reported against the first declaration only, so N redeclarations produce N-1 findings
        // rather than one per pair.
        const Symbol &first = *local[0];

        for (size_t i = 1; i < local.size(); ++i)
        {
            const Symbol &other = *local[i];

            // The same declaration reached twice through different index paths is not a
            // redeclaration; only distinct source positions are.
            if (first.startLine == other.startLine && first.startCharacter == other.startCharacter)
            {
                continue;
            }

            if (first.type != other.type)
            {
                ctx.LogRule("ValidateDuplicates", "as-err-name-conflict", other);
                ctx.Emit(other, "as-err-name-conflict", other.name, KindWord(first.type));
                continue;
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
                    continue;
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
            }

            // A virtual property's accessors share one name by design.
            if (first.type == SymbolType::Variable &&
                (first.GetVariable().isVirtualProperty || other.GetVariable().isVirtualProperty))
            {
                continue;
            }

            ctx.LogRule("ValidateDuplicates", "as-err-duplicate-symbol", other);
            ctx.Emit(other, "as-err-duplicate-symbol", other.name);
        }
    }
}
