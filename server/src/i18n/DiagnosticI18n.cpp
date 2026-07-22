/**
 * @file DiagnosticI18n.cpp
 * @brief Implementation of diagnostic message translation tables.
 * @ingroup i18n
 */

#include "i18n/DiagnosticI18n.h"
#include <ankerl/unordered_dense.h>
#include <regex>

namespace angel_lsp::i18n
{

    std::string DiagnosticI18n::Translate(const std::string &originalMsg, Locale locale)
    {
        if (locale == Locale::EN || locale == Locale::UNKNOWN)
        {
            return originalMsg;
        }

        if (locale == Locale::ES)
        {
            // 1. Static exact matches (O(1) lookup using unordered_dense)
            static const ankerl::unordered_dense::map<std::string, std::string> staticMap =
                {
                    {"Invalid #include directive: missing opening quote or angle bracket", "Directiva #include no válida: falta comilla de apertura o corchete angular"},
                    {"Invalid #include directive: unexpected characters after path", "Directiva #include no válida: caracteres no esperados después de la ruta"},
                    {"Invalid #include directive: empty file path", "Directiva #include no válida: ruta de archivo vacía"},
                    {"Initialization after return. All code paths must initialize the members", "Inicialización después del regreso. Todas las rutas de código deben inicializar los miembros."},
                    {"Output argument expression is not assignable", "La expresión del argumento de salida no es asignable"},
                    {"Auto is not allowed here", "Auto no está permitido aquí"},
                    {"Can't find unambiguous implicit conversion to make both expressions have the same type", "No se puede encontrar una conversión implícita inequívoca para que ambas expresiones tengan el mismo tipo"},
                    {"Both conditions must call constructor", "Ambas condiciones deben llamar al constructor."},
                    {"Base class doesn't have default constructor. Make explicit call to base constructor", "La clase base no tiene un constructor predeterminado. Hacer una llamada explícita al constructor base."},
                    {"Base class doesn't have copy constructor or default constructor and assign operator. Make explicit call to base constructor", "La clase base no tiene constructor de copia ni constructor predeterminado ni operador de asignación. Hacer una llamada explícita al constructor base."},
                    {"Candidates are:", "Las firmas candidatas son:"},
                    {"Can't call a constructor in loops", "No se puede llamar a un constructor en bucles"},
                    {"Can't call a constructor in switch", "No se puede llamar a un constructor en switch"},
                    {"Can't call a constructor multiple times", "No se puede llamar a un constructor varias veces"},
                    {"Can't create delegate", "No se puede crear delegado"},
                    {"Can't create delegate for types that do not support handles", "No se pueden crear delegados para tipos que no admiten identificadores"},
                    {"Cannot flag function that will not be auto generated as deleted", "No se puede marcar una función que no se generará automáticamente como eliminada"},
                    {"Conflict with explicit declaration of function and deleted function", "Conflicto con declaración explícita de función y función eliminada"},
                    {"Can't implement itself, or another interface that implements this interface", "No se puede implementar a sí mismo ni a otra interfaz que implemente esta interfaz"},
                    {"Can't implicitly call explicit copy constructor", "No se puede llamar implícitamente al constructor de copia explícito"},
                    {"Can't inherit from multiple classes", "No se puede heredar de varias clases"},
                    {"Can't inherit from itself, or another class that inherits from this class", "No se puede heredar de sí mismo ni de otra clase que herede de esta clase"},
                    {"Can't initialize the members in loops", "No se pueden inicializar los miembros en bucles"},
                    {"Can't initialize the members in switch", "No se pueden inicializar los miembros en switch"},
                    {"Can't pass class method as arg directly. Use a delegate object instead", "No se puede pasar el método de clase como arg directamente. Utilice un objeto delegado en su lugar"},
                    {"Unable to resolve auto type", "No se puede resolver el tipo 'auto'"},
                    {"Can't return reference to local value.", "No se puede devolver la referencia al valor local."},
                    {"Can't return value when return type is 'void'", "No se puede devolver valor cuando el tipo de devolución es 'void'"},
                    {"Implicit conversion changed sign of value", "La conversión implícita cambió el signo de valor"},
                    {"A class cannot be both abstract and final", "Una clase no puede ser a la vez abstracta y final."},
                    {"Compound assignments with property accessors on value types are not supported", "No se admiten asignaciones compuestas con descriptores de acceso a propiedades en tipos de valores"},
                    {"Compound assignments with indexed property accessors are not supported", "No se admiten asignaciones compuestas con descriptores de acceso a propiedades indexadas"},
                    {"Compound assignments with property accessors require both get and set accessors", "Las asignaciones compuestas con descriptores de acceso a propiedades requieren descriptores de acceso get y set"},
                    {"Variables cannot be declared in switch cases, except inside statement blocks", "Las variables no se pueden declarar en casos de 'switch', excepto dentro de bloques de sentencias"},
                    {"The default case must be the last one", "El caso 'default' debe ser el último."},
                    {"The type of the default argument expression doesn't match the function parameter type", "El tipo de expresión de argumento predeterminado no coincide con el tipo de parámetro de función"},
                    {"Deleted functions cannot have implementation", "Las funciones eliminadas no pueden tener implementación."},
                    {"The destructor must not have any parameters", "El destructor no debe tener ningún parámetro."},
                    {"Value assignment on reference types is not allowed. Did you mean to do a handle assignment?", "No se permite la asignación por valor a tipos de referencia. ¿Querías hacer una asignación de handle (identificador)?"},
                    {"Compound assignment on reference types is not allowed", "No se permite la asignación compuesta en tipos de referencia"},
                    {"Duplicate switch case", "Caso de 'switch' duplicado"},
                    {"Else with empty statement", "Sentencia 'else' vacía"},
                    {"Empty list element is not allowed", "No se permite el elemento de lista vacío"},
                    {"Empty switch statement", "Sentencia 'switch' vacía"},
                    {"Expected constant", "Se esperaba una constante"},
                    {"Expected data type", "Se esperaba un tipo de dato"},
                    {"Expected expression value", "Se esperaba un valor de expresión"},
                    {"Expected identifier", "Se esperaba un identificador"},
                    {"Expected a list enclosed by { } to match pattern", "Se esperaba una lista encerrada por {} para coincidir con el patrón"},
                    {"Expected method or property", "Método o propiedad esperado"},
                    {"Expected one of: ", "Se esperaba uno de:"},
                    {"Expected operator", "Se esperaba un operador"},
                    {"Expected post operator", "Se esperaba un operador postfijo"},
                    {"Expected pre operator", "Se esperaba un operador prefijo"},
                    {"Expected string", "Se esperaba una cadena"},
                    {"Expression doesn't evaluate to a function", "La expresión no se evalúa como una función."},
                    {"Previous error occurred while attempting to create a temporary copy of object", "Se produjo un error anterior al intentar crear una copia temporal del objeto."},
                    {"Float value truncated in implicit conversion to integer", "Valor flotante truncado en conversión implícita a entero"},
                    {"Found multiple matching enum values", "Se encontraron múltiples valores de enumeración coincidentes."},
                    {"A function with the same name and parameters already exists", "Ya existe una función con el mismo nombre y parámetros"},
                    {"Global variables have been disabled by the application", "Las variables globales han sido deshabilitadas por la aplicación."},
                    {"It is not allowed to perform a handle assignment on a non-handle property", "No está permitido realizar una asignación de 'handle' en una propiedad que no sea 'handle'."},
                    {"The operand is implicitly converted to handle in order to compare them", "El operando se convierte implícitamente a 'handle' para poder compararlos."},
                    {"Handle to handle is not allowed", "No se permite asignación de 'handle' a 'handle'"},
                    {"If with empty statement", "Sentencia 'if' vacía"},
                    {"Illegal member type", "Tipo de miembro ilegal"},
                    {"Illegal operation on this datatype", "Operación ilegal en este tipo de datos"},
                    {"Illegal target type for reference cast", "Tipo de destino ilegal para 'reference cast'"},
                    {"Interfaces can only implement other interfaces", "Las interfaces solo pueden implementar otras interfaces."},
                    {"Invalid 'break'", "Sentencia 'break' no válida"},
                    {"Invalid character literal", "Literal de carácter no válido"},
                    {"Invalid 'continue'", "Sentencia 'continue' no válida"},
                    {"Invalid escape sequence", "Secuencia de escape no válida"},
                    {"Invalid expression: ambiguous name", "Expresión no válida: nombre ambiguo"},
                    {"Invalid expression: stand-alone anonymous function", "Expresión no válida: función anónima independiente"},
                    {"Invalid operation on method", "Operación no válida en el método"},
                    {"Invalid reference. Property accessors cannot be used in combined read/write operations", "Referencia no válida. Los descriptores de acceso a propiedades no se pueden utilizar en operaciones combinadas de lectura/escritura."},
                    {"Invalid scope resolution", "Resolución de ámbito no válida"},
                    {"Invalid signature for virtual property", "Firma no válida para propiedad virtual"},
                    {"Invalid type", "tipo no válido"},
                    {"Invalid unicode code point", "Punto de código Unicode no válido"},
                    {"Invalid unicode sequence in source", "Secuencia Unicode no válida en la fuente"},
                    {"Invalid use of named arguments", "Uso no válido de argumentos con nombre"},
                    {"The method cannot be named with the class name", "El método no puede ser nombrado con el nombre de la clase."},
                    {"Mixin classes cannot have constructors or destructors", "Las clases Mixin no pueden tener constructores ni destructores."},
                    {"Mixin class cannot inherit from classes", "La clase Mixin no puede heredar de las clases"},
                    {"Mixin classes cannot have child types", "Las clases Mixin no pueden tener tipos secundarios."},
                    {"Found more than one matching operator", "Se encontró más de un operador coincidente"},
                    {"Multiline strings are not allowed in this application", "No se permiten cadenas multilínea en esta aplicación."},
                    {"Only objects have constructors", "Sólo los objetos tienen constructores."},
                    {"Must return a value", "Debe devolver un valor"},
                    {"Detected named argument with old syntax", "Argumento con nombre detectado con sintaxis antigua"},
                    {"No appropriate indexing operator found", "No se encontró ningún operador de indexación apropiado"},
                    {"No appropriate opEquals method found", "No se encontró ningún método opEquals apropiado"},
                    {"The application doesn't support the default array type.", "La aplicación no admite el tipo de 'array' predeterminado."},
                    {"Non-const method call on read-only object reference", "Llamada a método no constante en referencia de objeto de solo lectura"},
                    {"Non-terminated string literal", "Literal de cadena no terminada"},
                    {"Not all paths return a value", "No todas las rutas devuelven un valor"},
                    {"Rejected due to not enough parameters", "Rechazado por falta de parámetros"},
                    {"Not enough values to match pattern", "No hay suficientes valores para coincidir con el patrón"},
                    {"Implicit conversion of value is not exact", "La conversión implícita de valor no es exacta"},
                    {"Expression is not an l-value", "La expresión no es un 'l-value'"},
                    {"Not a valid reference", "No es una referencia válida"},
                    {"Not a valid lvalue", "No es un 'lvalue' válido"},
                    {"Nothing was built in the module", "No se construyó nada en el módulo."},
                    {"Object handle is not supported for this type", "No se soportan 'object handles' para este tipo"},
                    {"Only object types that support object handles can use &inout. Use &in or &out instead", "Sólo los tipos de objetos que admiten 'object handles' pueden utilizar '&inout'. Utilice '&in' o '&out' en su lugar"},
                    {"A cast operator has one argument", "Un operador de conversión tiene un argumento."},
                    {"The code must contain one and only one function", "El código debe contener una y sólo una función."},
                    {"The code must contain one and only one global variable", "El código debe contener una y sólo una variable global."},
                    {"Both operands must be handles when comparing identity", "Ambos operandos deben ser identificadores al comparar la identidad."},
                    {"The overloaded functions are identical on initial parameters without default arguments", "Las funciones sobrecargadas son idénticas en los parámetros iniciales sin argumentos predeterminados."},
                    {"Parameter already declared", "Parámetro ya declarado"},
                    {"Positional arguments cannot be passed after named arguments", "Los argumentos posicionales no se pueden pasar después de argumentos con nombre"},
                    {"Potentially matching non-const method is hidden on read-only object reference", "El método no constante potencialmente coincidente está oculto en la referencia de objeto de solo lectura"},
                    {"Property accessor with index must have 1 and only 1 index argument", "El descriptor de acceso a la propiedad con índice debe tener 1 y solo 1 argumento de índice."},
                    {"Property accessors have been disabled by the application", "La aplicación ha inhabilitado los accesores a la propiedad."},
                    {"Property accessor must be implemented", "Se debe implementar el descriptor de acceso a la propiedad."},
                    {"Class properties cannot be declared as const", "Las propiedades de clase no se pueden declarar como constantes"},
                    {"The property has no get accessor", "La propiedad no tiene acceso get."},
                    {"The property has no set accessor", "La propiedad no tiene un descriptor de acceso establecido."},
                    {"Virtual property must have at least one get or set accessor", "La propiedad virtual debe tener al menos un descriptor de acceso get o set"},
                    {"Resulting reference cannot be returned. Returned references must not refer to local variables.", "La referencia resultante no se puede devolver. Las referencias devueltas no deben hacer referencia a variables locales."},
                    {"Resulting reference cannot be returned. There are deferred arguments that may invalidate it.", "La referencia resultante no se puede devolver. Hay argumentos diferidos que pueden invalidarlo."},
                    {"Resulting reference cannot be returned. The expression uses objects that during cleanup may invalidate it.", "La referencia resultante no se puede devolver. La expresión utiliza objetos que durante la limpieza pueden invalidarla."},
                    {"Reference is read-only", "La referencia es de solo lectura."},
                    {"Reference is temporary", "La referencia es temporal."},
                    {"Reference types cannot be passed by value in function parameters", "Los tipos de referencia no se pueden pasar por valor en los parámetros de función"},
                    {"Reference types cannot be returned by value from functions", "Los tipos de referencia no se pueden devolver por valor de funciones"},
                    {"The script section is empty", "La sección de 'script' está vacía."},
                    {"Signed/Unsigned mismatch", "No coincidencia de tipos Signed/Unsigned"},
                    {"Strings are not recognized by the application", "La aplicación no reconoce las cadenas"},
                    {"Case expressions must be literal constants", "Las expresiones de casos deben ser constantes literales."},
                    {"Switch expressions must be integral numbers", "Las expresiones de 'switch' deben ser números enteros"},
                    {"Rejected due to too many parameters", "Rechazado debido a demasiados parámetros"},
                    {"The function has too many jump labels to handle. Split the function into smaller ones.", "La función tiene demasiadas etiquetas de salto para manejar. Divide la función en otras más pequeñas."},
                    {"Too many values to match pattern", "Demasiados valores para coincidir con el patrón"},
                    {"Unexpected end of file", "Fin de archivo inesperado"},
                    {"Unexpected variable declaration", "Declaración de variable inesperada"},
                    {"Unreachable code", "Código inalcanzable"},
                    {"Virtual property contains unrecognized aspect", "La propiedad virtual contiene un aspecto no reconocido"},
                    {"Unused script node", "Nodo de script no utilizado"},
                    {"Value is too large for data type", "El valor es demasiado grande para el tipo de datos."},
                    {"Void cannot be an operand in expressions", "void no puede ser un operando en expresiones"},
                    {"Warnings are treated as errors by the application", "La aplicación trata las advertencias como errores."},
                    {"While parsing argument list", "Al analizar la lista de argumentos"},
                    {"While parsing expression", "Al analizar la expresión"},
                    {"While parsing initialization list", "Al analizar la lista de inicialización"},
                    {"While parsing namespace", "Al analizar el espacio de nombres"},
                    {"While parsing statement block", "Al analizar el bloque de sentencias"},
                    {"Previous error occurred while including mixin", "Se produjo un error anterior al incluir mixin"},
                    {"Autohandles cannot be used with types that have been registered with NOCOUNT", "Los identificadores automáticos no se pueden utilizar con tipos que se hayan registrado con NOCOUNT"},
                    {"First parameter to template factory must be a reference to primitive type. This will be used to pass the object type of the template", "El primer parámetro de la fábrica de plantillas debe ser una referencia al tipo primitivo. Esto se utilizará para pasar el tipo de objeto de la plantilla."},
                    {"Invalid configuration. Verify the registered application interface.", "Configuración no válida. Verifique la interfaz de la aplicación registrada."},
                    {"A value type must be registered with a non-zero size", "Se debe registrar un tipo de valor con un tamaño distinto de cero"},
                    {"The behaviour is not compatible with the type", "El comportamiento no es compatible con el tipo."},
                    {"A garbage collected ref type must have the addref, release, and all gc behaviours", "Un tipo por referencia con 'garbage collector' debe tener los comportamientos 'addref', 'release' y todos los 'gc'."},
                    {"A garbage collected value type must have the gc enum references behaviour", "Un tipo por valor con 'garbage collector' debe tener el comportamiento 'gc enum references'"},
                    {"A scoped reference type must have the release behaviour", "Un tipo de referencia 'scoped' debe tener el comportamiento 'release'"},
                    {"A reference type must have the addref and release behaviours", "Un tipo de referencia debe tener los comportamientos 'addref' y 'release'."},
                    {"A non-pod value type must have at least one constructor and the destructor behaviours", "Un tipo de valor que no sea 'POD' debe tener al menos un constructor y los comportamientos del destructor."},
                    {"Template list factory expects two reference parameters. The last is the pointer to the initialization buffer", "La fábrica de listas de plantillas espera dos parámetros de referencia. El último es el puntero al búfer de inicialización."},
                    {"List factory expects only one reference parameter. The pointer to the initialization buffer will be passed in this parameter", "La fábrica de listas espera solo un parámetro de referencia. El puntero al búfer de inicialización se pasará en este parámetro."},
                    {"AddScriptObjectToGC called with null pointer", "AddScriptObjectToGC llamado con puntero nulo"},
                    {"An exception occurred in a nested call", "Se produjo una excepción en una llamada anidada"},
                    {"Uh oh! The engine's reference count is increasing while it is being destroyed. Make sure references needed for clean-up are immediately released", "¡Oh, oh! El recuento de referencias del motor aumenta mientras se destruye. Asegúrese de que las referencias necesarias para la limpieza se liberen de inmediato."},
                    {"The module is still in use and cannot be rebuilt. Discard it and request another module", "El módulo todavía está en uso y no se puede reconstruir. Descartarlo y solicitar otro módulo"},
                    {"Property", "Propiedad"},
                    {"System function", "Función del sistema"},
                    {"Variable declaration", "Declaración de variable"},
                    {"Stack overflow", "Desbordamiento de pila"},
                    {"Null pointer access", "Acceso a puntero nulo"},
                    {"Divide by zero", "División por cero"},
                    {"Overflow in integer division", "Desbordamiento en división de enteros"},
                    {"Overflow in exponent operation", "Desbordamiento en operación exponente"},
                    {"Unrecognized byte code", "'Bytecode' no reconocido"},
                    {"Invalid calling convention", "Convención de llamadas no válida"},
                    {"Unbound function called", "Llamada a función sin vincular (unbound)"},
                    {"Out of range", "Fuera de rango"},
                    {"Caught an exception from the application", "Detectó una excepción de la aplicación."},
                    {"Mismatching types in value assignment", "Tipos no coincidentes en la asignación de valor"},
                    {"Too many nested calls", "Demasiadas llamadas anidadas"}};

            auto it = staticMap.find(originalMsg);
            if (it != staticMap.end())
            {
                return it->second;
            }

            // 2. Dynamic regex matches (Sequential)

            {
                static const std::regex rgx(R"('(.*)'\ is\ already\ declared)");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("'" + match[1].str() + "' ya está declarado");
                }
            }
            {
                static const std::regex rgx(R"(Abstract\ class\ '(.*)'\ cannot\ be\ instantiated)");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("No se puede crear una instancia de la clase abstracta '" + match[1].str() + "'");
                }
            }
            {
                static const std::regex rgx(R"(Accessing\ private\ property\ '(.*)'\ of\ parent\ class)");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("Accediendo a la propiedad privada '" + match[1].str() + "' de la clase base");
                }
            }
            {
                static const std::regex rgx(R"(Rejected\ due\ to\ type\ mismatch\ at\ positional\ parameter\ (-?\\d+))");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("Rechazado debido a una discrepancia de tipos en el parámetro posicional %i");
                }
            }
            {
                static const std::regex rgx(R"(Rejected\ due\ to\ type\ mismatch\ on\ parameter\ '(.*)')");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("Rechazado debido a que el tipo no coincide en el parámetro '" + match[1].str() + "'");
                }
            }
            {
                static const std::regex rgx(R"(Attribute\ '(.*)'\ informed\ multiple\ times)");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("Atributo '" + match[1].str() + "' informado varias veces");
                }
            }
            {
                static const std::regex rgx(R"(Both\ conditions\ must\ initialize\ member\ '(.*)')");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("Ambas condiciones deben inicializar el miembro '" + match[1].str() + "'");
                }
            }
            {
                static const std::regex rgx(R"(Can't\ form\ arrays\ of\ subtype\ '(.*)')");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("No se pueden formar 'arrays' del subtipo '" + match[1].str() + "'");
                }
            }
            {
                static const std::regex rgx(R"(Can't\ inherit\ from\ class\ '(.*)'\ marked\ as\ final)");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("No se puede heredar de la clase '" + match[1].str() + "' marcada como final");
                }
            }
            {
                static const std::regex rgx(R"(Cannot\ access\ non\-static\ member\ '(.*)'\ like\ this)");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("No se puede acceder al miembro no estático '" + match[1].str() + "' de esta manera");
                }
            }
            {
                static const std::regex rgx(R"(Can't\ construct\ handle\ '(.*)'\.\ Use\ ref\ cast\ instead)");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("No se puede construir el 'handle' '" + match[1].str() + "'. Utilice un 'ref cast' en su lugar");
                }
            }
            {
                static const std::regex rgx(R"(Can't\ implicitly\ convert\ from\ '(.*)'\ to\ '(.*)'\.)");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("No se puede convertir implícitamente de '" + match[1].str() + "' a '" + match[2].str() + "'.");
                }
            }
            {
                static const std::regex rgx(R"(Compiling\ (.*))");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("Compilando " + match[1].str() + "");
                }
            }
            {
                static const std::regex rgx(R"(Compiling\ auto\ generated\ (.*))");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("Compilando " + match[1].str() + " generado automáticamente");
                }
            }
            {
                static const std::regex rgx(R"(Implemented\ property\ accessor\ '(.*)'\ does\ not\ expect\ index\ argument)");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("El descriptor de acceso a la propiedad implementado '" + match[1].str() + "' no espera un argumento de índice");
                }
            }
            {
                static const std::regex rgx(R"(Implemented\ property\ accessor\ '(.*)'\ expects\ index\ argument)");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("El descriptor de acceso de propiedad implementado '" + match[1].str() + "' espera un argumento de índice");
                }
            }
            {
                static const std::regex rgx(R"(Data\ type\ can't\ be\ '(.*)')");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("El tipo de dato no puede ser '" + match[1].str() + "'");
                }
            }
            {
                static const std::regex rgx(R"(All\ subsequent\ parameters\ after\ the\ first\ default\ value\ must\ have\ default\ values\ in\ function\ '(.*)')");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("Todos los parámetros posteriores al primer valor predeterminado deben tener valores predeterminados en la función '" + match[1].str() + "'");
                }
            }
            {
                static const std::regex rgx(R"(The\ method\ in\ the\ derived\ class\ must\ have\ the\ same\ return\ type\ as\ in\ the\ base\ class:\ '(.*)')");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("El método de la clase derivada debe tener el mismo tipo de retorno que el de la clase base: '" + match[1].str() + "'");
                }
            }
            {
                static const std::regex rgx(R"(The\ name\ of\ the\ destructor\ '(.*)::\~(.*)'\ must\ be\ the\ same\ as\ the\ class)");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("El nombre del destructor '" + match[1].str() + "::~" + match[2].str() + "' debe ser el mismo que el de la clase");
                }
            }
            {
                static const std::regex rgx(R"(Duplicate\ named\ argument\ '(.*)')");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("Argumento con nombre duplicado '" + match[1].str() + "'");
                }
            }
            {
                static const std::regex rgx(R"(Expected\ '(.*)')");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("Se esperaba '" + match[1].str() + "'");
                }
            }
            {
                static const std::regex rgx(R"(Expected\ '(.*)'\ or\ '(.*)')");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("Se esperaba '" + match[1].str() + "' o '" + match[2].str() + "'");
                }
            }
            {
                static const std::regex rgx(R"(Expression\ must\ be\ of\ boolean\ type,\ instead\ found\ '(.*)')");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("La expresión debe ser de tipo booleano, en su lugar se encuentra '" + match[1].str() + "'");
                }
            }
            {
                static const std::regex rgx(R"(Expression\ '(.*)'\ is\ a\ data\ type)");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("La expresión '" + match[1].str() + "' es un tipo de datos");
                }
            }
            {
                static const std::regex rgx(R"(External\ shared\ entity\ '(.*)'\ not\ found)");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("No se encontró la entidad compartida externa '" + match[1].str() + "'");
                }
            }
            {
                static const std::regex rgx(R"(External\ shared\ entity\ '(.*)'\ cannot\ redefine\ the\ original\ entity)");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("La entidad compartida externa '" + match[1].str() + "' no puede redefinir la entidad original");
                }
            }
            {
                static const std::regex rgx(R"(Failed\ while\ compiling\ default\ arg\ for\ parameter\ (-?\\d+)\ in\ function\ '(.*)')");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("Error al compilar el argumento predeterminado para el parámetro " + match[1].str() + " en la función '" + match[1].str() + "'");
                }
            }
            {
                static const std::regex rgx(R"(Function\ '(.*)'\ not\ found)");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("Función '" + match[1].str() + "' no encontrada");
                }
            }
            {
                static const std::regex rgx(R"(The\ property\ '(.*)'\ has\ mismatching\ types\ for\ the\ get\ and\ set\ accessors)");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("La propiedad '" + match[1].str() + "' tiene tipos que no coinciden para los descriptores de acceso get y set");
                }
            }
            {
                static const std::regex rgx(R"(Variable\ '(.*)'\ hides\ another\ variable\ of\ same\ name\ in\ outer\ scope)");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("La variable '" + match[1].str() + "' oculta otra variable con el mismo nombre en el ámbito externo");
                }
            }
            {
                static const std::regex rgx(R"(Identifier\ '(.*)'\ is\ not\ a\ data\ type)");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("El identificador '" + match[1].str() + "' no es un tipo de dato");
                }
            }
            {
                static const std::regex rgx(R"(Identifier\ '(.*)'\ is\ not\ a\ data\ type\ in\ global\ namespace)");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("El identificador '" + match[1].str() + "' no es un tipo de dato en el espacio de nombres global");
                }
            }
            {
                static const std::regex rgx(R"(Identifier\ '(.*)'\ is\ not\ a\ data\ type\ in\ namespace\ '(.*)'\ or\ parent)");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("El identificador '" + match[1].str() + "' no es un tipo de datos en el espacio de nombres '" + match[2].str() + "' o padre");
                }
            }
            {
                static const std::regex rgx(R"(Illegal\ operation\ on\ '(.*)')");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("Operación ilegal en '" + match[1].str() + "'");
                }
            }
            {
                static const std::regex rgx(R"(Illegal\ return\ by\ value\ for\ '(.*)'\ in\ type\ cast)");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("Devolución por valor ilegal para '" + match[1].str() + "' en 'type cast'");
                }
            }
            {
                static const std::regex rgx(R"(Illegal\ variable\ name\ '(.*)'\.)");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("Nombre de variable ilegal '" + match[1].str() + "'.");
                }
            }
            {
                static const std::regex rgx(R"(Illegal\ access\ to\ inherited\ private\ property\ '(.*)')");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("Acceso ilegal a propiedad privada heredada '" + match[1].str() + "'");
                }
            }
            {
                static const std::regex rgx(R"(Initialization\ lists\ cannot\ be\ used\ with\ '(.*)')");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("Las listas de inicialización no se pueden usar con el tipo '" + match[1].str() + "'");
                }
            }
            {
                static const std::regex rgx(R"(Attempting\ to\ instantiate\ invalid\ template\ '(.*)<(.*)>')");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("Intentando crear una instancia de la plantilla no válida '" + match[1].str() + "<" + match[2].str() + ">'");
                }
            }
            {
                static const std::regex rgx(R"(Instead\ found\ '(.*)')");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("En su lugar se encontró '" + match[1].str() + "'");
                }
            }
            {
                static const std::regex rgx(R"(Instead\ found\ identifier\ '(.*)')");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("En su lugar se encontró el identificador '" + match[1].str() + "'");
                }
            }
            {
                static const std::regex rgx(R"(Instead\ found\ reserved\ keyword\ '(.*)')");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("En su lugar se encontró la palabra clave reservada '" + match[1].str() + "'");
                }
            }
            {
                static const std::regex rgx(R"(Interface\ '(.*)'\ cannot\ be\ instantiated)");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("No se puede crear una instancia de la interfaz '" + match[1].str() + "'");
                }
            }
            {
                static const std::regex rgx(R"(Invalid\ unicode\ escape\ sequence,\ expected\ (-?\\d+)\ hex\ digits)");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("Secuencia de escape Unicode no válida, se esperan dígitos hexadecimales %d");
                }
            }
            {
                static const std::regex rgx(R"(The\ member\ '(.*)'\ is\ accessed\ before\ the\ initialization)");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("Se accede al miembro '" + match[1].str() + "' antes de la inicialización.");
                }
            }
            {
                static const std::regex rgx(R"(Method\ '(.*)'\ declared\ as\ final\ and\ cannot\ be\ overridden)");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("El método '" + match[1].str() + "' declarado como final y no se puede anular");
                }
            }
            {
                static const std::regex rgx(R"(Method\ '(.*)'\ marked\ as\ override\ but\ does\ not\ replace\ any\ base\ class\ or\ interface\ method)");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("El método '" + match[1].str() + "' está marcado como anulado pero no reemplaza ninguna clase base o método de interfaz");
                }
            }
            {
                static const std::regex rgx(R"(Method\ '(.*)::(.*)'\ is\ missing\ the\ return\ type,\ nor\ is\ it\ the\ same\ name\ as\ object\ to\ be\ a\ constructor)");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("Al método '" + match[1].str() + "::" + match[2].str() + "' le falta el tipo de retorno, ni es el mismo nombre que el objeto para ser constructor");
                }
            }
            {
                static const std::regex rgx(R"(Method\ '(.*)'\ is\ not\ part\ of\ object\ '(.*)')");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("El método '" + match[1].str() + "' no forma parte del objeto '" + match[2].str() + "'");
                }
            }
            {
                static const std::regex rgx(R"(Rejected\ due\ to\ named\ parameter\ '(.*)'\ missing)");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("Rechazado debido a que falta el parámetro con nombre '" + match[1].str() + "'");
                }
            }
            {
                static const std::regex rgx(R"(Missing\ implementation\ of\ '(.*)')");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("Falta implementación de '" + match[1].str() + "'");
                }
            }
            {
                static const std::regex rgx(R"(Missing\ definition\ of\ '(.*)')");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("Falta la definición de '" + match[1].str() + "'");
                }
            }
            {
                static const std::regex rgx(R"(Missing\ or\ invalid\ definition\ of\ '(.*)')");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("Definición faltante o no válida de '" + match[1].str() + "'");
                }
            }
            {
                static const std::regex rgx(R"(Mixin\ class\ cannot\ be\ declared\ as\ '(.*)')");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("La clase Mixin no se puede declarar como '" + match[1].str() + "'");
                }
            }
            {
                static const std::regex rgx(R"(Multiple\ matching\ signatures\ to\ '(.*)')");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("Existen múltiples firmas coincidentes para '" + match[1].str() + "'");
                }
            }
            {
                static const std::regex rgx(R"(Found\ multiple\ get\ accessors\ for\ property\ '(.*)')");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("Se encontraron varios descriptores de acceso para obtener la propiedad '" + match[1].str() + "'");
                }
            }
            {
                static const std::regex rgx(R"(Found\ multiple\ set\ accessors\ for\ property\ '(.*)')");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("Se encontraron múltiples descriptores de acceso para la propiedad '" + match[1].str() + "'");
                }
            }
            {
                static const std::regex rgx(R"(Namespace\ '(.*)'\ doesn't\ exist\.)");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("El espacio de nombres '" + match[1].str() + "' no existe.");
                }
            }
            {
                static const std::regex rgx(R"(Name\ conflict\.\ '(.*)'\ is\ an\ extended\ data\ type\.)");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("Conflicto de nombres. '" + match[1].str() + "' es un tipo de datos extendido.");
                }
            }
            {
                static const std::regex rgx(R"(Name\ conflict\.\ '(.*)'\ is\ a\ global\ property\.)");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("Conflicto de nombres. '" + match[1].str() + "' es una propiedad global.");
                }
            }
            {
                static const std::regex rgx(R"(Name\ conflict\.\ '(.*)'\ is\ a\ named\ type\.)");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("Conflicto de nombres. '" + match[1].str() + "' es un tipo con nombre.");
                }
            }
            {
                static const std::regex rgx(R"(Name\ conflict\.\ '(.*)'\ is\ a\ funcdef\.)");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("Conflicto de nombres. '" + match[1].str() + "' es un 'funcdef' (definición de función).");
                }
            }
            {
                static const std::regex rgx(R"(Name\ conflict\.\ '(.*)'\ is\ a\ global\ function\.)");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("Conflicto de nombres. '" + match[1].str() + "' es una función global.");
                }
            }
            {
                static const std::regex rgx(R"(Name\ conflict\.\ '(.*)'\ is\ a\ mixin\ class\.)");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("Conflicto de nombres. '" + match[1].str() + "' es una clase mixta.");
                }
            }
            {
                static const std::regex rgx(R"(Name\ conflict\.\ '(.*)'\ is\ a\ virtual\ property\.)");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("Conflicto de nombres. '" + match[1].str() + "' es una propiedad virtual.");
                }
            }
            {
                static const std::regex rgx(R"(Name\ conflict\.\ '(.*)'\ is\ a\ class\.)");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("Conflicto de nombres. '" + match[1].str() + "' es una clase.");
                }
            }
            {
                static const std::regex rgx(R"(Name\ conflict\.\ '(.*)'\ is\ an\ interface\.)");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("Conflicto de nombres. '" + match[1].str() + "' es una interfaz.");
                }
            }
            {
                static const std::regex rgx(R"(Name\ conflict\.\ '(.*)'\ is\ an\ object\ property\.)");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("Conflicto de nombres. '" + match[1].str() + "' es una propiedad de objeto.");
                }
            }
            {
                static const std::regex rgx(R"(Name\ conflict\.\ '(.*)'\ is\ a\ class\ method\.)");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("Conflicto de nombres. '" + match[1].str() + "' es un método de clase.");
                }
            }
            {
                static const std::regex rgx(R"(Name\ conflict\.\ '(.*)'\ is\ already\ used\.)");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("Conflicto de nombres. '" + match[1].str() + "' ya está en uso.");
                }
            }
            {
                static const std::regex rgx(R"(No\ appropriate\ opHndlAssign\ method\ found\ in\ '(.*)'\ for\ handle\ assignment)");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("No se encontró ningún método 'opHndlAssign' apropiado en '" + match[1].str() + "' para la asignación de 'handle'");
                }
            }
            {
                static const std::regex rgx(R"(No\ conversion\ from\ '(.*)'\ to\ '(.*)'\ available\.)");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("No hay conversión disponible de '" + match[1].str() + "' a '" + match[2].str() + "'.");
                }
            }
            {
                static const std::regex rgx(R"(No\ conversion\ from\ '(.*)'\ to\ math\ type\ available\.)");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("No hay conversión disponible de '" + match[1].str() + "' al tipo matemático.");
                }
            }
            {
                static const std::regex rgx(R"(No\ default\ constructor\ for\ object\ of\ type\ '(.*)'\.)");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("No hay constructor por defecto para el objeto de tipo '" + match[1].str() + "'.");
                }
            }
            {
                static const std::regex rgx(R"(No\ appropriate\ opAssign\ method\ found\ in\ '(.*)'\ for\ value\ assignment)");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("No se encontró un método opAssign apropiado en '" + match[1].str() + "' para la asignación");
                }
            }
            {
                static const std::regex rgx(R"(No\ copy\ constructor\ for\ object\ of\ type\ '(.*)'\.)");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("No hay constructor de copia para el objeto de tipo '" + match[1].str() + "'.");
                }
            }
            {
                static const std::regex rgx(R"(No\ matching\ signatures\ to\ '(.*)')");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("No hay firmas coincidentes para la función '" + match[1].str() + "'");
                }
            }
            {
                static const std::regex rgx(R"(No\ matching\ operator\ that\ takes\ the\ type\ '(.*)'\ found)");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("No se encontró ningún operador coincidente que tome el tipo '" + match[1].str() + "'");
                }
            }
            {
                static const std::regex rgx(R"(No\ matching\ operator\ that\ takes\ the\ types\ '(.*)'\ and\ '(.*)'\ found)");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("No se encontró ningún operador coincidente que tome los tipos '" + match[1].str() + "' y '" + match[2].str() + "'");
                }
            }
            {
                static const std::regex rgx(R"(No\ matching\ symbol\ '(.*)')");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("No hay símbolo coincidente '" + match[1].str() + "'");
                }
            }
            {
                static const std::regex rgx(R"(Expression\ doesn't\ form\ a\ function\ call\.\ '(.*)'\ evaluates\ to\ the\ non\-function\ type\ '(.*)')");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("La expresión no forma una llamada de función. '" + match[1].str() + "' se evalúa como el tipo sin función '" + match[2].str() + "'");
                }
            }
            {
                static const std::regex rgx(R"('(.*)'\ is\ not\ declared)");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("'" + match[1].str() + "' no está declarado");
                }
            }
            {
                static const std::regex rgx(R"(Type\ '(.*)'\ is\ not\ valid\ type\ for\ foreach\ loops)");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("El tipo '" + match[1].str() + "' no es un tipo válido para bucles foreach");
                }
            }
            {
                static const std::regex rgx(R"('(.*)'\ is\ not\ initialized\.)");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("'" + match[1].str() + "' no está inicializado.");
                }
            }
            {
                static const std::regex rgx(R"('(.*)'\ is\ not\ a\ member\ of\ '(.*)')");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("'" + match[1].str() + "' no es miembro de '" + match[2].str() + "'");
                }
            }
            {
                static const std::regex rgx(R"(Ambiguous\ symbol\ name\ '(.*)')");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("Nombre de símbolo ambiguo '" + match[1].str() + "'");
                }
            }
            {
                static const std::regex rgx(R"(Type\ '(.*)'\ doesn't\ support\ the\ indexing\ operator)");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("El tipo '" + match[1].str() + "' no admite el operador de indexación");
                }
            }
            {
                static const std::regex rgx(R"(Parameter\ type\ can't\ be\ '(.*)',\ because\ the\ type\ cannot\ be\ instantiated\.)");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("El tipo de parámetro no puede ser '" + match[1].str() + "' porque no se puede crear una instancia del tipo.");
                }
            }
            {
                static const std::regex rgx(R"(Previous\ error\ occurred\ while\ attempting\ to\ compile\ initialization\ list\ for\ type\ '(.*)')");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("Se produjo un error anterior al intentar compilar la lista de inicialización para el tipo '" + match[1].str() + "'");
                }
            }
            {
                static const std::regex rgx(R"(Illegal\ call\ to\ private\ method\ '(.*)')");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("Llamada ilegal al método privado '" + match[1].str() + "'");
                }
            }
            {
                static const std::regex rgx(R"(Illegal\ access\ to\ private\ property\ '(.*)')");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("Acceso ilegal a propiedad privada '" + match[1].str() + "'");
                }
            }
            {
                static const std::regex rgx(R"(Illegal\ call\ to\ protected\ method\ '(.*)')");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("Llamada ilegal al método protegido '" + match[1].str() + "'");
                }
            }
            {
                static const std::regex rgx(R"(Illegal\ access\ to\ protected\ property\ '(.*)')");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("Acceso ilegal a la propiedad protegida '" + match[1].str() + "'");
                }
            }
            {
                static const std::regex rgx(R"(Return\ type\ can't\ be\ '(.*)')");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("El tipo de devolución no puede ser '" + match[1].str() + "'");
                }
            }
            {
                static const std::regex rgx(R"(Shared\ code\ cannot\ access\ non\-shared\ global\ variable\ '(.*)')");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("El código compartido no puede acceder a la variable global no compartida '" + match[1].str() + "'");
                }
            }
            {
                static const std::regex rgx(R"(Shared\ code\ cannot\ call\ non\-shared\ function\ '(.*)')");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("El código compartido no puede llamar a la función no compartida '" + match[1].str() + "'");
                }
            }
            {
                static const std::regex rgx(R"(Shared\ type\ cannot\ implement\ non\-shared\ interface\ '(.*)')");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("El tipo compartido no puede implementar la interfaz no compartida '" + match[1].str() + "'");
                }
            }
            {
                static const std::regex rgx(R"(Shared\ class\ cannot\ inherit\ from\ non\-shared\ class\ '(.*)')");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("La clase compartida no puede heredar de la clase no compartida '" + match[1].str() + "'");
                }
            }
            {
                static const std::regex rgx(R"(Shared\ code\ cannot\ use\ non\-shared\ type\ '(.*)')");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("El código compartido no puede utilizar el tipo no compartido '" + match[1].str() + "'");
                }
            }
            {
                static const std::regex rgx(R"(Shared\ type\ '(.*)'\ doesn't\ match\ the\ declaration\ in\ module\ '(.*)')");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("El tipo compartido '" + match[1].str() + "' no coincide con la declaración en el módulo '" + match[2].str() + "'");
                }
            }
            {
                static const std::regex rgx(R"(Template\ '(.*)'\ expects\ (-?\\d+)\ sub\ type\(s\))");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("La plantilla '" + match[1].str() + "' espera subtipos " + match[1].str() + "");
                }
            }
            {
                static const std::regex rgx(R"(Type\ '(.*)'\ cannot\ be\ a\ reference)");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("El tipo '" + match[1].str() + "' no puede ser una referencia");
                }
            }
            {
                static const std::regex rgx(R"(Type\ '(.*)'\ is\ not\ available\ for\ this\ module)");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("El tipo '" + match[1].str() + "' no está disponible para este módulo");
                }
            }
            {
                static const std::regex rgx(R"(Type\ '(.*)'\ is\ not\ a\ template\ type)");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("El tipo '" + match[1].str() + "' no es un tipo de plantilla");
                }
            }
            {
                static const std::regex rgx(R"(Unexpected\ token\ '(.*)')");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("Token inesperado '" + match[1].str() + "'");
                }
            }
            {
                static const std::regex rgx(R"(Use\ of\ uninitialized\ global\ variable\ '(.*)'\.)");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("Uso de la variable global no inicializada '" + match[1].str() + "'.");
                }
            }
            {
                static const std::regex rgx(R"(Unknown\ scope\ '(.*)')");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("Ámbito desconocido '" + match[1].str() + "'");
                }
            }
            {
                static const std::regex rgx(R"(Where\ '(.*)'\ is\ '(.*)')");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("Donde '" + match[1].str() + "' es '" + match[2].str() + "'");
                }
            }
            {
                static const std::regex rgx(R"(Failed\ to\ initialize\ global\ variable\ '(.*)')");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("No se pudo inicializar la variable global '" + match[1].str() + "'");
                }
            }
            {
                static const std::regex rgx(R"(Exception\ '(.*)'\ in\ '(.*)')");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("Excepción '" + match[1].str() + "' en '" + match[2].str() + "'");
                }
            }
            {
                static const std::regex rgx(R"(Type\ '(.*)'\ is\ missing\ behaviours)");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("Al tipo '" + match[1].str() + "' le faltan comportamientos");
                }
            }
            {
                static const std::regex rgx(R"(Can't\ pass\ type\ '(.*)'\ by\ value\ unless\ the\ application\ type\ is\ informed\ in\ the\ registration)");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("No se puede pasar el tipo '" + match[1].str() + "' por valor a menos que se informe el tipo de aplicación en el registro");
                }
            }
            {
                static const std::regex rgx(R"(Can't\ return\ type\ '(.*)'\ by\ value\ unless\ the\ application\ type\ is\ informed\ in\ the\ registration)");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("No se puede devolver el tipo '" + match[1].str() + "' por valor a menos que se informe el tipo de aplicación en el registro");
                }
            }
            {
                static const std::regex rgx(R"(Don't\ support\ passing\ type\ '(.*)'\ by\ value\ to\ application\ in\ native\ calling\ convention\ on\ this\ platform)");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("No se admite pasar el tipo '" + match[1].str() + "' por valor a la aplicación en la convención de llamadas nativa en esta plataforma");
                }
            }
            {
                static const std::regex rgx(R"(Don't\ support\ returning\ type\ '(.*)'\ by\ value\ from\ application\ in\ native\ calling\ convention\ on\ this\ platform)");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("No se admite la devolución del tipo '" + match[1].str() + "' por valor de la aplicación en la convención de llamadas nativa en esta plataforma");
                }
            }
            {
                static const std::regex rgx(R"(Object\ \{(-?\\d+)\}\.\ GC\ cannot\ destroy\ an\ object\ of\ type\ '(.*)'\ as\ it\ doesn't\ know\ how\ many\ references\ to\ there\ are\.)");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("Objeto {" + match[1].str() + "}. GC no puede destruir un objeto de tipo '" + match[1].str() + "' porque no sabe cuántas referencias hay.");
                }
            }
            {
                static const std::regex rgx(R"(Object\ \{(-?\\d+)\}\.\ GC\ cannot\ destroy\ an\ object\ of\ type\ '(.*)'\ as\ it\ can't\ see\ all\ references\.\ Current\ ref\ count\ is\ (-?\\d+)\.)");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("Objeto {" + match[1].str() + "}. GC no puede destruir un objeto de tipo '" + match[1].str() + "' porque no puede ver todas las referencias. El recuento de referencias actual es " + match[1].str() + ".");
                }
            }
            {
                static const std::regex rgx(R"(Object\ type\ '(.*)'\ doesn't\ exist)");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("El tipo de objeto '" + match[1].str() + "' no existe");
                }
            }
            {
                static const std::regex rgx(R"(Cannot\ register\.\ The\ template\ type\ instance\ '(.*)'\ has\ already\ been\ generated\.)");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("No se puede registrar. La instancia de tipo de plantilla '" + match[1].str() + "' ya se ha generado.");
                }
            }
            {
                static const std::regex rgx(R"(Template\ type\ '(.*)'\ doesn't\ exist)");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("El tipo de plantilla '" + match[1].str() + "' no existe");
                }
            }
            {
                static const std::regex rgx(R"(Template\ subtype\ '(.*)'\ doesn't\ exist)");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("El subtipo de plantilla '" + match[1].str() + "' no existe");
                }
            }
            {
                static const std::regex rgx(R"(Failed\ to\ read\ subtype\ of\ template\ type\ '(.*)')");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("No se pudo leer el subtipo del tipo de plantilla '" + match[1].str() + "'");
                }
            }
            {
                static const std::regex rgx(R"(Failed\ in\ call\ to\ function\ '(.*)'\ \(Code:\ (.*),\ (-?\\d+)\))");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("Error al llamar a la función '" + match[1].str() + "' (Código: " + match[2].str() + ", ");
                }
            }
            {
                static const std::regex rgx(R"(Failed\ in\ call\ to\ function\ '(.*)'\ with\ '(.*)'\ \(Code:\ (.*),\ (-?\\d+)\))");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("Error al llamar a la función '%s' con '%s' (Código: %s, %d)");
                }
            }
            {
                static const std::regex rgx(R"(Failed\ in\ call\ to\ function\ '(.*)'\ with\ '(.*)'\ and\ '(.*)'\ \(Code:\ (.*),\ (-?\\d+)\))");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("Error al llamar a la función '%s' con '%s' y '%s' (Código: %s, %d)");
                }
            }
            {
                static const std::regex rgx(R"(Type\ '(.*)'\ is\ still\ used\ by\ function\ '(.*)')");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("El tipo '" + match[1].str() + "' todavía lo utiliza la función '" + match[2].str() + "'");
                }
            }
            {
                static const std::regex rgx(R"(The\ builtin\ type\ in\ previous\ message\ is\ named\ '(.*)')");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("El tipo incorporado en el mensaje anterior se llama '" + match[1].str() + "'");
                }
            }
            {
                static const std::regex rgx(R"(The\ function\ in\ previous\ message\ is\ named\ '(.*)'\.\ The\ func\ type\ is\ (-?\\d+))");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("La función del mensaje anterior se denomina '" + match[1].str() + "'. El tipo de función es " + match[1].str() + "");
                }
            }
            {
                static const std::regex rgx(R"(The\ script\ object\ of\ type\ '(.*)'\ is\ being\ resurrected\ illegally\ during\ destruction)");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("El objeto script de tipo '" + match[1].str() + "' está resucitando ilegalmente durante la destrucción");
                }
            }
            {
                static const std::regex rgx(R"(LoadByteCode\ failed\.\ The\ bytecode\ is\ invalid\.\ Number\ of\ bytes\ read\ from\ stream:\ (-?\\d+))");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("Error en LoadByteCode. El 'bytecode' no es válido. Número de bytes leídos del 'stream': %d");
                }
            }
            {
                static const std::regex rgx(R"(Function\ '(.*)'\ appears\ to\ have\ been\ compiled\ without\ JIT\ entry\ points)");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("La función '" + match[1].str() + "' parece haber sido compilada sin puntos de entrada JIT");
                }
            }
            {
                static const std::regex rgx(R"(There\ is\ an\ external\ reference\ to\ an\ object\ in\ module\ '(.*)',\ preventing\ it\ from\ being\ deleted)");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("Hay una referencia externa a un objeto en el módulo '" + match[1].str() + "', lo que impide que se elimine");
                }
            }
            {
                static const std::regex rgx(R"(The\ engine\ was\ shutdown\ before\ the\ context\ released\.\ Function\ '(.*)'\ cannot\ be\ cleaned\ up)");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("El motor se apagó antes de que se liberara el contexto. La función '" + match[1].str() + "' no se puede limpiar");
                }
            }
            {
                static const std::regex rgx(R"(Invalid\ #include\ directive:\ unclosed\ path\ delimiter\ '(.*)')");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("Directiva #include no válida: delimitador de ruta sin cerrar '" + match[1].str() + "'");
                }
            }
            {
                static const std::regex rgx(R"(Included\ file\ not\ found:\ '(.*)')");
                std::smatch match;
                if (std::regex_match(originalMsg, match, rgx))
                {
                    return std::string("Archivo incluido no encontrado: '" + match[1].str() + "'");
                }
            }
        }

        // Fallback
        return originalMsg;
    }

} // namespace angel_lsp::i18n
