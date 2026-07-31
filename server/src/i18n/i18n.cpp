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
        m_messages["as-err-const-out-param"] = "Parameter '{}' cannot be both 'const' and '&out'.";
        m_messages["as-err-mixin-as-base"] = "Class '{}' cannot inherit from mixin '{}'. Mixins can only be included using 'mixin class'.";
        m_messages["as-err-interface-impl-missing"] = "Class '{}' does not implement interface method '{}' from interface '{}'.";
        m_messages["as-err-attribute-repeated"] = "Attribute '{}' is informed multiple times.";
        m_messages["as-err-reserved-keyword-name"] = "Instead found reserved keyword '{}'.";
        m_messages["as-err-name-conflict"] = "Name conflict. '{}' is already declared as a {}.";
        m_messages["as-err-const-void-return"] = "Return type can't be 'const void'.";
        m_messages["as-err-global-function-qualifiers"] = "Global function '{}' cannot have member function qualifiers (const, override, final).";
        m_messages["as-err-override-no-base"] = "Method '{}' marked as override but class '{}' does not replace any base class or interface method.";
        m_messages["as-err-default-param-order"] = "All subsequent parameters after the first default value must also have default values in function '{}'.";
        m_messages["as-err-inout-on-primitive"] = "Only object types that support object references can use &inout ('{}').";
        m_messages["as-err-global-variable-access-modifier"] = "Global or namespace variable '{}' cannot have access modifiers (private/protected).";
        m_messages["as-err-void-reference"] = "Type 'void' cannot be a reference.";
        m_messages["as-err-property-accessor-missing-body"] = "Property accessor '{}' must be implemented.";
        m_messages["as-err-destructor-param"] = "The destructor '{}' must not have any parameters.";
        m_messages["as-err-destructor-delete"] = "Cannot flag destructor '{}' with delete.";
        m_messages["as-err-class-member-const"] = "Class member '{}' cannot be declared as const.";
        m_messages["as-err-delete-with-body"] = "Deleted function '{}' cannot have a body.";
        m_messages["as-err-void-parameter"] = "Parameter '{}' in function '{}' cannot be of type 'void'.";
        m_messages["as-err-binary-operator-arity"] = "Binary operator overload '{}' must take exactly 1 parameter.";
        m_messages["as-err-opindex-no-params"] = "Index operator 'opIndex' must take at least 1 parameter.";
        m_messages["as-err-opequals-return-bool"] = "Equality operator 'opEquals' must return 'bool'.";
        m_messages["as-err-opcmp-return-int"] = "Comparison operator 'opCmp' must return 'int'.";
        m_messages["as-err-override-final-method"] = "Cannot override method '{}' declared as final in base class '{}'.";

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
            m_messages["as-err-const-out-param"] = "El parámetro '{}' no puede ser 'const' y '&out' al mismo tiempo.";
            m_messages["as-err-mixin-as-base"] = "La clase '{}' no puede heredar del mixin '{}'. Los mixins solo pueden incluirse con 'mixin class'.";
            m_messages["as-err-interface-impl-missing"] = "La clase '{}' no implementa el método de interfaz '{}' de la interfaz '{}'.";
            m_messages["as-err-attribute-repeated"] = "El atributo '{}' se informa múltiples veces.";
            m_messages["as-err-reserved-keyword-name"] = "Se encontró la palabra reservada '{}' en lugar de un identificador.";
            m_messages["as-err-name-conflict"] = "Conflicto de nombre. '{}' ya está declarado como {}.";
            m_messages["as-err-const-void-return"] = "El tipo de retorno no puede ser 'const void'.";
            m_messages["as-err-global-function-qualifiers"] = "La función global '{}' no puede tener calificadores de función miembro (const, override, final).";
            m_messages["as-err-override-no-base"] = "El método '{}' marcado como override no reemplaza ningún método de clase base o interfaz en la clase '{}'.";
            m_messages["as-err-default-param-order"] = "Todos los parámetros subsiguientes después del primer valor por defecto deben tener valores por defecto en la función '{}'.";
            m_messages["as-err-inout-on-primitive"] = "Solo los tipos de objeto que admiten referencias pueden usar &inout ('{}').";
            m_messages["as-err-global-variable-access-modifier"] = "La variable global o de namespace '{}' no puede tener modificadores de acceso (private/protected).";
            m_messages["as-err-void-reference"] = "El tipo 'void' no puede ser una referencia.";
            m_messages["as-err-property-accessor-missing-body"] = "El accesor de propiedad '{}' debe tener una implementación.";
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