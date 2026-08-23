#pragma once

#include "analysis/SymbolTable.h"

#include <string>

namespace angel_lsp::analysis
{
    /**
     * @brief Renders AngelScript declarations back into source-faithful text.
     *
     * Every feature that shows a declaration to the user (hover, signature help, completion
     * detail) has to spell the same modifiers the same way. Keeping the rendering here means a
     * modifier that is captured by SymbolCollector cannot silently go missing in one feature
     * while showing up in another.
     */

    /**
     * @brief Renders the access keyword that precedes a class member declaration.
     * @param modifiers Modifier set of the symbol.
     * @return "private " or "protected ", or an empty string for public symbols.
     */
    std::string FormatAccessPrefix(const SymbolModifiers &modifiers);

    /**
     * @brief Renders the declaration-level keywords written before the type of a declaration.
     * @param modifiers Modifier set of the symbol.
     * @return Space-terminated keyword run such as "private shared abstract ", or an empty string.
     * @note 'final'/'abstract' are only emitted here when they were written as declaration
     *       modifiers; as function attributes they belong after the parameter list instead.
     */
    std::string FormatDeclarationPrefix(const SymbolModifiers &modifiers);

    /**
     * @brief Renders the reference/direction suffix of a parameter type.
     * @param param Parameter to describe.
     * @return "&in", "&out", "&inout", a bare "&" for an undirected reference, or an empty string.
     */
    std::string FormatParameterReference(const ParameterInformation &param);

    /**
     * @brief Renders a single parameter the way it is spelled in AngelScript source.
     * @param param Parameter to render.
     * @return Text such as "const string &in name = \"x\"".
     */
    std::string FormatParameter(const ParameterInformation &param);

    /**
     * @brief Renders the function attributes that trail a function declaration.
     * @param modifiers Modifier set of the function.
     * @return Text such as " const override", already prefixed with a space, or an empty string.
     */
    std::string FormatFunctionSuffix(const SymbolModifiers &modifiers);

    /**
     * @brief Renders a return type, re-attaching the '&' of a by-reference return.
     * @param returnType Return type text as captured (already carries 'const' and '@').
     * @param modifiers Modifier set of the function, consulted for the reference flag.
     * @return Return type text, or "void" when nothing was captured.
     */
    std::string FormatReturnType(const std::string &returnType, const SymbolModifiers &modifiers);

    /**
     * @brief Renders a full function or funcdef declaration, modifiers included.
     * @param sym Symbol of type Function or Funcdef.
     * @param qualified True to prefix the name with its container ("Class::name").
     * @return Declaration text, or an empty string when sym is not a function.
     */
    std::string FormatFunctionDeclaration(const Symbol &sym, bool qualified = true);

    /**
     * @brief Renders a variable, field or property declaration, modifiers included.
     * @param var Variable signature to render.
     * @param name Declared name.
     * @return Text such as "private const string m_name".
     */
    std::string FormatVariableDeclaration(const VariableSignature &var, const std::string &name);

    /**
     * @brief Renders a class or interface declaration including bases and modifiers.
     * @param sym Symbol of type Class or Interface.
     * @return Declaration text, or an empty string when sym is neither.
     */
    std::string FormatTypeDeclaration(const Symbol &sym);
}
