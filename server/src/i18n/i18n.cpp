#include "i18n.h"

namespace angel_lsp::i18n
{
    I18n::I18n(const std::string &locale)
        : m_locale(locale)
    {
        m_messages["as-err-duplicate-symbol"] = "Duplicate symbol declaration '{}' in the same scope.";
        m_messages["as-err-mixin-final"] = "A mixin ('{}') cannot be declared as 'final'.";
        m_messages["as-err-mixin-abstract"] = "A mixin ('{}') cannot be declared as 'abstract'.";
        m_messages["as-err-missing-body"] = "Function '{}' must have a body '{{}}'.";
        m_messages["as-err-out-param-default"] = "The '&out' parameter '{}' cannot have a default value.";
        m_messages["as-err-template-class-not-supported"] = "Template/generic class definition ('{}') is only allowed in predefined files.";
        m_messages["as-err-inherit-final"] = "Cannot inherit from final class '{}'.";
        m_messages["as-syntax-error"] = "Syntax error: \"{}\"";
        m_messages["as-syntax-error-missing"] = "Syntax error: missing '{}'";
        m_messages["as-syntax-error-generic"] = "Syntax error";

        if (locale == "es")
        {
            m_messages["as-err-duplicate-symbol"] = "Redeclaración de símbolo '{}' en el mismo ámbito.";
            m_messages["as-err-mixin-final"] = "Un mixin ('{}') no puede ser declarado como 'final'.";
            m_messages["as-err-mixin-abstract"] = "Un mixin ('{}') no puede ser declarado como 'abstract'.";
            m_messages["as-err-missing-body"] = "La función '{}' debe tener un cuerpo '{{}}'.";
            m_messages["as-err-out-param-default"] = "El parámetro '&out' '{}' no puede tener un valor por defecto.";
            m_messages["as-err-template-class-not-supported"] = "La definición de clases plantilla/genéricas ('{}') solo está permitida en archivos predefinidos.";
            m_messages["as-err-inherit-final"] = "No se puede heredar de la clase final '{}'.";
            m_messages["as-syntax-error"] = "Error de sintaxis: \"{}\"";
            m_messages["as-syntax-error-missing"] = "Error de sintaxis: falta '{}'";
            m_messages["as-syntax-error-generic"] = "Error de sintaxis";
        }
    }

    std::string I18n::GetMessage(const std::string &key) const
    {
        auto it = m_messages.find(key);
        if (it != m_messages.end())
        {
            return it->second;
        }
        return "";
    }
}