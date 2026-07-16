#include "i18n/LspStrings.h"

namespace i18n {

static const LspStrings EN_STRINGS = {
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
    .kindUnknown = "Symbol",

    .hoverIn = "in",
    .hoverBuiltinFunc = "Built-in Function",
    .hoverBuiltinType = "Built-in Type",
    .hoverEngineError = "Engine Error:",
    .hoverEngineWarn = "Engine Warning:",
    .hoverUnresolved = "Unresolved symbol",
    .hoverAmbiguous = "Ambiguous symbol"
};

static const LspStrings ES_STRINGS = {
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
    .kindUnknown = "Símbolo",

    .hoverIn = "en",
    .hoverBuiltinFunc = "Función Integrada",
    .hoverBuiltinType = "Tipo Integrado",
    .hoverEngineError = "Error del Motor:",
    .hoverEngineWarn = "Advertencia del Motor:",
    .hoverUnresolved = "Símbolo no resuelto",
    .hoverAmbiguous = "Símbolo ambiguo"
};

Locale ParseLocale(const std::string& localeStr)
{
    if (localeStr.starts_with("es")) {
        return Locale::ES;
    }
    if (localeStr.starts_with("en")) {
        return Locale::EN;
    }
    return Locale::UNKNOWN;
}

const LspStrings& GetStrings(Locale locale)
{
    switch (locale) {
        case Locale::ES: return ES_STRINGS;
        case Locale::EN:
        default:
            return EN_STRINGS;
    }
}

} // namespace i18n
