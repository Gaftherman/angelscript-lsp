#include "i18n.h"

#include <cctype>

namespace angel_lsp::i18n
{
    namespace
    {
        /**
         * @brief Reduces a locale tag to its lowercased primary language subtag.
         *
         * The tag arrives from two places and neither is under this server's control: the client's
         * `initialize` params, where BCP 47 is the rule and an editor may send `es-ES`, `es-419` or
         * `es_MX`, and `--locale`, which this project's own README documents as `es-ES`. Comparing
         * the whole tag meant every one of those fell through to English, so a Spanish user got
         * Spanish only by asking for exactly "es".
         */
        std::string PrimaryLanguageSubtag(const std::string &locale)
        {
            std::string language;
            for (const char c : locale)
            {
                if (c == '-' || c == '_')
                {
                    break;
                }
                language.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            }
            return language;
        }
    }

    // ---------------------------------------------------------------------------------
    // Six of the codes below are deliberately never emitted. They are kept because each names a
    // real AngelScript rule, and deleted ones are not kept at all - a message describing a rule the
    // language does not have is worse than no message, since someone eventually implements it.
    //
    // Note what is no longer on this list. Several of these were once filed as undecidable because
    // the answer lives in the host's SetEngineProperty calls rather than in the script; the host
    // can now say so through config::EngineProperties, and a rule that reads one is an ordinary
    // rule again. When a reason below names an engine option, that is a rule waiting for a
    // property to be plumbed through - not one that cannot exist.
    //
    // Every reason here was checked against a real engine build rather than reasoned about, and the
    // full argument sits at each rule's site:
    //
    //   as-err-base-not-found       A base that resolves to nothing is what an engine-registered
    //   as-err-unresolved-type      type looks like. The corpus is almost entirely those, so the
    //                               rule would report the engine. Decidable only once a workspace's
    //                               stubs are known complete, which one document cannot establish.
    //                               See ClassRules.cpp and FunctionRules.cpp.
    //
    //   as-err-external-not-found   'external shared' resolves against another module. Modules are
    //                               not a concept this analyzer has. See ClassRules.cpp.
    //
    //   as-err-invalid-reference-return   Not an engine option after all, which is what this note
    //                                     used to say. At the engine's defaults `int& GetRef() {
    //                                     return g_value; }` builds clean and only `return local;`
    //                                     answers "Not a valid reference" - so the declaration is
    //                                     never what is wrong, and judging the returned expression
    //                                     belongs with the use-site rules. See FunctionRules.cpp.
    //
    //   as-err-standalone-reference       `int &r = x;` is legal only under
    //                                     asEP_ALLOW_UNSAFE_REFERENCES, which
    //                                     EngineProperties::allowUnsafeReferences now supplies -
    //                                     but the grammar refuses the declaration outright, the
    //                                     way the engine's own parser does at its defaults. Parsing
    //                                     it is what this rule waits on, not knowledge. The
    //                                     parameter half of the same question is live: a bare `&`
    //                                     on a primitive is reported through
    //                                     as-err-inout-on-primitive, which is the sentence the
    //                                     engine itself answers with. See FunctionRules.cpp.
    //
    //   as-warn-shadow-global       Shadowing a global is legal and ordinary; the warning fired on
    //                               plain parameter naming throughout the corpus rather than on
    //                               anything a user would change. See FunctionRules.cpp.
    //
    // Deleted rather than left here: as-err-mixin-as-base (described the opposite of how a mixin is
    // included), as-err-opindex-arity (byte-identical to as-err-opindex-no-params),
    // as-err-implicit-conversion (superseded by as-err-no-implicit-conversion, which names both
    // types), and as-err-typedef-unresolved (a second typedef failure mode AngelScript does not
    // have - the type never gets far enough to be looked up).
    // ---------------------------------------------------------------------------------
    I18n::I18n(const std::string &localeTag)
        : m_locale(PrimaryLanguageSubtag(localeTag))
    {
        const std::string &locale = m_locale;

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
        m_messages["as-err-declaration-missing-body"] = "'{}' must have a body '{{}}'. Only 'external shared' declares one without.";
        m_messages["as-err-external-not-shared"] = "'external' requires 'shared' on '{}'.";
        m_messages["as-err-property-duplicate-accessor"] = "Virtual property '{}' declares '{}' more than once.";
        m_messages["as-err-unresolved-type"] = "Unknown type '{}'.";
        m_messages["as-err-handle-on-primitive"] = "Cannot use handle '@' on primitive type '{}'.";
        m_messages["as-err-void-variable"] = "Cannot declare a variable of type 'void'.";
        m_messages["as-err-multi-class-inherit"] = "Class '{}' cannot inherit from multiple classes.";
        m_messages["as-err-base-not-found"] = "Base type '{}' not found.";
        m_messages["as-err-funcdef-not-handle"] = "Variables or parameters of funcdef type '{}' must be declared as handles ('{}@').";
        m_messages["as-err-duplicate-param"] = "Duplicate parameter name '{}' in function '{}'.";
        m_messages["as-warn-shadow-global"] = "Parameter '{}' shadows a global variable of the same name.";
        m_messages["as-warn-include-not-found"] = "Included file '{}' was not found.";
        m_messages["as-err-circular-inherit"] = "Circular inheritance detected: '{}' inherits from itself.";
        m_messages["as-err-const-out-param"] = "Parameter '{}' cannot be both 'const' and '&out'.";
        m_messages["as-err-interface-impl-missing"] = "Class '{}' does not implement interface method '{}' from interface '{}'.";
        m_messages["as-err-attribute-repeated"] = "Attribute '{}' is informed multiple times.";
        m_messages["as-err-reserved-keyword-name"] = "Instead found reserved keyword '{}'.";
        m_messages["as-err-name-conflict"] = "Name conflict. '{}' is already declared as a {}.";
        m_messages["as-err-const-void-return"] = "Return type can't be 'const void'.";
        m_messages["as-err-global-function-qualifiers"] = "Global function '{}' cannot be declared 'const'.";
        m_messages["as-err-override-no-base"] = "Method '{}' marked as override but class '{}' does not replace any base class or interface method.";
        m_messages["as-err-default-param-order"] = "All subsequent parameters after the first default value must also have default values in function '{}'.";
        m_messages["as-err-inout-on-primitive"] = "Only object types that support object references can use &inout ('{}').";
        m_messages["as-err-global-variable-access-modifier"] = "Global or namespace variable '{}' cannot have access modifiers (private/protected).";
        m_messages["as-err-global-vars-disallowed"] = "Global variable '{}' is not allowed: global variables have been disabled by the application.";
        m_messages["as-err-void-reference"] = "Type 'void' cannot be a reference.";
        m_messages["as-err-property-accessor-missing-body"] = "Property accessor '{}' must be implemented.";
        m_messages["as-err-destructor-param"] = "The destructor '{}' must not have any parameters.";
        m_messages["as-err-destructor-return-type"] = "The destructor '{}' must not have a return type.";
        m_messages["as-err-destructor-delete"] = "Cannot flag destructor '{}' with delete.";
        m_messages["as-err-class-member-const"] = "Class member '{}' cannot be declared as const.";
        m_messages["as-err-delete-with-body"] = "Deleted function '{}' cannot have a body.";
        m_messages["as-err-void-parameter"] = "Parameter '{}' in function '{}' cannot be of type 'void'.";
        m_messages["as-err-binary-operator-arity"] = "Binary operator overload '{}' must take exactly 1 parameter.";
        m_messages["as-err-opindex-no-params"] = "Index operator 'opIndex' must take at least 1 parameter.";
        m_messages["as-err-opequals-return-bool"] = "Equality operator 'opEquals' must return 'bool'.";
        m_messages["as-err-opcmp-return-int"] = "Comparison operator 'opCmp' must return 'int'.";
        m_messages["as-err-override-final-method"] = "Cannot override method '{}' declared as final in base class '{}'.";
        m_messages["as-err-enum-invalid-initializer"] = "Enum member initializer '{}' must be a constant integer expression.";
        m_messages["as-err-standalone-reference"] = "Standalone reference variable '{}' is not supported.";
        m_messages["as-err-delete-with-other-qualifier"] = "Cannot flag function '{}' that will be deleted with other qualifiers.";
        m_messages["as-err-delete-not-auto-generated"] = "Cannot flag function '{}' that will not be auto generated as deleted. Only a default constructor, a copy constructor or 'opAssign' may be deleted.";
        m_messages["as-err-explicit-not-member"] = "'explicit' is only allowed on a class method, and '{}' is not one.";
        m_messages["as-err-interface-method-attribute"] = "An interface method cannot carry the '{}' attribute ('{}').";
        m_messages["as-err-funcdef-attribute"] = "A funcdef cannot carry the '{}' attribute ('{}').";
        m_messages["as-warn-global-function-attribute"] = "'{}' describes a method's relationship to a class, so it means nothing on global function '{}'. AngelScript accepts it and ignores it.";
        m_messages["as-err-private-member-access"] = "Illegal access to private member '{}', declared in class '{}'.";
        m_messages["as-err-protected-member-access"] = "Illegal access to protected member '{}', declared in class '{}'. A protected member is reachable from a derived class, and only through an object of that class's own type.";
        m_messages["as-err-virtual-property-signature"] = "Invalid signature for virtual property '{}'. A 'get_' accessor returns a value and takes at most an index; a 'set_' accessor returns void and takes the value, optionally preceded by an index.";
        m_messages["as-err-array-invalid-template"] = "Attempting to instantiate invalid template parameter '{}'.";
        m_messages["as-err-typedef-non-primitive"] = "Typedef base type '{}' must be a primitive data type.";
        m_messages["as-err-invalid-reference-return"] = "Not a valid reference type '{}'.";
        m_messages["as-err-external-not-found"] = "External shared entity '{}' not found.";
        m_messages["as-err-mixin-child-type"] = "Mixin class '{}' cannot have child type declarations.";
        m_messages["as-err-mixin-virtual-property"] = "The virtual property syntax is currently not supported for mixin classes";
        m_messages["as-err-double-reference"] = "Invalid parameter reference qualifier format for type '{}'.";
        m_messages["as-err-interface-constructor"] = "Interface '{}' cannot declare constructors or destructors.";
        m_messages["as-err-mixin-constructor"] = "Mixin class '{}' cannot declare a constructor.";
        m_messages["as-err-mixin-destructor"] = "Mixin class '{}' cannot declare a destructor.";
        m_messages["as-err-op-overload-global"] = "Operator overload '{}' must be a class member method.";
        m_messages["as-err-break-outside-loop"] = "'break' statement can only be used within loop or switch.";
        m_messages["as-err-continue-outside-loop"] = "'continue' statement can only be used within loop.";
        m_messages["as-err-invalid-case-type"] = "Case value must be an integer, char, or enum constant expression.";
        m_messages["as-err-duplicate-case-value"] = "Duplicate case value '{}' in switch statement.";
        m_messages["as-err-default-must-be-last"] = "The default case must be the last one.";
        m_messages["as-err-not-all-paths-return"] = "Not all paths of '{}' return a value.";
        m_messages["as-err-abstract-instantiated"] = "Abstract class '{}' cannot be instantiated. Declare a handle ('{}@') instead.";
        m_messages["as-err-interface-instantiated"] = "Interface '{}' cannot be instantiated. Declare a handle ('{}@') instead.";
        m_messages["as-err-parameter-not-instantiable"] = "Parameter type can't be '{}', because the type cannot be instantiated.";
        m_messages["as-err-return-not-instantiable"] = "Return type can't be '{}', because the type cannot be instantiated.";
        m_messages["as-err-mixin-not-a-type"] = "Mixin '{}' cannot be used as a data type.";
        m_messages["as-err-undeclared-identifier"] = "Undeclared identifier '{}'.";
        m_messages["as-warn-unused-variable"] = "Local variable '{}' is never used.";
        m_messages["as-err-null-non-handle"] = "Cannot assign 'null' to non-handle type '{}'.";
        m_messages["as-err-no-implicit-conversion"] = "Cannot implicitly convert '{}' to '{}'. Declare a matching constructor, an 'opImplConv' or an 'opAssign' overload.";
        m_messages["as-err-no-explicit-conversion"] = "No conversion from '{}' to '{}' exists. Declare a matching constructor on the target, or an 'opConv'/'opImplConv' overload on the source.";
        m_messages["as-err-invalid-cast"] = "Cannot cast '{}' to '{}'. The types are unrelated and no 'opCast'/'opImplCast' overload is declared.";

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
            m_messages["as-err-declaration-missing-body"] = "'{}' debe tener un cuerpo '{{}}'. Solo 'external shared' se declara sin él.";
            m_messages["as-err-external-not-shared"] = "'external' requiere 'shared' en '{}'.";
            m_messages["as-err-property-duplicate-accessor"] = "La propiedad virtual '{}' declara '{}' más de una vez.";
            m_messages["as-err-unresolved-type"] = "Tipo desconocido '{}'.";
            m_messages["as-err-handle-on-primitive"] = "No se puede usar handle '@' en el tipo primitivo '{}'.";
            m_messages["as-err-void-variable"] = "No se puede declarar una variable de tipo 'void'.";
            m_messages["as-err-multi-class-inherit"] = "La clase '{}' no puede heredar de múltiples clases.";
            m_messages["as-err-base-not-found"] = "Tipo base '{}' no encontrado.";
            m_messages["as-err-funcdef-not-handle"] = "Variables o parámetros de tipo funcdef '{}' deben declararse como handle ('{}@').";
            m_messages["as-err-duplicate-param"] = "Nombre de parámetro '{}' duplicado en la función '{}'.";
            m_messages["as-warn-shadow-global"] = "El parámetro '{}' oculta una variable global con el mismo nombre.";
            m_messages["as-warn-include-not-found"] = "No se encontró el archivo incluido '{}'.";
            m_messages["as-err-circular-inherit"] = "Herencia circular detectada: '{}' hereda de sí misma.";
            m_messages["as-err-const-out-param"] = "El parámetro '{}' no puede ser 'const' y '&out' al mismo tiempo.";
            m_messages["as-err-interface-impl-missing"] = "La clase '{}' no implementa el método de interfaz '{}' de la interfaz '{}'.";
            m_messages["as-err-attribute-repeated"] = "El atributo '{}' se informa múltiples veces.";
            m_messages["as-err-reserved-keyword-name"] = "Se encontró la palabra reservada '{}' en lugar de un identificador.";
            m_messages["as-err-name-conflict"] = "Conflicto de nombre. '{}' ya está declarado como {}.";
            m_messages["as-err-const-void-return"] = "El tipo de retorno no puede ser 'const void'.";
            m_messages["as-err-global-function-qualifiers"] = "La función global '{}' no puede declararse 'const'.";
            m_messages["as-err-override-no-base"] = "El método '{}' marcado como override no reemplaza ningún método de clase base o interfaz en la clase '{}'.";
            m_messages["as-err-default-param-order"] = "Todos los parámetros subsiguientes después del primer valor por defecto deben tener valores por defecto en la función '{}'.";
            m_messages["as-err-inout-on-primitive"] = "Solo los tipos de objeto que admiten referencias pueden usar &inout ('{}').";
            m_messages["as-err-global-variable-access-modifier"] = "La variable global o de namespace '{}' no puede tener modificadores de acceso (private/protected).";
            m_messages["as-err-global-vars-disallowed"] = "La variable global '{}' no está permitida: la aplicación ha deshabilitado las variables globales.";
            m_messages["as-err-void-reference"] = "El tipo 'void' no puede ser una referencia.";
            m_messages["as-err-property-accessor-missing-body"] = "El accesor de propiedad '{}' debe tener una implementación.";
            m_messages["as-err-destructor-param"] = "El destructor '{}' no debe tener ningún parámetro.";
            m_messages["as-err-destructor-return-type"] = "El destructor '{}' no debe tener un tipo de retorno.";
            m_messages["as-err-destructor-delete"] = "No se puede marcar el destructor '{}' con '= delete'.";
            m_messages["as-err-class-member-const"] = "El miembro de clase '{}' no puede ser declarado como 'const'.";
            m_messages["as-err-delete-with-body"] = "La función eliminada con '= delete' ('{}') no puede tener un cuerpo.";
            m_messages["as-err-void-parameter"] = "El parámetro '{}' en la función '{}' no puede ser de tipo 'void'.";
            m_messages["as-err-binary-operator-arity"] = "La sobrecarga del operador binario '{}' debe tomar exactamente 1 parámetro.";
            m_messages["as-err-opindex-no-params"] = "El operador de índice 'opIndex' debe tomar al menos 1 parámetro.";
            m_messages["as-err-opequals-return-bool"] = "El operador de igualdad 'opEquals' debe retornar 'bool'.";
            m_messages["as-err-opcmp-return-int"] = "El operador de comparación 'opCmp' debe retornar 'int'.";
            m_messages["as-err-override-final-method"] = "No se puede sobrescribir el método '{}' declarado como 'final' en la clase base '{}'.";
            m_messages["as-err-enum-invalid-initializer"] = "El inicializador del miembro de enum '{}' debe ser una expresión entera constante.";
            m_messages["as-err-standalone-reference"] = "La variable de referencia independiente '{}' no está soportada.";
            m_messages["as-err-delete-with-other-qualifier"] = "No se puede marcar con modificadores adicionales la función '{}' que será eliminada con '= delete'.";
            m_messages["as-err-delete-not-auto-generated"] = "No se puede marcar como eliminada la función '{}' porque el motor no la autogenera. Solo pueden eliminarse el constructor por defecto, el constructor de copia u 'opAssign'.";
            m_messages["as-err-explicit-not-member"] = "'explicit' solo se permite en un método de clase, y '{}' no lo es.";
            m_messages["as-err-interface-method-attribute"] = "Un método de interfaz no puede llevar el atributo '{}' ('{}').";
            m_messages["as-err-funcdef-attribute"] = "Un funcdef no puede llevar el atributo '{}' ('{}').";
            m_messages["as-warn-global-function-attribute"] = "'{}' describe la relación de un método con su clase, así que no significa nada en la función global '{}'. AngelScript lo acepta y lo ignora.";
            m_messages["as-err-private-member-access"] = "Acceso ilegal al miembro privado '{}', declarado en la clase '{}'.";
            m_messages["as-err-protected-member-access"] = "Acceso ilegal al miembro protegido '{}', declarado en la clase '{}'. Un miembro protegido es accesible desde una clase derivada, y únicamente a través de un objeto del tipo de esa misma clase.";
            m_messages["as-err-virtual-property-signature"] = "Firma no válida para la propiedad virtual '{}'. Un accesor 'get_' devuelve un valor y toma como mucho un índice; un accesor 'set_' devuelve void y toma el valor, precedido opcionalmente por un índice.";
            m_messages["as-err-array-invalid-template"] = "Intento de instanciar un parámetro de plantilla no válido ('{}').";
            m_messages["as-err-typedef-non-primitive"] = "El tipo base del typedef ('{}') debe ser un tipo de dato primitivo.";
            m_messages["as-err-invalid-reference-return"] = "No es un tipo de retorno por referencia válido ('{}').";
            m_messages["as-err-external-not-found"] = "Entidad compartida externa ('{}') no encontrada.";
            m_messages["as-err-mixin-child-type"] = "La clase mixin ('{}') no puede contener declaraciones de tipos hijos.";
            m_messages["as-err-mixin-virtual-property"] = "La sintaxis de propiedad virtual actualmente no está soportada para clases mixin.";
            m_messages["as-err-double-reference"] = "Formato inválido de cualificador de referencia de parámetro para el tipo '{}'.";
            m_messages["as-err-interface-constructor"] = "La interfaz '{}' no puede declarar constructores ni destructores.";
            m_messages["as-err-mixin-constructor"] = "La clase mixin '{}' no puede declarar un constructor.";
            m_messages["as-err-mixin-destructor"] = "La clase mixin '{}' no puede declarar un destructor.";
            m_messages["as-err-op-overload-global"] = "La sobrecarga del operador '{}' debe ser un método miembro de clase.";
            m_messages["as-err-break-outside-loop"] = "La sentencia 'break' solo puede usarse dentro de un bucle o un 'switch'.";
            m_messages["as-err-continue-outside-loop"] = "La sentencia 'continue' solo puede usarse dentro de un bucle.";
            m_messages["as-err-invalid-case-type"] = "El valor de 'case' debe ser una expresión constante entera, de carácter o de enumeración.";
            m_messages["as-err-duplicate-case-value"] = "Valor de 'case' duplicado ('{}') en la sentencia 'switch'.";
            m_messages["as-err-default-must-be-last"] = "El caso por defecto (default) debe ser el último.";
            m_messages["as-err-not-all-paths-return"] = "No todos los caminos de '{}' devuelven un valor.";
            m_messages["as-err-abstract-instantiated"] = "No se puede instanciar la clase abstracta '{}'. Declara un handle ('{}@') en su lugar.";
            m_messages["as-err-interface-instantiated"] = "No se puede instanciar la interfaz '{}'. Declara un handle ('{}@') en su lugar.";
            m_messages["as-err-parameter-not-instantiable"] = "El tipo de parámetro no puede ser '{}', porque ese tipo no se puede instanciar.";
            m_messages["as-err-return-not-instantiable"] = "El tipo de retorno no puede ser '{}', porque ese tipo no se puede instanciar.";
            m_messages["as-err-mixin-not-a-type"] = "El mixin '{}' no puede ser usado como un tipo de dato.";
            m_messages["as-err-undeclared-identifier"] = "Identificador no declarado '{}'.";
            m_messages["as-warn-unused-variable"] = "La variable local '{}' nunca se usa.";
            m_messages["as-err-null-non-handle"] = "No se puede asignar 'null' al tipo no-handle '{}'.";
            m_messages["as-err-no-implicit-conversion"] = "No se puede convertir implícitamente '{}' a '{}'. Declara un constructor compatible, un 'opImplConv' o una sobrecarga 'opAssign'.";
            m_messages["as-err-no-explicit-conversion"] = "No existe conversión de '{}' a '{}'. Declara un constructor compatible en el destino, o una sobrecarga 'opConv'/'opImplConv' en el origen.";
            m_messages["as-err-invalid-cast"] = "No se puede convertir '{}' a '{}' con cast. Los tipos no están relacionados y no hay sobrecarga 'opCast'/'opImplCast'.";
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