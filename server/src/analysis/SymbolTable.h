#pragma once

#include "utils/LspLogger.h"

#include <string>
#include <vector>
#include <shared_mutex>
#include <optional>
#include <ankerl/unordered_dense.h>

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
        Unknown, // e.g.: unresolved type
        Auto,    // e.g.: auto x = 5

        Void, // e.g.: void

        Int8,  // e.g.: int8
        Int16, // e.g.: int16
        Int32, // e.g.: int32 / int
        Int64, // e.g.: int64
        Int = Int32,

        UInt8,  // e.g.: uint8
        UInt16, // e.g.: uint16
        UInt32, // e.g.: uint32 / uint
        UInt64, // e.g.: uint64
        UInt = UInt32,

        Float,  // e.g.: float
        Double, // e.g.: double

        Bool,   // e.g.: bool
        String, // e.g.: string

        Object, // e.g.: class / struct
        Array,  // e.g.: array<T>
        Handle  // e.g.: Player@
    };

    struct SymbolModifiers
    {
        AccessModifier access = AccessModifier::Public; // e.g.: AccessModifier::Private

        bool isConst = false;                                      // e.g.: const int x
        bool isHandle = false;                                     // e.g.: Player@ p
        bool isShared = false;                                     // e.g.: shared class Entity
        bool isMixin = false;                                      // e.g.: mixin class Player
        bool isAbstract = false;                                   // e.g.: class Entity abstract
        bool isFinal = false;                                      // e.g.: class Player final
        bool isOverride = false;                                   // e.g.: void Update() override
        bool isExplicit = false;                                   // e.g.: explicit Player(int x)
        bool isProperty = false;                                   // e.g.: property int hp
        bool isDelete = false;                                     // e.g.: void Func() delete
        bool isExternal = false;                                   // e.g.: external void Func()
        bool isReturnReference = false;                            // e.g.: int& GetRef()
        ParameterModifier paramModifier = ParameterModifier::None; // e.g.: ParameterModifier::In || ParameterModifier::Out || ParameterModifier::InOut
    };

    struct ParameterInformation
    {
        std::string name;
        std::string typeName;
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
        std::string name;  // e.g.: "Red"
        std::string value; // e.g.: "0"
    };

    struct EnumSignature
    {
        SymbolModifiers modifiers;                  // e.g.: { isShared = true }
        std::vector<EnumMemberInformation> members; // e.g.: member list (filled in Pass 2)
    };

    struct ClassSignature
    {
        /** All base names as declared in source (unresolved). Pass 2 (SemanticAnalyzer)
         *  will resolve each entry into interfaces or mixin bases using the SymbolTable. */
        std::vector<std::string> bases; // e.g.: {"IUpdatable", "MyBase", "DynamicBehavior"}
        SymbolModifiers modifiers;      // e.g.: { isAbstract = true, isShared = true, isFinal = true }
        bool isTemplate = false;        // e.g.: class array<T>
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

        FunctionSignature functionSignature;
        VariableSignature variableSignature;
        EnumSignature enumSignature;
        ClassSignature classSignature;
        InterfaceSignature interfaceSignature;
        TypedefSignature typedefSignature;

        std::string fileUri;

        uint32_t startLine = 0;
        uint32_t startCharacter = 0;
        uint32_t endLine = 0;
        uint32_t endCharacter = 0;
    };

    class SymbolTable
    {
    public:
        SymbolTable() = default;
        ~SymbolTable() = default;

        void AddSymbol(const Symbol &symbol);
        void ClearDocumentSymbols(const std::string &fileUri);

        bool HasSymbol(const std::string &qualifiedName) const;

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
}