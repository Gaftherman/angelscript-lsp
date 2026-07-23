/**
 * @file ClassValidator.cpp
 * @brief Implementation of ClassValidator for classes, interfaces, mixins, and virtual properties.
 * @ingroup Analysis
 */

#include "ClassValidator.h"
#include <spdlog/fmt/fmt.h>
#include <ankerl/unordered_dense.h>

namespace analysis::validators
{
    std::vector<lsp::Diagnostic> ClassValidator::ValidateClass(
        TSNode node,
        const Document &doc,
        const SymbolTable &globalTable,
        const SymbolTable &localTable,
        i18n::Locale locale)
    {
        std::vector<lsp::Diagnostic> diags;
        if (ts_node_is_null(node))
        {
            return diags;
        }

        const auto &strs = i18n::GetStrings(locale);

        SymbolTable combined = localTable;
        combined.MergeGlobals(globalTable);

        // 1. Extract class name
        TSNode nameNode = ts_node_child_by_field_name(node, "name", sizeof("name") - 1);
        if (ts_node_is_null(nameNode))
        {
            uint32_t count = ts_node_child_count(node);
            for (uint32_t i = 0; i < count; ++i)
            {
                TSNode child = ts_node_child(node, i);
                const char *cType = ts_node_type(child);
                if (cType && std::string(cType) == "identifier")
                {
                    nameNode = child;
                    break;
                }
            }
        }

        std::string className;
        if (!ts_node_is_null(nameNode))
        {
            className = std::string(doc.SourceAt(nameNode));
            std::vector<Symbol *> globals = combined.FindAllGlobalsByName(className);
            size_t classCount = 0;
            for (const auto *sym : globals)
            {
                if (sym->kind == SymbolKind::Class || sym->kind == SymbolKind::Interface)
                {
                    classCount++;
                }
            }
            if (classCount > 1)
            {
                TSPoint start = ts_node_start_point(nameNode);
                TSPoint end = ts_node_end_point(nameNode);

                lsp::Diagnostic d;
                d.range.start.line = start.row;
                d.range.start.character = start.column;
                d.range.end.line = end.row;
                d.range.end.character = end.column;
                d.severity = lsp::DiagnosticSeverity::Error;
                d.source = "angelscript";
                d.message = fmt::format(fmt::runtime(strs.diagDuplicateClassName), className);
                diags.push_back(d);
            }
        }

        // 2. Check modifiers (abstract, final)
        bool isAbstractClass = false;
        uint32_t topChildCount = ts_node_child_count(node);
        for (uint32_t i = 0; i < topChildCount; ++i)
        {
            TSNode child = ts_node_child(node, i);
            std::string_view text = doc.SourceAt(child);
            if (text == "abstract")
            {
                isAbstractClass = true;
                break;
            }
        }

        // 3. Process base classes and interfaces
        size_t concreteBaseClassCount = 0;
        std::vector<const Symbol *> baseSymbols;

        for (uint32_t i = 0; i < topChildCount; ++i)
        {
            TSNode child = ts_node_child(node, i);
            const char *cType = ts_node_type(child);
            if (cType && std::string(cType) == "base_class_list")
            {
                uint32_t bCount = ts_node_child_count(child);
                for (uint32_t j = 0; j < bCount; ++j)
                {
                    TSNode bChild = ts_node_child(child, j);
                    std::string_view bText = doc.SourceAt(bChild);
                    if (bText != ":" && bText != ",")
                    {
                        std::string baseName = std::string(bText);
                        std::string_view shortName = baseName;
                        size_t pos = baseName.rfind("::");
                        if (pos != std::string_view::npos)
                        {
                            shortName = baseName.substr(pos + 2);
                        }

                        const Symbol *baseSym = combined.FindByNameDeep(baseName);
                        if (!baseSym)
                        {
                            baseSym = combined.FindFirst(shortName);
                        }
                        if (!baseSym)
                        {
                            TSPoint start = ts_node_start_point(bChild);
                            TSPoint end = ts_node_end_point(bChild);

                            lsp::Diagnostic d;
                            d.range.start.line = start.row;
                            d.range.start.character = start.column;
                            d.range.end.line = end.row;
                            d.range.end.character = end.column;
                            d.severity = lsp::DiagnosticSeverity::Error;
                            d.source = "angelscript";
                            d.message = fmt::format(fmt::runtime(strs.diagUndeclaredType), baseName);
                            diags.push_back(d);
                        }
                        else
                        {
                            baseSymbols.push_back(baseSym);

                            if (baseSym->kind == SymbolKind::Class)
                            {
                                concreteBaseClassCount++;
                                if (concreteBaseClassCount > 1)
                                {
                                    TSPoint start = ts_node_start_point(bChild);
                                    TSPoint end = ts_node_end_point(bChild);

                                    lsp::Diagnostic d;
                                    d.range.start.line = start.row;
                                    d.range.start.character = start.column;
                                    d.range.end.line = end.row;
                                    d.range.end.character = end.column;
                                    d.severity = lsp::DiagnosticSeverity::Error;
                                    d.source = "angelscript";
                                    d.message = std::string(strs.diagMultipleClassInheritance);
                                    diags.push_back(d);
                                }

                                if (baseSym->isFinal)
                                {
                                    TSPoint start = ts_node_start_point(bChild);
                                    TSPoint end = ts_node_end_point(bChild);

                                    lsp::Diagnostic d;
                                    d.range.start.line = start.row;
                                    d.range.start.character = start.column;
                                    d.range.end.line = end.row;
                                    d.range.end.character = end.column;
                                    d.severity = lsp::DiagnosticSeverity::Error;
                                    d.source = "angelscript";
                                    d.message = fmt::format(fmt::runtime(strs.diagInheritFinalClass), baseName);
                                    diags.push_back(d);
                                }
                            }
                        }
                    }
                }
            }
        }

        // 4. Collect class body methods
        TSNode bodyNode = ts_node_child_by_field_name(node, "body", sizeof("body") - 1);
        if (ts_node_is_null(bodyNode))
        {
            for (uint32_t i = 0; i < topChildCount; ++i)
            {
                TSNode child = ts_node_child(node, i);
                std::string_view text = doc.SourceAt(child);
                if (text.starts_with("{"))
                {
                    bodyNode = child;
                    break;
                }
            }
        }

        ankerl::unordered_dense::set<std::string> classMethodNames;
        if (!ts_node_is_null(bodyNode))
        {
            uint32_t bCount = ts_node_child_count(bodyNode);
            for (uint32_t i = 0; i < bCount; ++i)
            {
                TSNode mChild = ts_node_child(bodyNode, i);
                const char *cType = ts_node_type(mChild);
                if (cType && (std::string(cType) == "func_declaration" || std::string(cType) == "func"))
                {
                    TSNode mNameNode = ts_node_child_by_field_name(mChild, "name", sizeof("name") - 1);
                    if (ts_node_is_null(mNameNode))
                    {
                        uint32_t subCount = ts_node_child_count(mChild);
                        for (uint32_t j = 0; j < subCount; ++j)
                        {
                            TSNode sub = ts_node_child(mChild, j);
                            if (std::string_view(ts_node_type(sub)) == "identifier")
                            {
                                mNameNode = sub;
                                break;
                            }
                        }
                    }

                    if (!ts_node_is_null(mNameNode))
                    {
                        std::string mName = std::string(doc.SourceAt(mNameNode));
                        classMethodNames.insert(mName);

                        // Check method override and final rules
                        bool hasOverride = false;
                        uint32_t subCount = ts_node_child_count(mChild);
                        for (uint32_t j = 0; j < subCount; ++j)
                        {
                            if (doc.SourceAt(ts_node_child(mChild, j)) == "override")
                            {
                                hasOverride = true;
                                break;
                            }
                        }

                        // Search base class / interfaces for matching method
                        bool foundInBase = false;
                        bool baseIsFinal = false;
                        for (const auto *baseSym : baseSymbols)
                        {
                            for (const auto &bChildSym : baseSym->children)
                            {
                                if (bChildSym->name == mName && (bChildSym->kind == SymbolKind::Method || bChildSym->kind == SymbolKind::Function))
                                {
                                    foundInBase = true;
                                    if (bChildSym->isFinal)
                                    {
                                        baseIsFinal = true;
                                    }
                                }
                            }
                        }

                        if (hasOverride && !foundInBase)
                        {
                            TSPoint start = ts_node_start_point(mNameNode);
                            TSPoint end = ts_node_end_point(mNameNode);

                            lsp::Diagnostic d;
                            d.range.start.line = start.row;
                            d.range.start.character = start.column;
                            d.range.end.line = end.row;
                            d.range.end.character = end.column;
                            d.severity = lsp::DiagnosticSeverity::Error;
                            d.source = "angelscript";
                            d.message = fmt::format(fmt::runtime(strs.diagOverrideNoMatchingBase), mName);
                            diags.push_back(d);
                        }

                        if (baseIsFinal)
                        {
                            TSPoint start = ts_node_start_point(mNameNode);
                            TSPoint end = ts_node_end_point(mNameNode);

                            lsp::Diagnostic d;
                            d.range.start.line = start.row;
                            d.range.start.character = start.column;
                            d.range.end.line = end.row;
                            d.range.end.character = end.column;
                            d.severity = lsp::DiagnosticSeverity::Error;
                            d.source = "angelscript";
                            d.message = fmt::format(fmt::runtime(strs.diagOverrideFinalMethod), mName);
                            diags.push_back(d);
                        }
                    }
                }
            }
        }

        // 5. Check unimplemented interface methods
        if (!isAbstractClass)
        {
            for (const auto *baseSym : baseSymbols)
            {
                if (baseSym->kind == SymbolKind::Interface)
                {
                    for (const auto &ifaceMethod : baseSym->children)
                    {
                        if (ifaceMethod->kind == SymbolKind::Method || ifaceMethod->kind == SymbolKind::Function)
                        {
                            if (!classMethodNames.contains(ifaceMethod->name))
                            {
                                TSPoint start = ts_node_start_point(node);
                                if (!ts_node_is_null(nameNode))
                                {
                                    start = ts_node_start_point(nameNode);
                                }
                                TSPoint end = ts_node_end_point(node);

                                lsp::Diagnostic d;
                                d.range.start.line = start.row;
                                d.range.start.character = start.column;
                                d.range.end.line = end.row;
                                d.range.end.character = end.column;
                                d.severity = lsp::DiagnosticSeverity::Error;
                                d.source = "angelscript";
                                d.message = fmt::format(fmt::runtime(strs.diagUnimplementedInterfaceMethod), className, ifaceMethod->name);
                                diags.push_back(d);
                            }
                        }
                    }
                }
            }
        }

        return diags;
    }

