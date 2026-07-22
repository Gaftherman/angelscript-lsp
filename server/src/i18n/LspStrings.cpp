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
            .hoverIncludedFile  = "Included script file:",

            .diagSyntaxError = "Syntax error",
            .diagUnexpectedToken = "Syntax error: unexpected token '{}'",
            .diagMissingExpectedToken = "Syntax error: missing expected token",
            .diagUndeclaredSymbol = "Undeclared identifier or symbol '{}'",
            .diagIncludedFileNotFound = "Included file not found: '{}'",
            .diagMissingIncludeDelimiter = "Syntax error: missing include path delimiter",
            .diagUnclosedIncludeDelimiter = "Syntax error: unclosed path delimiter in #include",
            .diagUnexpectedCharsAfterInclude = "Syntax error: unexpected characters after #include directive",
            .diagRedefinitionOfSymbol = "Redefinition of symbol '{}'",
            .diagMethodMustBeCalled = "Method '{}' must be called with ()"
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
            .hoverIncludedFile  = "Archivo de script incluido:",

            .diagSyntaxError = "Error de sintaxis",
            .diagUnexpectedToken = "Error de sintaxis: token inesperado '{}'",
            .diagMissingExpectedToken = "Error de sintaxis: se esperaba un token",
            .diagUndeclaredSymbol = "Identificador o símbolo no declarado '{}'",
            .diagIncludedFileNotFound = "Archivo incluido no encontrado: '{}'",
            .diagMissingIncludeDelimiter = "Error de sintaxis: falta delimitador de ruta en #include",
            .diagUnclosedIncludeDelimiter = "Error de sintaxis: delimitador de ruta sin cerrar en #include",
            .diagUnexpectedCharsAfterInclude = "Error de sintaxis: caracteres no esperados después de directiva #include",
            .diagRedefinitionOfSymbol = "Redefinición del símbolo '{}'",
            .diagMethodMustBeCalled = "El método '{}' debe llamarse con ()"
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
