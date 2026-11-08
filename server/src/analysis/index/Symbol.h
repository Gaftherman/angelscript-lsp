#pragma once

#include "utils/Constants.h"

#include <ankerl/unordered_dense.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tree_sitter/api.h>
#include <variant>
#include <vector>

namespace angel_lsp::analysis
{
enum class SymbolType : uint8_t
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

enum class AccessModifier : uint8_t
{
    Public,
    Private,
    Protected
};

enum class ParameterModifier : uint8_t
{
    None,
    In,
    Out,
    InOut
};

enum class TypeKind : uint8_t
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
 * @brief Checks if a TypeKind is a primitive value-only type (cannot be null or handle).
 * @param[in] kind The TypeKind to inspect.
 * @return True if the type is a non-nullable primitive value type; false otherwise.
 */
[[nodiscard]] constexpr bool IsValueOnlyKind(TypeKind kind) noexcept
{
    switch (kind)
    {
    case TypeKind::Int8:
    case TypeKind::Int16:
    case TypeKind::Int32:
    case TypeKind::Int64:
    case TypeKind::UInt8:
    case TypeKind::UInt16:
    case TypeKind::UInt32:
    case TypeKind::UInt64:
    case TypeKind::Float:
    case TypeKind::Double:
    case TypeKind::Bool:
        return true;
    default:
        return false;
    }
}

/**
 * @brief Alias for IsValueOnlyKind indicating unconditionally non-nullable primitive types.
 * @param[in] kind The TypeKind to inspect.
 * @return True if non-nullable primitive; false otherwise.
 */
[[nodiscard]] constexpr bool IsUnconditionallyNonNullablePrimitive(TypeKind kind) noexcept
{
    return IsValueOnlyKind(kind);
}

/**
 * @brief Classification categories for enclosing lexical containers.
 */
enum class ContainerKind : uint8_t
{
    None = 0,
    Class,
    Interface,
    Namespace,
    Enum
};

/**
 * @brief Syntactic placement of declaration modifiers and attributes.
 */
enum class ModifierPlacement : uint8_t
{
    None = 0,
    Prefix,  ///< Placed before type (e.g., 'final class Foo', 'abstract void Bar()')
    Trailing ///< Placed after parameter list (e.g., 'void Bar() final', 'void Bar() override')
};

/**
 * @brief Canonical representation of AngelScript type qualifiers and reference modifiers.
 */
struct TypeQualifiers
{
    bool isConst : 1 = false;
    bool isHandle : 1 = false;
    bool isConstHandle : 1 = false;
    bool isReference : 1 = false;
    bool isOutRef : 1 = false;
    bool isInOutRef : 1 = false;
    bool isStandaloneRef : 1 = false;
    bool isAuto : 1 = false;
};
static_assert(sizeof(TypeQualifiers) == 1);

/**
 * @brief Declaration modifiers and syntactic attributes attached to symbols.
 */
struct SymbolModifiers
{
    AccessModifier access = AccessModifier::Public;
    ParameterModifier paramModifier = ParameterModifier::None;
    bool isConst : 1 = false;
    bool isHandle : 1 = false;
    bool isShared : 1 = false;
    bool isMixin : 1 = false;
    bool isAbstract : 1 = false;
    bool isFinal : 1 = false;
    bool isDeclarationFinal : 1 =
        false; ///< True when 'final' is a declaration_modifier (before type), not a func_attribute
    bool isDeclarationAbstract : 1 = false; ///< True when 'abstract' is a declaration_modifier (before type)
    bool isOverride : 1 = false;
    bool isExplicit : 1 = false;
    bool isProperty : 1 = false;
    bool isDelete : 1 = false;
    bool isExternal : 1 = false;
    bool isReturnReference : 1 = false;

    [[nodiscard]] ModifierPlacement GetFinalPlacement() const noexcept
    {
        if (isDeclarationFinal)
            return ModifierPlacement::Prefix;
        if (isFinal)
            return ModifierPlacement::Trailing;
        return ModifierPlacement::None;
    }

