#pragma once
#include <string>
#include <vector>
#include <memory>
#include <lsp/types.h>

namespace analysis
{
    /**
     * @brief Represents the kind of an AngelScript symbol.
     */
    enum class SymbolKind
    {
        Unknown,
        Variable,
        Function,
        Class,
        Namespace,
        Parameter,
        Property,
        Method,
        Enum,
        EnumMember,
        Interface,
        Funcdef,
        Mixin,
        Typedef,
        Constructor,
        Destructor
    };

    /**
     * @brief Represents a parameter for functions, methods, or funcdefs.
     */
    struct SymbolParam
    {
        std::string typeName;     // "int", "Player@", "const float &in"
        std::string name;         // "amount", "target", ""
        std::string defaultValue; // "0.0f", "true", ""
    };

    /**
     * @brief A generic symbol in the AngelScript AST.
     *
     * Stores contextual information, ranges, and hierarchical relationships
     * used for Hover, Go To Definition, and Signature Help.
     */
    struct Symbol
    {
        std::string name;
        SymbolKind kind = SymbolKind::Unknown;

        // Contextual information
        std::string typeInfo;   // e.g. "int", "Player@"
        std::string docComment; // Extracted documentation if any
        std::string value;      // E.g., enum member value, constant value
        std::string templateParam;
        std::string templateType; // E.g., "T", "TValue"
        std::string accessors;    // "{ get const; set; }"

        bool isConstMethod = false;
        bool isAbstract = false;
        bool isShared = false;
        bool isPrivate = false;
        bool isProtected = false;
        bool isFinal = false;
        bool isOverride = false;
        bool isPropertyFunc = false;
        bool isExplicit = false;
        bool isDeleted = false;

        std::vector<SymbolParam> params;      // For functions/methods/funcdefs
        std::vector<std::string> baseClasses; // For inheritance and mixins

        // Where is it defined in the AST?
        std::string uri; // The document URI where this symbol is defined
        lsp::Range selectionRange; // The identifier range
        lsp::Range fullRange;      // The full declaration range

        // For nested scopes/symbols (e.g. methods inside classes, locals inside functions)
        std::vector<std::shared_ptr<Symbol>> children;
        Symbol *parent = nullptr; // Weak reference to parent

        /**
         * @brief Builds the signature string dynamically from the symbol's fields.
         *
         * @param includeParentClass If true, prepends the parent class name (e.g. "ClassName::").
         * @return The formatted signature string.
         */
        std::string BuildSignature(bool includeParentClass = false, const std::string &nameOverride = "") const
        {
            std::string sig;

            // Prefixes
            if (isPrivate)
                sig += "private ";
            if (isProtected)
                sig += "protected ";
            if (isShared)
                sig += "shared ";
            if (isAbstract)
                sig += "abstract ";
            if (isExplicit)
                sig += "explicit ";
            if (isFinal && (kind == SymbolKind::Class || kind == SymbolKind::Interface || kind == SymbolKind::Mixin))
                sig += "final ";

            if (kind == SymbolKind::Class)
                sig += "class ";
            else if (kind == SymbolKind::Interface)
                sig += "interface ";
            else if (kind == SymbolKind::Mixin)
                sig += "mixin ";
            else if (kind == SymbolKind::Enum)
                sig += "enum ";
            else if (kind == SymbolKind::Namespace)
                sig += "namespace ";

            // Return type (for functions/methods) or base type (for typedefs/variables)
            bool isFuncLike = (kind == SymbolKind::Function || kind == SymbolKind::Method || kind == SymbolKind::Funcdef);
            bool isConstructorLike = (kind == SymbolKind::Constructor || kind == SymbolKind::Destructor);

            if (isConstMethod && !isFuncLike)
                sig += "const ";

            if (kind == SymbolKind::Typedef)
            {
                sig += "typedef " + typeInfo + " ";
            }
            else if (!isConstructorLike && !typeInfo.empty())
            {
                sig += typeInfo + " ";
            }

            std::string finalName = nameOverride.empty() ? name : nameOverride;

            // Qualified name
            if (includeParentClass && parent)
                sig += parent->name + "::";
            sig += finalName;

            // Parameters for functions/methods/funcdefs/constructors
            if (isFuncLike || isConstructorLike)
            {
                sig += "(";
                for (size_t i = 0; i < params.size(); ++i)
                {
                    sig += params[i].typeName;
                    if (!params[i].name.empty())
                        sig += " " + params[i].name;
                    if (!params[i].defaultValue.empty())
                        sig += " = " + params[i].defaultValue;
                    if (i + 1 < params.size())
                        sig += ", ";
                }
                sig += ")";

                // Suffixes
                if (isConstMethod)
                    sig += " const";
                if (isOverride)
                    sig += " override";
                if (isFinal)
                    sig += " final";
                if (isPropertyFunc)
                    sig += " property";
                if (isDeleted)
                    sig += " delete";
            }
            else if (kind == SymbolKind::Class || kind == SymbolKind::Interface || kind == SymbolKind::Mixin)
            {
                if (!baseClasses.empty())
                {
                    sig += " : ";
                    for (size_t i = 0; i < baseClasses.size(); ++i)
                    {
                        if (i > 0)
                            sig += ", ";
                        sig += baseClasses[i];
                    }
                }
            }

            if (!accessors.empty())
            {
                sig += " " + accessors;
            }

            return sig;
        }
    };

    /**
     * @brief Converts an internal SymbolKind to the LSP protocol SymbolKind.
     *
     * @param kind The internal symbol kind.
     * @return The corresponding LSP SymbolKind.
     */
    inline lsp::SymbolKind ToLspSymbolKind(SymbolKind kind)
    {
        switch (kind)
        {
        case SymbolKind::Variable:
            return lsp::SymbolKind::Variable;
        case SymbolKind::Function:
            return lsp::SymbolKind::Function;
        case SymbolKind::Class:
            return lsp::SymbolKind::Class;
        case SymbolKind::Namespace:
            return lsp::SymbolKind::Namespace;
        case SymbolKind::Parameter:
            return lsp::SymbolKind::Variable;
        case SymbolKind::Property:
            return lsp::SymbolKind::Property;
        case SymbolKind::Method:
            return lsp::SymbolKind::Method;
        case SymbolKind::Enum:
            return lsp::SymbolKind::Enum;
        case SymbolKind::EnumMember:
            return lsp::SymbolKind::EnumMember;
        case SymbolKind::Interface:
            return lsp::SymbolKind::Interface;
        case SymbolKind::Funcdef:
            return lsp::SymbolKind::Function;
        case SymbolKind::Mixin:
            return lsp::SymbolKind::Class;
        default:
            return lsp::SymbolKind::Null;
        }
    }
}