    std::vector<lsp::Diagnostic> ClassValidator::ValidateInterface(
        TSNode node,
        const Document &doc,
        const SymbolTable &globalTable,
        const SymbolTable &localTable,
        i18n::Locale locale)
    {
        std::vector<lsp::Diagnostic> diags;
        if (ts_node_is_null(node))
        {
            return diags;
        }

        const auto &strs = i18n::GetStrings(locale);

        SymbolTable combined = localTable;
        combined.MergeGlobals(globalTable);

        TSNode nameNode = ts_node_child_by_field_name(node, "name", sizeof("name") - 1);
        if (ts_node_is_null(nameNode))
        {
            uint32_t count = ts_node_child_count(node);
            for (uint32_t i = 0; i < count; ++i)
            {
                TSNode child = ts_node_child(node, i);
                const char *cType = ts_node_type(child);
                if (cType && std::string(cType) == "identifier")
                {
                    nameNode = child;
                    break;
                }
            }
        }

        if (!ts_node_is_null(nameNode))
        {
            std::string_view ifaceName = doc.SourceAt(nameNode);
            std::vector<Symbol *> globals = combined.FindAllGlobalsByName(ifaceName);
            size_t count = 0;
            for (const auto *sym : globals)
            {
                if (sym->kind == SymbolKind::Class || sym->kind == SymbolKind::Interface)
                {
                    count++;
                }
            }
            if (count > 1)
            {
                TSPoint start = ts_node_start_point(nameNode);
                TSPoint end = ts_node_end_point(nameNode);

                lsp::Diagnostic d;
                d.range.start.line = start.row;
                d.range.start.character = start.column;
                d.range.end.line = end.row;
                d.range.end.character = end.column;
                d.severity = lsp::DiagnosticSeverity::Error;
                d.source = "angelscript";
                d.message = fmt::format(fmt::runtime(strs.diagDuplicateClassName), ifaceName);
                diags.push_back(d);
            }
        }

        return diags;
    }

