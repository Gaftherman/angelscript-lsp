#pragma once

#include "utils/LspLogger.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <shared_mutex>
#include <optional>
#include <ankerl/unordered_dense.h>

#include <variant>

namespace angel_lsp::analysis
{
    namespace rules
    {
        struct RuleIndex;
    }

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

    /**
     * @brief Syntactic placement of declaration modifiers and attributes.
     */
    enum class ModifierPlacement : uint8_t
    {
        None = 0,
        Prefix,   ///< Placed before type (e.g., 'final class Foo', 'abstract void Bar()')
        Trailing  ///< Placed after parameter list (e.g., 'void Bar() final', 'void Bar() override')
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

        [[nodiscard]] ModifierPlacement GetFinalPlacement() const noexcept
        {
            if (isDeclarationFinal) return ModifierPlacement::Prefix;
            if (isFinal) return ModifierPlacement::Trailing;
            return ModifierPlacement::None;
        }

        [[nodiscard]] ModifierPlacement GetAbstractPlacement() const noexcept
        {
            if (isDeclarationAbstract) return ModifierPlacement::Prefix;
            if (isAbstract) return ModifierPlacement::Trailing;
            return ModifierPlacement::None;
        }

        void SetFinalPlacement(ModifierPlacement placement) noexcept
        {
            isFinal = (placement != ModifierPlacement::None);
            isDeclarationFinal = (placement == ModifierPlacement::Prefix);
        }

        void SetAbstractPlacement(ModifierPlacement placement) noexcept
        {
            isAbstract = (placement != ModifierPlacement::None);
            isDeclarationAbstract = (placement == ModifierPlacement::Prefix);
        }
    };

    struct ParameterInformation
    {
        ParameterInformation() = default;
        ParameterInformation(std::string name_, std::string typeName_, std::string rawText_ = "", std::string baseTypeName_ = "")
            : name(std::move(name_)), typeName(std::move(typeName_)), rawText(std::move(rawText_)), baseTypeName(std::move(baseTypeName_)) {}

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
        bool isImported = false;
        std::string originModule;
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

        /**
         * @brief The shape of initializer list this type accepts, from its `@listpattern` tag.
         *
         * Held as written - `{repeat T}`, `{repeat {string, ?}}` - and parsed on use. Empty for the
         * overwhelming majority of types, which is the answer "this stub does not say", not "no
         * list is accepted": see analysis/ListPattern.h for why a stub is the only thing that can
         * say and why the rules stay silent when it does not.
         */
        std::string listPattern;
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
        uint32_t baseTypeStartLine = 0;
        uint32_t baseTypeStartCharacter = 0;
        uint32_t baseTypeEndLine = 0;
        uint32_t baseTypeEndCharacter = 0;
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
        uint32_t returnTypeStartLine = 0;
        uint32_t returnTypeStartCharacter = 0;
        uint32_t returnTypeEndLine = 0;
        uint32_t returnTypeEndCharacter = 0;
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

    using SymbolKind = SymbolType;

    class SymbolTable
    {
    public:
        SymbolTable() = default;
        ~SymbolTable() = default;

        void AddSymbol(const Symbol &symbol);
        void InsertSymbol(const std::string &name, SymbolKind kind, const std::string &type = "");
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
        std::optional<Symbol> LookupSymbol(const std::string &name) const;

        /** @brief Returns a copy of all symbols currently present in the table. */
        std::vector<Symbol> GetAllSymbols() const;

        /** @brief Iterates all symbols in the table.
         *  @param visitor Callback invoked for each (qualifiedName, symbol_list) pair.
         *  @note The buckets are snapshotted under the lock and visited outside it, so a visitor
         *        may safely look other symbols up - see the implementation for why that matters. */
        void ForEachSymbol(const std::function<void(const std::string &, const std::vector<Symbol> &)> &visitor) const;

        /**
         * @brief Iterates only the buckets holding at least one symbol from one document.
         *
         * The reason this exists: analysis runs per document, but the table is workspace-wide. A
         * pass that walks everything and then discards by fileUri pays for all 50,000 symbols in
         * the workspace to diagnose a forty-line file, on every debounced keystroke. A visitor
         * still sees the whole bucket, which is what the redeclaration rule needs - the same name
         * declared in a sibling file has to be visible for the comparison to mean anything.
         *
         * @param fileUri Document whose buckets to visit.
         * @param visitor Callback invoked for each (qualifiedName, symbol_list) pair.
         */
        void ForEachSymbolInFile(const std::string &fileUri,
                                 const std::function<void(const std::string &, const std::vector<Symbol> &)> &visitor) const;

        /** @brief Counter bumped on every mutation, so a derived index can tell it is still current. */
        uint64_t Version() const;

        /**
         * @brief The declaration rules' member index, rebuilt only when the table has changed.
         *
         * Hosted here rather than on the analysis request because the answer depends on the table
         * and nothing else: built per request it was one full walk per keystroke, and the table
         * usually has not changed between two of them.
         */
        std::shared_ptr<const rules::RuleIndex> GetRuleIndex() const;

        void PrintSymbols(angel_lsp::utils::LspLogger *logger) const;

    private:
        /** @brief Records that a symbol's bucket now holds something from its file. Caller holds the write lock. */
        void IndexKeyForFileLocked(const std::string &fileUri, const std::string &key);

        /** @brief Erases every symbol belonging to fileUri, touching only that file's buckets. Caller holds the write lock. */
        void EraseDocumentLocked(const std::string &fileUri);

        struct TransparentStringHash
        {
            using is_transparent = void;

            [[nodiscard]] uint64_t operator()(std::string_view sv) const noexcept
            {
                return ankerl::unordered_dense::hash<std::string_view>{}(sv);
            }
            [[nodiscard]] uint64_t operator()(const std::string &s) const noexcept
            {
                return ankerl::unordered_dense::hash<std::string_view>{}(s);
            }
        };

        mutable std::shared_mutex m_mutex;
        ankerl::unordered_dense::map<std::string, std::shared_ptr<std::vector<Symbol>>, TransparentStringHash, std::equal_to<>> m_symbols;

        /** @brief Bucket keys touched by each document, so a per-file walk need not scan the rest. */
        ankerl::unordered_dense::map<std::string, std::vector<std::string>> m_keysByFile;

        uint64_t m_version = 0;

        // Guarded separately from m_mutex: building the index reads the table, so holding the
        // table's lock across the build would be a lock taken twice by one thread.
        mutable std::mutex m_ruleIndexMutex;
        mutable std::shared_ptr<const rules::RuleIndex> m_ruleIndex;
        mutable uint64_t m_ruleIndexVersion = 0;
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