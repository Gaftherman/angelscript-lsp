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
        m_messages["as-err-unresolved-type"] = "Unknown type '{}'.";
        m_messages["as-err-handle-on-primitive"] = "Cannot use handle '@' on primitive type '{}'.";
        m_messages["as-err-void-variable"] = "Cannot declare a variable of type 'void'.";
        m_messages["as-err-multi-class-inherit"] = "Class '{}' cannot inherit from multiple classes.";
        m_messages["as-err-base-not-found"] = "Base type '{}' not found.";
        m_messages["as-err-typedef-unresolved"] = "Typedef base type '{}' is not defined.";
        m_messages["as-err-funcdef-not-handle"] = "Variables or parameters of funcdef type '{}' must be declared as handles ('{}@').";
        m_messages["as-err-duplicate-param"] = "Duplicate parameter name '{}' in function '{}'.";
        m_messages["as-warn-shadow-global"] = "Parameter '{}' shadows a global variable of the same name.";
        m_messages["as-err-circular-inherit"] = "Circular inheritance detected: '{}' inherits from itself.";

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
            m_messages["as-err-unresolved-type"] = "Tipo desconocido '{}'.";
            m_messages["as-err-handle-on-primitive"] = "No se puede usar handle '@' en el tipo primitivo '{}'.";
            m_messages["as-err-void-variable"] = "No se puede declarar una variable de tipo 'void'.";
            m_messages["as-err-multi-class-inherit"] = "La clase '{}' no puede heredar de múltiples clases.";
            m_messages["as-err-base-not-found"] = "Tipo base '{}' no encontrado.";
            m_messages["as-err-typedef-unresolved"] = "El tipo base '{}' del typedef no está definido.";
            m_messages["as-err-funcdef-not-handle"] = "Variables o parámetros de tipo funcdef '{}' deben declararse como handle ('{}@').";
            m_messages["as-err-duplicate-param"] = "Nombre de parámetro '{}' duplicado en la función '{}'.";
            m_messages["as-warn-shadow-global"] = "El parámetro '{}' oculta una variable global con el mismo nombre.";
            m_messages["as-err-circular-inherit"] = "Herencia circular detectada: '{}' hereda de sí misma.";
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