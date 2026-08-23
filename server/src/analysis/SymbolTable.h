#pragma once

#include "utils/LspLogger.h"

#include <memory>
#include <string>
#include <vector>
#include <shared_mutex>
#include <optional>
#include <ankerl/unordered_dense.h>

#include <variant>

namespace angel_lsp::analysis
{
    enum class SymbolType
    {
        Variable,
        Function,
        Class,
        Interface,
        Enum,
        Typedef,
        Namespace,
        Funcdef,
        Property,
        CallReference
    };

    enum class AccessModifier
    {
        Public,
        Private,
        Protected
    };

    enum class ParameterModifier
    {
        None,
        In,
        Out,
        InOut
    };

    enum class TypeKind
    {
        Unknown,
        Auto,
        Void,
        Int8,
        Int16,
        Int32,
        Int64,
        UInt8,
        UInt16,
        UInt32,
        UInt64,
        Float,
        Double,
        Bool,
        String,
        Object,
        Array,
        Handle,
        // Aliases placed last so they don't reset the implicit counter for the entries above.
        Int = Int32,
        UInt = UInt32
    };

    struct SymbolModifiers
    {
        AccessModifier access = AccessModifier::Public;
        bool isConst = false;
        bool isHandle = false;
        bool isShared = false;
        bool isMixin = false;
        bool isAbstract = false;
        bool isFinal = false;
        bool isDeclarationFinal = false; ///< True when 'final' is a declaration_modifier (before type), not a func_attribute
        bool isDeclarationAbstract = false; ///< True when 'abstract' is a declaration_modifier (before type)
        bool isOverride = false;
        bool isExplicit = false;
        bool isProperty = false;
        bool isDelete = false;
        bool isExternal = false;
        bool isReturnReference = false;
        ParameterModifier paramModifier = ParameterModifier::None;
    };

    struct ParameterInformation
    {
        std::string name;
        std::string typeName;
        std::string rawText;
        std::string baseTypeName;
        std::string templateName;
        TypeKind typeKind = TypeKind::Unknown;
        bool isArray = false;
        bool hasPrimitiveHandle = false;
        uint32_t arrayDepth = 0;
        ParameterModifier modifier = ParameterModifier::None;
        std::string defaultValue;
        bool isHandle = false;
        bool isConst = false;
        bool isReference = false;
        bool hasDoubleReference = false;
        bool isStandaloneRef = false;

        uint32_t startLine = 0;
        uint32_t startCharacter = 0;
        uint32_t endLine = 0;
        uint32_t endCharacter = 0;
    };

    struct SourceRange
    {
        uint32_t startLine = 0;
        uint32_t startCharacter = 0;
        uint32_t endLine = 0;
        uint32_t endCharacter = 0;
    };

    struct TypeExtractionResult
    {
        std::string baseTypeName;
        std::string templateName;
        TypeKind kind = TypeKind::Unknown;
        bool isArray = false;
        bool isHandle = false;
        bool isReference = false;
        bool isConst = false;
        bool hasPrimitiveHandle = false;
        uint32_t arrayDepth = 0;
        std::vector<TypeExtractionResult> templateArguments;
    };

    struct FunctionSignature
    {
        std::string returnType;
        std::string returnBaseTypeName;
        std::string returnTemplateName;
        SymbolModifiers modifiers;
        TypeKind returnTypeKind = TypeKind::Unknown;
        bool returnIsArray = false;
        bool returnIsConst = false;
        bool returnHasPrimitiveHandle = false;
        uint32_t returnArrayDepth = 0;
        std::vector<ParameterInformation> parameters;
        bool hasBody = false;
        bool isInterfaceMethod = false;
        std::string defaultValue;
    };


    struct VariableSignature
    {
        std::string typeName;
        std::string baseTypeName;
        std::string templateName;
        std::vector<std::string> templateArgumentTypes;
        TypeKind typeKind = TypeKind::Unknown;
        bool isArray = false;
        bool hasPrimitiveHandle = false;
        uint32_t arrayDepth = 0;
        std::string defaultValue;
        SymbolModifiers modifiers;
        bool isVirtualProperty = false;
        bool hasGet = false;
        bool hasSet = false;
        bool isGetConst = false;
        bool hasBodyGet = false;
        bool hasBodySet = false;
        bool hasDuplicateGet = false;
        bool hasDuplicateSet = false;
        bool isGetOverride = false;
        bool isSetOverride = false;
        bool isGetFinal = false;
        bool isSetFinal = false;
        bool hasNullInitializer = false;
        bool hasSemicolon = true;
        bool isLocal = false;
    };

    struct EnumMemberInformation
    {
        std::string name;
        std::string value;
        std::string valueNodeType;
    };

    struct EnumSignature
    {
        SymbolModifiers modifiers;
        std::vector<EnumMemberInformation> members;
        bool hasBraces = false;
    };

    struct ClassSignature
    {
        std::vector<std::string> bases;
        std::vector<std::string> templateParams;
        SymbolModifiers modifiers;
        bool isTemplate = false;
        bool hasBraces = false;
    };

    struct InterfaceSignature
    {
        std::vector<std::string> inheritedInterfaces;
        SymbolModifiers modifiers;
    };

    struct TypedefSignature
    {
        std::string baseType;
        TypeKind typeKind = TypeKind::Unknown;
        bool hasSemicolon = true;
    };

    struct FuncdefSignature
    {
        std::string returnType;
        std::string returnBaseTypeName;
        std::string returnTemplateName;
        TypeKind returnTypeKind = TypeKind::Unknown;
        bool returnIsArray = false;
        bool returnIsConst = false;
        bool returnHasPrimitiveHandle = false;
        uint32_t returnArrayDepth = 0;
        SymbolModifiers modifiers;
        std::vector<ParameterInformation> parameters;
    };

