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
        std::string typeName; // "int", "Player@", "const float &in"
        std::string name;     // "amount", "target", ""
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
        std::string typeInfo;     // e.g. "int", "Player@"
        std::string signature;    // e.g. "void DoThing(int)"
        std::string docComment;   // Extracted documentation if any
        bool isConstMethod = false;
        bool isAbstract = false;
        bool isShared = false;
        bool isMixin = false;
        
        std::vector<SymbolParam> params; // For functions/methods/funcdefs
        std::vector<std::string> baseClasses; // For inheritance and mixins
        
        // Where is it defined in the AST?
        std::string uri;           // The document URI where this symbol is defined
        size_t definitionStartByte = 0;
        size_t definitionEndByte = 0;
        lsp::Range selectionRange; // The identifier range
        lsp::Range fullRange;      // The full declaration range

        // For nested scopes/symbols (e.g. methods inside classes, locals inside functions)
        std::vector<std::shared_ptr<Symbol>> children;
        Symbol *parent = nullptr; // Weak reference to parent
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
            case SymbolKind::Variable:   return lsp::SymbolKind::Variable;
            case SymbolKind::Function:   return lsp::SymbolKind::Function;
            case SymbolKind::Class:      return lsp::SymbolKind::Class;
            case SymbolKind::Namespace:  return lsp::SymbolKind::Namespace;
            case SymbolKind::Parameter:  return lsp::SymbolKind::Variable;
            case SymbolKind::Property:   return lsp::SymbolKind::Property;
            case SymbolKind::Method:     return lsp::SymbolKind::Method;
            case SymbolKind::Enum:       return lsp::SymbolKind::Enum;
            case SymbolKind::EnumMember: return lsp::SymbolKind::EnumMember;
            case SymbolKind::Interface:  return lsp::SymbolKind::Interface;
            case SymbolKind::Funcdef:    return lsp::SymbolKind::Function;
            case SymbolKind::Mixin:      return lsp::SymbolKind::Class;
            default:                     return lsp::SymbolKind::Null;
        }
    }
}
