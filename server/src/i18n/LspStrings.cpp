/**
 * @file LspStrings.cpp
 * @brief Implementation of static localized string tables for EN and ES locales.
 * @ingroup i18n
 */

#include "i18n/LspStrings.h"

namespace angel_lsp::i18n
{
    static const LspStrings EN_STRINGS =
        {
            .kindVariable = "Variable",
            .kindFunction = "Function",
            .kindClass = "Class",
            .kindNamespace = "Namespace",
            .kindParameter = "Parameter",
            .kindProperty = "Property",
            .kindMethod = "Method",
            .kindEnum = "Enum",
            .kindEnumMember = "Enum Member",
            .kindInterface = "Interface",
            .kindFuncdef = "Funcdef",
            .kindMixin = "Mixin",
            .kindTypedef = "Typedef",
            .kindConstructor = "Constructor",
            .kindDestructor = "Destructor",
            .kindUnknown = "Symbol",

            .hoverIn = "In",
            .hoverBuiltinFunc = "Built-in Function",
            .hoverBuiltinType = "Built-in Type",
            .hoverEngineError = "Engine Error:",
            .hoverEngineWarn = "Engine Warning:",
            .hoverUnresolved = "Unresolved symbol",
            .hoverAmbiguous = "Ambiguous symbol",
            .hoverDefinedIn = "Defined in",
            .hoverAliasTo = "Alias to",

            .hoverTemplateParams = "Template Parameters",
            .hoverParams = "Parameters",
            .hoverReturns = "Returns",
            .hoverThrows = "Throws",
            .hoverNote = "Note",
            .hoverWarning = "Warning",
            .hoverDeprecated = "Deprecated",
            .hoverOverloads = "overloads",
            .hoverParameterOf = "Parameter of",
            .hoverNeverUsed = "This symbol is never used locally",
            
            .hoverParameter     = "parameter",
            .hoverProperty      = "property",
            .hoverField         = "field",
            .hoverLocalVariable = "local variable",
            .hoverEnumMember    = "enum member",
            .hoverIncludedFile  = "Included script file:"
        };

    static const LspStrings ES_STRINGS =
        {
            .kindVariable = "Variable",
            .kindFunction = "Función",
            .kindClass = "Clase",
            .kindNamespace = "Espacio de Nombres",
            .kindParameter = "Parámetro",
            .kindProperty = "Propiedad",
            .kindMethod = "Método",
            .kindEnum = "Enumeración",
            .kindEnumMember = "Miembro de Enum",
            .kindInterface = "Interfaz",
            .kindFuncdef = "Definición de Función",
            .kindMixin = "Mixin",
            .kindTypedef = "Definición de Tipo",
            .kindConstructor = "Constructor",
            .kindDestructor = "Destructor",
            .kindUnknown = "Símbolo",

            .hoverIn = "En",
            .hoverBuiltinFunc = "Función Integrada",
            .hoverBuiltinType = "Tipo Integrado",
            .hoverEngineError = "Error del Motor:",
            .hoverEngineWarn = "Advertencia del Motor:",
            .hoverUnresolved = "Símbolo no resuelto",
            .hoverAmbiguous = "Símbolo ambiguo",
            .hoverDefinedIn = "Definido en",
            .hoverAliasTo = "Alias de",

            .hoverTemplateParams = "Parámetros de plantilla",
            .hoverParams = "Parámetros",
            .hoverReturns = "Devuelve",
            .hoverThrows = "Excepciones",
            .hoverNote = "Nota",
            .hoverWarning = "Advertencia",
            .hoverDeprecated = "En desuso",
            .hoverOverloads = "sobrecargas",
            .hoverParameterOf = "Parámetro de",
            .hoverNeverUsed = "Este símbolo nunca se usa localmente",
            
            .hoverParameter     = "parámetro",
            .hoverProperty      = "propiedad",
            .hoverField         = "campo",
            .hoverLocalVariable = "variable local",
            .hoverEnumMember    = "miembro de enum",
            .hoverIncludedFile  = "Archivo de script incluido:"
        };

    Locale ParseLocale(const std::string &localeStr)
    {
        if (localeStr.starts_with("es"))
        {
            return Locale::ES;
        }
        if (localeStr.starts_with("en"))
        {
            return Locale::EN;
        }
        return Locale::UNKNOWN;
    }

    const LspStrings &GetStrings(Locale locale)
    {
        switch (locale)
        {
        case Locale::ES:
            return ES_STRINGS;
        case Locale::EN:
        default:
            return EN_STRINGS;
        }
    }

    static const DoxygenHeaderStrings EN_DOXYGEN_HEADERS =
        {
            .parameters = "Parameters:",
            .typeParameters = "Type Parameters:",
            .returns = "Returns:",
            .exceptions = "Exceptions:",
            .warning = "Warning:",
            .deprecated = "Deprecated:",
            .note = "Note:",
            .seeAlso = "See also:"
        };

    static const DoxygenHeaderStrings ES_DOXYGEN_HEADERS =
        {
            .parameters = "Parámetros:",
            .typeParameters = "Parámetros de Tipo:",
            .returns = "Retorna:",
            .exceptions = "Excepciones:",
            .warning = "Advertencia:",
            .deprecated = "Obsoleto:",
            .note = "Nota:",
            .seeAlso = "Ver también:"
        };

    DoxygenHeaderStrings GetDoxygenHeaders(Locale locale)
    {
        switch (locale)
        {
        case Locale::ES:
            return ES_DOXYGEN_HEADERS;
        case Locale::EN:
        default:
            return EN_DOXYGEN_HEADERS;
        }
    }

} // namespace angel_lsp::i18n
