#include "i18n/LspStrings.h"

namespace i18n
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
            
            .hoverField         = "field",
            .hoverLocalVariable = "local variable",
            .hoverEnumMember    = "enum member"
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
            
            .hoverField         = "campo",
            .hoverLocalVariable = "variable local",
            .hoverEnumMember    = "miembro de enum"
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

} // namespace i18n