    [[nodiscard]] ModifierPlacement GetAbstractPlacement() const noexcept
    {
        if (isDeclarationAbstract)
            return ModifierPlacement::Prefix;
        if (isAbstract)
            return ModifierPlacement::Trailing;
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
static_assert(sizeof(SymbolModifiers) <= 4);

/**
 * @brief Signature metadata and semantic type qualifiers for a function parameter.
 */
struct ParameterInformation
{
    ParameterInformation() = default;
    ParameterInformation(std::string name_, std::string typeName_, std::string baseTypeName_ = "")
        : name(std::move(name_)), typeName(std::move(typeName_)), baseTypeName(std::move(baseTypeName_))
    {
    }

    std::string name;
    std::string typeName;
    std::string baseTypeName;
    std::string templateName;
    std::string defaultValue;

    uint32_t arrayDepth = 0;
    uint32_t startLine = 0;
    uint32_t startCharacter = 0;
    uint32_t endLine = 0;
    uint32_t endCharacter = 0;
    uint32_t nameStartLine = 0;
    uint32_t nameStartCharacter = 0;
    uint32_t nameEndLine = 0;
    uint32_t nameEndCharacter = 0;

    TypeKind typeKind = TypeKind::Unknown;
    ParameterModifier modifier = ParameterModifier::None;

    bool isArray : 1 = false;
    bool hasPrimitiveHandle : 1 = false;
    bool isHandle : 1 = false;
    bool isConst : 1 = false;
    bool isReference : 1 = false;
    bool hasDoubleReference : 1 = false;
    bool isStandaloneRef : 1 = false;
    bool isOutRef : 1 = false;
    bool isInOutRef : 1 = false;
};
static_assert(sizeof(ParameterInformation) <= 200);

struct SourceRange
{
    uint32_t startLine = 0;
    uint32_t startCharacter = 0;
    uint32_t endLine = 0;
    uint32_t endCharacter = 0;

    bool operator==(const SourceRange& other) const noexcept = default;
};

/**
 * @brief Hash functor for SourceRange.
 */
struct SourceRangeHash
{
    size_t operator()(const SourceRange& k) const noexcept
    {
        size_t h = std::hash<uint32_t>{}(k.startLine);
        h ^= std::hash<uint32_t>{}(k.startCharacter) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<uint32_t>{}(k.endLine) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<uint32_t>{}(k.endCharacter) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

/**
 * @brief Constructs a SourceRange from a Tree-Sitter AST node.
 * @param[in] node AST node.
 * @return SourceRange spanning start and end points of the node.
 */
[[nodiscard]] inline SourceRange ToSourceRange(TSNode node) noexcept
{
    if (ts_node_is_null(node))
    {
        return {};
    }
    const TSPoint start = ts_node_start_point(node);
    const TSPoint end = ts_node_end_point(node);
    return SourceRange{start.row, start.column, end.row, end.column};
}

/**
 * @brief Converts a possibly-null Tree-Sitter node into the range a diagnostic targets.
 * @param[in] node AST node, which may be null when an optional child is absent.
 * @return The node's SourceRange, or std::nullopt for a null node so the caller emits nothing.
 */
[[nodiscard]] inline std::optional<SourceRange> ToOptionalSourceRange(TSNode node) noexcept
{
    if (ts_node_is_null(node))
    {
        return std::nullopt;
    }
    return ToSourceRange(node);
}

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
    std::vector<std::string> templateArgumentTypes;
};

/**
 * @brief Signature metadata and parameter list for callable symbols.
 */
struct FunctionSignature
{
    std::string returnType;
    std::string returnBaseTypeName;
    std::string returnTemplateName;
    std::string originModule;
    std::string defaultValue;
    std::string displaySignature; ///< Canonical pre-formatted declaration for zero-copy std::string_view diagnostics.
    std::vector<ParameterInformation> parameters;

    uint32_t returnArrayDepth = 0;
    SymbolModifiers modifiers;
    TypeKind returnTypeKind = TypeKind::Unknown;

    bool returnIsArray : 1 = false;
    bool returnIsConst : 1 = false;
    bool returnHasPrimitiveHandle : 1 = false;
    bool hasBody : 1 = false;
    bool isInterfaceMethod : 1 = false;
    bool isImported : 1 = false;
};
static_assert(sizeof(FunctionSignature) <= 240);

/**
 * @brief Signature and accessor metadata for variables and virtual properties.
 */
struct VariableSignature
{
    std::string typeName;
    std::string baseTypeName;
    std::string templateName;
    std::string defaultValue;
    std::vector<std::string> templateArgumentTypes;

    SourceRange typeRange{};

    uint32_t arrayDepth = 0;
    SymbolModifiers modifiers;
    TypeKind typeKind = TypeKind::Unknown;

    bool isArray : 1 = false;
    bool hasPrimitiveHandle : 1 = false;
    bool isVirtualProperty : 1 = false;
    bool hasGet : 1 = false;
    bool hasSet : 1 = false;
    bool isGetConst : 1 = false;
    bool hasBodyGet : 1 = false;
    bool hasBodySet : 1 = false;
    bool hasDuplicateGet : 1 = false;
    bool hasDuplicateSet : 1 = false;
    bool isGetOverride : 1 = false;
    bool isSetOverride : 1 = false;
    bool isGetFinal : 1 = false;
    bool isSetFinal : 1 = false;
    bool hasNullInitializer : 1 = false;
    bool hasSemicolon : 1 = true;
    bool isLocal : 1 = false;
    bool isEnumConstant : 1 = false;
    bool hasCallInitializer : 1 = false;
};
static_assert(sizeof(VariableSignature) <= 192);

struct EnumMemberInformation
{
    std::string name;
    std::string value;
    std::string valueNodeType;
};

/**
 * @brief Enum declaration metadata and member constant listings.
 */
struct EnumSignature
{
    std::vector<EnumMemberInformation> members;
    SymbolModifiers modifiers;
    bool hasBraces : 1 = false;
};

/**
 * @brief Class inheritance, included mixins, and template parameter metadata.
 */
struct ClassSignature
{
    std::vector<std::string> bases;
    std::vector<std::string> includedMixins;
    std::vector<std::string> templateParams;
    SymbolModifiers modifiers;
    bool isTemplate : 1 = false;
    bool hasBraces : 1 = false;
    bool hasListPattern : 1 = false;
};
static_assert(sizeof(ClassSignature) <= 80);

/**
 * @brief Interface inheritance and modifier metadata.
 */
struct InterfaceSignature
{
    std::vector<std::string> inheritedInterfaces;
    SymbolModifiers modifiers;
};

/**
 * @brief Typedef alias declaration metadata and source position ranges.
 */
struct TypedefSignature
{
    std::string baseType;
    uint32_t baseTypeStartLine = 0;
    uint32_t baseTypeStartCharacter = 0;
    uint32_t baseTypeEndLine = 0;
    uint32_t baseTypeEndCharacter = 0;
    TypeKind typeKind = TypeKind::Unknown;
    bool hasSemicolon : 1 = true;
};

/**
 * @brief Function definition pointer signature and parameter metadata.
 */
struct FuncdefSignature
{
    std::string returnType;
    std::string returnBaseTypeName;
    std::string returnTemplateName;
    std::vector<ParameterInformation> parameters;

    uint32_t returnArrayDepth = 0;
    uint32_t returnTypeStartLine = 0;
    uint32_t returnTypeStartCharacter = 0;
    uint32_t returnTypeEndLine = 0;
    uint32_t returnTypeEndCharacter = 0;
    SymbolModifiers modifiers;
    TypeKind returnTypeKind = TypeKind::Unknown;

    bool returnIsArray : 1 = false;
    bool returnIsConst : 1 = false;
    bool returnHasPrimitiveHandle : 1 = false;
};
static_assert(sizeof(FuncdefSignature) <= 152);

/**
 * @brief Describes a function/method call that occurs outside of any function body
 *        (e.g. a global variable initializer, a class field initializer, or an enum value).
 */
struct CallReferenceSignature
{
    std::string calleeName;
    std::string objectExpression;
    bool isMethodCall : 1 = false;
};

/**
 * @brief Core symbol descriptor containing name, scope, container, and polymorphic signature variant.
 */
struct Symbol
{
    std::string name;
    std::string containerName;
    std::string qualifiedName;
    std::string fileUri;
    std::string virtualFileUri;        ///< Synthetic URI when virtual mixin documents are enabled (e.g.
                                       ///< angelscript-virtual://<host_class>/<mixin>.as).
    std::vector<std::string> metadata; ///< CScriptBuilder metadata strings attached to this symbol.

    std::variant<std::monostate, FunctionSignature, VariableSignature, EnumSignature, ClassSignature,
                 InterfaceSignature, TypedefSignature, FuncdefSignature, CallReferenceSignature>
        signature;

    SourceRange fullRange;      ///< Full enclosing source range of the declaration (for DocumentSymbol).
    SourceRange selectionRange; ///< Source range of the identifier token itself (for DocumentSymbol/Rename).

    uint32_t startLine = 0;
    uint32_t startCharacter = 0;
    uint32_t endLine = 0;
    uint32_t endCharacter = 0;

    SymbolType type = SymbolType::Variable;
    ContainerKind containerKind = ContainerKind::None;
    bool isSynthesized : 1 = false; ///< True when synthesized into a host class (e.g. from an included mixin).
    bool isPredefined : 1 = false;  ///< True when parsed from a predefined stub or builtin profile.

    FunctionSignature& GetFunction() noexcept
    {
        return std::get<FunctionSignature>(signature);
    }

    const FunctionSignature& GetFunction() const noexcept
    {
        return std::get<FunctionSignature>(signature);
    }

    VariableSignature& GetVariable() noexcept
    {
        return std::get<VariableSignature>(signature);
    }

    const VariableSignature& GetVariable() const noexcept
    {
        return std::get<VariableSignature>(signature);
    }

    EnumSignature& GetEnum() noexcept
    {
        return std::get<EnumSignature>(signature);
    }

    const EnumSignature& GetEnum() const noexcept
    {
        return std::get<EnumSignature>(signature);
    }

    ClassSignature& GetClass() noexcept
    {
        return std::get<ClassSignature>(signature);
    }

    const ClassSignature& GetClass() const noexcept
    {
        return std::get<ClassSignature>(signature);
    }

    InterfaceSignature& GetInterface() noexcept
    {
        return std::get<InterfaceSignature>(signature);
    }

    const InterfaceSignature& GetInterface() const noexcept
    {
        return std::get<InterfaceSignature>(signature);
    }

    TypedefSignature& GetTypedef() noexcept
    {
        return std::get<TypedefSignature>(signature);
    }

    const TypedefSignature& GetTypedef() const noexcept
    {
        return std::get<TypedefSignature>(signature);
    }

    FuncdefSignature& GetFuncdef() noexcept
    {
        return std::get<FuncdefSignature>(signature);
    }

    const FuncdefSignature& GetFuncdef() const noexcept
    {
        return std::get<FuncdefSignature>(signature);
    }

    CallReferenceSignature& GetCallReference() noexcept
    {
        return std::get<CallReferenceSignature>(signature);
    }

    const CallReferenceSignature& GetCallReference() const noexcept
    {
        return std::get<CallReferenceSignature>(signature);
    }

    /**
     * @brief Returns a zero-copy string_view of the symbol name.
     */
    [[nodiscard]] std::string_view GetName() const noexcept
    {
        return name;
    }

    /**
     * @brief Returns a zero-copy string_view of the container scope name.
     */
    [[nodiscard]] std::string_view GetContainerName() const noexcept
    {
        return containerName;
    }

    /**
     * @brief Returns a zero-copy string_view of the fully qualified name.
     */
    [[nodiscard]] std::string_view GetQualifiedName() const noexcept
    {
        return qualifiedName;
    }

    /**
     * @brief Returns a zero-copy string_view of the file URI.
     */
    [[nodiscard]] std::string_view GetFileUri() const noexcept
    {
        return fileUri;
    }

    /**
     * @brief Returns a zero-copy string_view of the canonical pre-formatted function declaration.
     * @return Pre-formatted signature string_view if populated, or symbol name as fallback.
     */
    [[nodiscard]] std::string_view GetDisplaySignature() const noexcept
    {
        if (type == SymbolType::Function && std::holds_alternative<FunctionSignature>(signature))
        {
            const auto& sig = std::get<FunctionSignature>(signature);
            if (!sig.displaySignature.empty())
            {
                return sig.displaySignature;
            }
        }
        return qualifiedName.empty() ? std::string_view(name) : std::string_view(qualifiedName);
    }
};
static_assert(sizeof(Symbol) <= 504);

/**
 * @brief Extracts all base classes or inherited interfaces declared on a symbol without heap allocation.
 * @param[in] sym Symbol representing a class or interface.
 * @return Span of declared base/interface name strings.
 */
[[nodiscard]] inline std::span<const std::string> GetDeclaredBases(const Symbol& sym) noexcept
{
    if (sym.type == SymbolType::Class && std::holds_alternative<ClassSignature>(sym.signature))
    {
        return sym.GetClass().bases;
    }
    if (sym.type == SymbolType::Interface && std::holds_alternative<InterfaceSignature>(sym.signature))
    {
        return sym.GetInterface().inheritedInterfaces;
    }
    return {};
}

using SymbolKind = SymbolType;

} // namespace angel_lsp::analysis
