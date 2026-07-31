#pragma once

#include "utils/LspLogger.h"

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
        Property
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
        Int = Int32,
        UInt8,
        UInt16,
        UInt32,
        UInt64,
        UInt = UInt32,
        Float,
        Double,
        Bool,
        String,
        Object,
        Array,
        Handle
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

        uint32_t startLine = 0;
        uint32_t startCharacter = 0;
        uint32_t endLine = 0;
        uint32_t endCharacter = 0;
    };

    struct FunctionSignature
    {
        std::string returnType;
        std::string returnBaseTypeName;
        std::string returnTemplateName;
        SymbolModifiers modifiers;
        TypeKind returnTypeKind = TypeKind::Unknown;
        bool returnIsArray = false;
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
        TypeKind typeKind = TypeKind::Unknown;
        bool isArray = false;
        bool hasPrimitiveHandle = false;
        uint32_t arrayDepth = 0;
        std::string defaultValue;
        SymbolModifiers modifiers;
    };

    struct EnumMemberInformation
    {
        std::string name;
        std::string value;
    };

    struct EnumSignature
    {
        SymbolModifiers modifiers;
        std::vector<EnumMemberInformation> members;
    };

    struct ClassSignature
    {
        std::vector<std::string> bases;
        SymbolModifiers modifiers;
        bool isTemplate = false;
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

        std::variant<
            std::monostate,
            FunctionSignature,
            VariableSignature,
            EnumSignature,
            ClassSignature,
            InterfaceSignature,
            TypedefSignature
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
    };

    class SymbolTable
    {
    public:
        SymbolTable() = default;
        ~SymbolTable() = default;

        void AddSymbol(const Symbol &symbol);
        void ClearDocumentSymbols(const std::string &fileUri);

        bool HasSymbol(const std::string &qualifiedName) const;
        bool HasSymbolAnywhere(const std::string &name) const;

        /** @brief Returns a pointer to the overload list for the given qualified name.
         *  @return Pointer into the internal map (valid until the next write), or nullptr if not found.
         *  @note Prefer this over FindSymbols() for hot read paths (Hover, Completion) to avoid vector copy. */
        const std::vector<Symbol> *FindSymbolsPtr(const std::string &qualifiedName) const;

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
        ankerl::unordered_dense::map<std::string, std::vector<Symbol>> m_symbols;
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
        default:                    return "unknown";
        }
    }
}