    /** @brief Describes a function/method call that occurs outside of any function body
     *         (e.g. a global variable initializer, a class field initializer, or an enum value). */
    struct CallReferenceSignature
    {
        std::string calleeName;
        bool isMethodCall = false;
        std::string objectExpression;
    };

    struct Symbol
    {
        SymbolType type;
        std::string name;
        std::string containerName;
        std::string qualifiedName;
        std::string fileUri;

        uint32_t startLine = 0;
        uint32_t startCharacter = 0;
        uint32_t endLine = 0;
        uint32_t endCharacter = 0;

        SourceRange fullRange;       ///< Full enclosing source range of the declaration (for DocumentSymbol).
        SourceRange selectionRange;  ///< Source range of the identifier token itself (for DocumentSymbol/Rename).

        std::variant<
            std::monostate,
            FunctionSignature,
            VariableSignature,
            EnumSignature,
            ClassSignature,
            InterfaceSignature,
            TypedefSignature,
            FuncdefSignature,
            CallReferenceSignature
        > signature;

        FunctionSignature &GetFunction()
        {
            return std::get<FunctionSignature>(signature);
        }

        const FunctionSignature &GetFunction() const
        {
            return std::get<FunctionSignature>(signature);
        }

        VariableSignature &GetVariable()
        {
            return std::get<VariableSignature>(signature);
        }

        const VariableSignature &GetVariable() const
        {
            return std::get<VariableSignature>(signature);
        }

        EnumSignature &GetEnum()
        {
            return std::get<EnumSignature>(signature);
        }

        const EnumSignature &GetEnum() const
        {
            return std::get<EnumSignature>(signature);
        }

        ClassSignature &GetClass()
        {
            return std::get<ClassSignature>(signature);
        }

        const ClassSignature &GetClass() const
        {
            return std::get<ClassSignature>(signature);
        }

        InterfaceSignature &GetInterface()
        {
            return std::get<InterfaceSignature>(signature);
        }

        const InterfaceSignature &GetInterface() const
        {
            return std::get<InterfaceSignature>(signature);
        }

        TypedefSignature &GetTypedef()
        {
            return std::get<TypedefSignature>(signature);
        }

        const TypedefSignature &GetTypedef() const
        {
            return std::get<TypedefSignature>(signature);
        }

        FuncdefSignature &GetFuncdef()
        {
            return std::get<FuncdefSignature>(signature);
        }

        const FuncdefSignature &GetFuncdef() const
        {
            return std::get<FuncdefSignature>(signature);
        }

        CallReferenceSignature &GetCallReference()
        {
            return std::get<CallReferenceSignature>(signature);
        }

        const CallReferenceSignature &GetCallReference() const
        {
            return std::get<CallReferenceSignature>(signature);
        }
    };

    class SymbolTable
    {
    public:
        SymbolTable() = default;
        ~SymbolTable() = default;

        void AddSymbol(const Symbol &symbol);
        void ClearDocumentSymbols(const std::string &fileUri);

        /** @brief Atomically swaps every symbol belonging to fileUri for the ones collected in staging.
         *  @param fileUri Document whose symbols are being replaced.
         *  @param staging Table the fresh symbols were collected into.
         *  @note Clearing and then re-collecting takes two locks, and between them the document has
         *        no symbols at all - a window a reader on another thread can land in and see an
         *        empty file. Since analysis moved off the message loop that window is on the hot
         *        editing path, so the swap happens under a single write lock instead. */
        void ReplaceDocumentSymbols(const std::string &fileUri, const SymbolTable &staging);

        bool HasSymbol(const std::string &qualifiedName) const;
        bool HasSymbolAnywhere(const std::string &name) const;

        /** @brief Returns a snapshot handle to the overload list for the given qualified name.
         *  @return Shared pointer to an immutable symbol list. The snapshot stays valid even if
         *          the table is mutated afterwards (copy-on-write), or nullptr if not found.
         *  @note Prefer this over FindSymbols() for hot read paths (Hover, Completion) to avoid vector copy. */
        std::shared_ptr<const std::vector<Symbol>> FindSymbolsPtr(const std::string &qualifiedName) const;

        /** @brief Returns a copy of all symbols matching qualifiedName. Safe across mutations. */
        std::vector<Symbol> FindSymbols(const std::string &qualifiedName) const;

        std::optional<Symbol> FindFirstSymbol(const std::string &qualifiedName) const;

        /** @brief Iterates all symbols in the table.
         *  @param visitor Callback invoked for each (qualifiedName, symbol_list) pair.
         *  @note Holds a shared_lock for the duration of the iteration. */
        void ForEachSymbol(const std::function<void(const std::string &, const std::vector<Symbol> &)> &visitor) const;

        void PrintSymbols(angel_lsp::utils::LspLogger *logger) const;

    private:
        mutable std::shared_mutex m_mutex;
        ankerl::unordered_dense::map<std::string, std::shared_ptr<std::vector<Symbol>>> m_symbols;
    };

    /** @brief Converts SymbolType enum to lower/string representation. */
    inline std::string SymbolTypeToString(SymbolType type)
    {
        switch (type)
        {
        case SymbolType::Variable:  return "variable";
        case SymbolType::Function:  return "function";
        case SymbolType::Class:     return "class";
        case SymbolType::Interface: return "interface";
        case SymbolType::Enum:      return "enum";
        case SymbolType::Typedef:   return "typedef";
        case SymbolType::Namespace: return "namespace";
        case SymbolType::Funcdef:   return "funcdef";
        case SymbolType::Property:  return "property";
        case SymbolType::CallReference: return "call_reference";
        default:                    return "unknown";
        }
    }
}