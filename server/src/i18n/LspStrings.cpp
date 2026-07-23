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
            .diagMethodMustBeCalled = "Method '{}' must be called with ()",
            .diagUndeclaredNamespace = "Undeclared namespace '{}' in using directive",
            .diagDuplicateUsingDirective = "Duplicate using namespace directive for '{}'",
            .diagInvalidImportModule = "Invalid module string in import directive",
            .diagDuplicateImport = "Redefinition of imported function '{}'",
            .diagDuplicateEnumName = "Enum '{}' has already been declared.",
            .diagDuplicateEnumerator = "Enumerator '{}' already exists in this enum declaration.",
            .diagInvalidEnumInitializer = "Enum initializer expression must be an integer type, got '{}'.",
            .diagTypedefCollision = "Typedef alias name '{}' collides with an existing declaration.",
            .diagDuplicateParamName = "Parameter '{}' is already declared in this function.",
            .diagDefaultParamOrder = "Parameters with default values must be at the end of the parameter list.",
            .diagDefaultParamTypeMismatch = "Default parameter value of type '{}' is not compatible with parameter type '{}'.",
            .diagUndeclaredType = "Type '{}' has not been declared.",
            .diagInvalidFuncAttr = "Attribute '{}' is only valid for class methods.",
            .diagVoidReturnWithValue = "A 'void' function cannot return a value.",
            .diagReturnTypeMismatch = "Cannot convert return value of type '{}' to return type '{}'.",
            .diagDuplicateClassName = "Class or interface '{}' has already been declared.",
            .diagInheritFinalClass = "Cannot inherit from 'final' class '{}'.",
            .diagMultipleClassInheritance = "A class can only inherit from one base class (in addition to multiple interfaces).",
            .diagUnimplementedInterfaceMethod = "Class '{}' does not implement interface method '{}'.",
            .diagOverrideNoMatchingBase = "Method '{}' marked with 'override' does not exist in any base class or interface.",
            .diagOverrideFinalMethod = "Cannot override method '{}' marked as 'final'.",
            .diagVirtPropTypeMismatch = "Virtual property accessor '{}' must use type '{}'.",
            .diagInvalidBinaryOperator = "Operator '{}' is not valid for types '{}' and '{}'.",
            .diagInvalidLogicalOperand = "Logical operator '{}' requires operand of type 'bool', but got '{}'.",
            .diagInvalidHandleComparison = "Operator '{}' can only be applied to handles or object references.",
            .diagCannotAssignToConst = "Cannot modify constant variable '{}'.",
            .diagInvalidCast = "Cannot explicitly cast from '{}' to '{}'.",
            .diagBreakOutsideLoop = "Statement 'break' can only be used inside a loop or switch.",
            .diagContinueOutsideLoop = "Statement 'continue' can only be used inside a loop.",
            .diagSwitchTypeMismatch = "Switch expression must be of integer, enum, or string type.",
            .diagDuplicateCaseValue = "Duplicate 'case' value '{}' in switch statement.",
            .diagMemberNotFound = "Type '{}' has no member '{}'.",
            .diagPrivateMemberAccess = "Cannot access private member '{}' of class '{}'.",
            .diagInvalidIndexType = "Type '{}' does not support indexing.",
            .diagInvalidIncrementOperand = "Increment/decrement operator requires a modifiable numeric variable.",
            .diagTernaryConditionType = "Ternary operator condition must be 'bool', got '{}'.",
            .diagTernaryTypeMismatch = "Incompatible types '{}' and '{}' in ternary operator.",
            .diagUnknownNamedParam = "Function '{}' has no parameter named '{}'.",
            .diagLValueRequired = "Expression passed to '&out' or '&inout' parameter must be a modifiable variable."
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
            .diagMethodMustBeCalled = "El método '{}' debe llamarse con ()",
            .diagUndeclaredNamespace = "Espacio de nombres '{}' no declarado en directiva using",
            .diagDuplicateUsingDirective = "Directiva using namespace duplicada para '{}'",
            .diagInvalidImportModule = "Cadena de módulo no válida en directiva import",
            .diagDuplicateImport = "Redefinición de la función importada '{}'",
            .diagDuplicateEnumName = "El enum '{}' ya ha sido declarado.",
            .diagDuplicateEnumerator = "El valor de enum '{}' ya existe en esta declaración.",
            .diagInvalidEnumInitializer = "La expresión inicializadora del enum debe ser de tipo entero, se obtuvo '{}'.",
            .diagTypedefCollision = "El nombre de typedef '{}' colisiona con una declaración existente.",
            .diagDuplicateParamName = "El parámetro '{}' ya ha sido declarado en esta función.",
            .diagDefaultParamOrder = "Los parámetros con valores por defecto deben ubicarse al final de la lista.",
            .diagDefaultParamTypeMismatch = "El valor por defecto de tipo '{}' no es compatible con el parámetro de tipo '{}'.",
            .diagUndeclaredType = "El tipo '{}' no ha sido declarado.",
            .diagInvalidFuncAttr = "El atributo '{}' solo es válido para métodos dentro de una clase.",
            .diagVoidReturnWithValue = "Una función 'void' no puede devolver un valor.",
            .diagReturnTypeMismatch = "No se puede convertir el valor retornado de tipo '{}' al tipo de retorno '{}'.",
            .diagDuplicateClassName = "La clase o interfaz '{}' ya ha sido declarada.",
            .diagInheritFinalClass = "No se puede heredar de la clase 'final' '{}'.",
            .diagMultipleClassInheritance = "Una clase solo puede heredar de una clase base (además de múltiples interfaces).",
            .diagUnimplementedInterfaceMethod = "La clase '{}' no implementa el método de interfaz '{}'.",
            .diagOverrideNoMatchingBase = "El método '{}' marcado con 'override' no existe en ninguna clase base o interfaz.",
            .diagOverrideFinalMethod = "No se puede sobrescribir el método marcado como 'final' '{}'.",
            .diagVirtPropTypeMismatch = "El accesor '{0}' de la propiedad virtual debe usar el tipo '{1}'.",
            .diagInvalidBinaryOperator = "Operador '{}' no válido para los tipos '{}' y '{}'.",
            .diagInvalidLogicalOperand = "El operador lógico '{}' requiere un operando de tipo 'bool', pero se obtuvo '{}'.",
            .diagInvalidHandleComparison = "El operador '{}' solo se puede aplicar a handles o referencias a objetos.",
            .diagCannotAssignToConst = "No se puede modificar la variable constante '{}'.",
            .diagInvalidCast = "No se puede realizar un cast explícito de '{}' a '{}'.",
            .diagBreakOutsideLoop = "La sentencia 'break' solo se puede usar dentro de un bucle o switch.",
            .diagContinueOutsideLoop = "La sentencia 'continue' solo se puede usar dentro de un bucle.",
            .diagSwitchTypeMismatch = "La expresión de switch debe ser de tipo entero, enum o cadena.",
            .diagDuplicateCaseValue = "Valor de 'case' duplicado '{}' en la sentencia switch.",
            .diagMemberNotFound = "El tipo '{}' no tiene el miembro '{}'.",
            .diagPrivateMemberAccess = "No se puede acceder al miembro privado '{}' de la clase '{}'.",
            .diagInvalidIndexType = "El tipo '{}' no admite indexación.",
            .diagInvalidIncrementOperand = "El operador de incremento/decremento requiere una variable numérica modificable.",
            .diagTernaryConditionType = "La condición del operador ternario debe ser 'bool', se obtuvo '{}'.",
            .diagTernaryTypeMismatch = "Tipos incompatibles '{}' y '{}' en el operador ternario.",
            .diagUnknownNamedParam = "La función '{}' no tiene un parámetro llamado '{}'.",
            .diagLValueRequired = "La expresión pasada a un parámetro '&out' o '&inout' debe ser una variable modificable."
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