    std::vector<lsp::Diagnostic> ClassValidator::ValidateVirtProp(
        TSNode node,
        const Document &doc,
        const SymbolTable &globalTable,
        const SymbolTable &localTable,
        i18n::Locale locale)
    {
        std::vector<lsp::Diagnostic> diags;
        if (ts_node_is_null(node))
        {
            return diags;
        }

        const auto &strs = i18n::GetStrings(locale);

        // Extract virtprop declared type
        TSNode typeNode = ts_node_child_by_field_name(node, "type", sizeof("type") - 1);
        if (ts_node_is_null(typeNode))
        {
            typeNode = ts_node_child(node, 0);
        }

        if (ts_node_is_null(typeNode))
        {
            return diags;
        }

        std::string_view propTypeStr = doc.SourceAt(typeNode);

        // Traverse get / set accessors inside virtprop
        uint32_t count = ts_node_child_count(node);
        for (uint32_t i = 0; i < count; ++i)
        {
            TSNode child = ts_node_child(node, i);
            const char *cType = ts_node_type(child);
            if (cType && (std::string(cType) == "accessor" || std::string(cType) == "virtual_property_accessor"))
            {
                std::string_view accText = doc.SourceAt(child);
                if (accText.starts_with("get"))
                {
                    // get accessor return check if explicit return present
                    TSNode bodyNode = ts_node_child_by_field_name(child, "body", sizeof("body") - 1);
                    if (!ts_node_is_null(bodyNode))
                    {
                        uint32_t bCount = ts_node_child_count(bodyNode);
                        for (uint32_t j = 0; j < bCount; ++j)
                        {
                            TSNode rNode = ts_node_child(bodyNode, j);
                            if (doc.SourceAt(rNode).starts_with("return"))
                            {
                                uint32_t rChildCount = ts_node_child_count(rNode);
                                for (uint32_t k = 0; k < rChildCount; ++k)
                                {
                                    TSNode valNode = ts_node_child(rNode, k);
                                    std::string_view vText = doc.SourceAt(valNode);
                                    if (vText != "return" && vText != ";")
                                    {
                                        auto inferredOpt = TypeEvaluator::InferType(valNode, doc, globalTable, localTable);
                                        if (inferredOpt.has_value() && !TypeEvaluator::AreTypesCompatible(propTypeStr, inferredOpt.value()))
                                        {
                                            TSPoint start = ts_node_start_point(valNode);
                                            TSPoint end = ts_node_end_point(valNode);

                                            lsp::Diagnostic d;
                                            d.range.start.line = start.row;
                                            d.range.start.character = start.column;
                                            d.range.end.line = end.row;
                                            d.range.end.character = end.column;
                                            d.severity = lsp::DiagnosticSeverity::Error;
                                            d.source = "angelscript";
                                            d.message = fmt::format(fmt::runtime(strs.diagVirtPropTypeMismatch), "get", propTypeStr);
                                            diags.push_back(d);
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        return diags;
    }

    std::vector<lsp::Diagnostic> ClassValidator::ValidateOperatorOverloads(
        TSNode funcNode,
        const Document &doc,
        i18n::Locale locale)
    {
        std::vector<lsp::Diagnostic> diags;
        if (ts_node_is_null(funcNode))
        {
            return diags;
        }

        const auto &strs = i18n::GetStrings(locale);

        TSNode nameNode = ts_node_child_by_field_name(funcNode, "name", sizeof("name") - 1);
        if (ts_node_is_null(nameNode))
        {
            uint32_t count = ts_node_child_count(funcNode);
            for (uint32_t i = 0; i < count; ++i)
            {
                TSNode child = ts_node_child(funcNode, i);
                const char *cType = ts_node_type(child);
                if (cType && std::string(cType) == "identifier")
                {
                    nameNode = child;
                    break;
                }
            }
        }

        if (ts_node_is_null(nameNode))
        {
            return diags;
        }

        std::string name = std::string(doc.SourceAt(nameNode));
        if (!name.starts_with("op"))
        {
            return diags;
        }

        TSNode paramListNode = ts_node_child_by_field_name(funcNode, "parameters", sizeof("parameters") - 1);
        if (ts_node_is_null(paramListNode))
        {
            uint32_t count = ts_node_child_count(funcNode);
            for (uint32_t i = 0; i < count; ++i)
            {
                TSNode child = ts_node_child(funcNode, i);
                const char *cType = ts_node_type(child);
                if (cType && std::string(cType) == "parameter_list")
                {
                    paramListNode = child;
                    break;
                }
            }
        }

        size_t paramCount = 0;
        if (!ts_node_is_null(paramListNode))
        {
            uint32_t count = ts_node_child_count(paramListNode);
            for (uint32_t i = 0; i < count; ++i)
            {
                TSNode child = ts_node_child(paramListNode, i);
                const char *cType = ts_node_type(child);
                if (cType && std::string(cType) == "parameter")
                {
                    paramCount++;
                }
            }
        }

        std::string returnType = "";
        TSNode retTypeNode = ts_node_child_by_field_name(funcNode, "return_type", sizeof("return_type") - 1);
        if (ts_node_is_null(retTypeNode))
        {
            retTypeNode = ts_node_child_by_field_name(funcNode, "type", sizeof("type") - 1);
        }
        if (ts_node_is_null(retTypeNode))
        {
            uint32_t count = ts_node_child_count(funcNode);
            for (uint32_t i = 0; i < count; ++i)
            {
                TSNode child = ts_node_child(funcNode, i);
                const char *cType = ts_node_type(child);
                if (cType && (std::string(cType) == "type" || std::string(cType) == "primitive_type"))
                {
                    retTypeNode = child;
                    break;
                }
            }
        }
        if (!ts_node_is_null(retTypeNode))
        {
            returnType = std::string(doc.SourceAt(retTypeNode));
        }

        bool isValid = true;

        static const ankerl::unordered_dense::set<std::string> binaryOps = {
            "opAdd", "opSub", "opMul", "opDiv", "opMod", "opPow", "opAnd", "opOr", "opXor",
            "opShl", "opShr", "opUShr", "opAddAssign", "opSubAssign", "opMulAssign", "opDivAssign",
            "opModAssign", "opPowAssign", "opAndAssign", "opOrAssign", "opXorAssign", "opShlAssign",
            "opShrAssign", "opUShrAssign"
        };

        static const ankerl::unordered_dense::set<std::string> unaryOps = {
            "opNeg", "opCom", "opPostInc", "opPostDec", "opPreInc", "opPreDec"
        };

        if (binaryOps.contains(name))
        {
            if (paramCount != 1)
            {
                isValid = false;
            }
        }
        else if (unaryOps.contains(name))
        {
            if (paramCount != 0)
            {
                isValid = false;
            }
        }
        else if (name == "opCmp")
        {
            if (paramCount != 1 || (returnType != "int" && !returnType.empty()))
            {
                isValid = false;
            }
        }
        else if (name == "opEquals")
        {
            if (paramCount != 1 || (returnType != "bool" && !returnType.empty()))
            {
                isValid = false;
            }
        }
        else if (name == "opIndex" || name == "opDefaultIndex")
        {
            if (paramCount < 1)
            {
                isValid = false;
            }
        }
        else if (name == "opCast" || name == "opImplCast")
        {
            if (paramCount != 0 || returnType == "void")
            {
                isValid = false;
            }
        }

        if (!isValid)
        {
            TSPoint start = ts_node_start_point(nameNode);
            TSPoint end = ts_node_end_point(nameNode);

            lsp::Diagnostic d;
            d.range.start.line = start.row;
            d.range.start.character = start.column;
            d.range.end.line = end.row;
            d.range.end.character = end.column;
            d.severity = lsp::DiagnosticSeverity::Error;
            d.source = "angelscript";
            d.message = fmt::format(fmt::runtime(strs.diagInvalidOpSignature), name);
            diags.push_back(d);
        }

        return diags;
    }

} // namespace analysis::validators
