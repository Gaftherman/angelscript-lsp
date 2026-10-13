#include "analysis/rules/ClassRules.h"
#include "analysis/ASTUtils.h"
#include "analysis/DiagnosticCodes.h"
#include "analysis/SemanticHelpers.h"
#include "document/Document.h"
#include "parser/AngelScriptParser.h"
#include "spdlog/fmt/fmt.h"
#include "utils/Utils.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <vector>

namespace angel_lsp::analysis::rules
{
namespace
{
/** @brief Checks if a class declaration is missing a body. */
void CheckBodyPresence(const Symbol& sym, const ClassSignature& sig, const DiagnosticContext& ctx)
{
    if (!sig.hasBraces && !sig.modifiers.isExternal)
    {
        bool hasFullDefinition = false;
        if (auto symsPtr = ctx.request.symbolTable.FindSymbolsPtr(sym.name))
        {
            for (const auto& s : *symsPtr)
            {
                if (s.type == SymbolType::Class && s.GetClass().hasBraces)
                {
                    hasFullDefinition = true;
                    break;
                }
            }
        }
        if (!hasFullDefinition)
        {
            ctx.LogRule("CheckClassModifiers", "as-err-declaration-missing-body", sym);
            ctx.Emit(sym, "as-err-declaration-missing-body", sym.name);
        }
    }
}

/** @brief Validates external and shared rules on a class declaration. */
void CheckExternalClass(const Symbol& sym, const ClassSignature& sig, const DiagnosticContext& ctx)
{
    if (!sig.modifiers.isExternal)
    {
        return;
    }

    if (!sig.modifiers.isShared)
    {
        ctx.LogRule("CheckClassModifiers", "as-err-external-not-shared", sym);
        ctx.Emit(sym, "as-err-external-not-shared", sym.name);
        return;
    }

    const bool knowsModules = ctx.request.moduleContext.has_value() && !ctx.request.moduleContext->name.empty();
    bool hasFullSharedDefinition = false;

    if (knowsModules)
    {
        hasFullSharedDefinition = ctx.request.moduleContext->sharedElsewhere.contains(sym.name);
    }
    else if (auto symsPtr = ctx.request.symbolTable.FindSymbolsPtr(sym.name))
    {
        for (const auto& s : *symsPtr)
        {
            if (s.type == SymbolType::Class && s.GetClass().hasBraces && s.GetClass().modifiers.isShared &&
                !s.GetClass().modifiers.isExternal)
            {
                hasFullSharedDefinition = true;
                break;
            }
        }
    }

    if (!hasFullSharedDefinition)
    {
        ctx.LogRule("CheckClassModifiers", "as-err-external-not-found", sym);
        ctx.Emit(sym, "as-err-external-not-found", sym.name);
    }
}

/** @brief Validates mixin-specific modifier restrictions. */
void CheckMixinModifiers(const Symbol& sym, const ClassSignature& sig, const DiagnosticContext& ctx)
{
    if (!sig.modifiers.isMixin)
    {
        return;
    }

    if (sig.modifiers.isFinal)
    {
        ctx.LogRule("CheckClassModifiers", "as-err-mixin-final", sym);
        ctx.Emit(sym, "as-err-mixin-final", sym.name);
    }

    if (sig.modifiers.isAbstract)
    {
        ctx.LogRule("CheckClassModifiers", "as-err-mixin-abstract", sym);
        ctx.Emit(sym, "as-err-mixin-abstract", sym.name);
    }

    if (ctx.request.GetRuleIndex().Members(sym.qualifiedName).hasNestedType)
    {
        ctx.LogRule("CheckClassModifiers", "as-err-mixin-child-type", sym);
        ctx.Emit(sym, "as-err-mixin-child-type", sym.name);
    }
}

/** @brief Modifier and shape rules a class declaration must satisfy on its own. */
void CheckClassModifiers(const Symbol& sym, const ClassSignature& sig, const DiagnosticContext& ctx)
{
    CheckBodyPresence(sym, sig, ctx);
    CheckExternalClass(sym, sig, ctx);
    CheckMixinModifiers(sym, sig, ctx);
}

/**
 * @brief Collects the method names a type actually provides, base *classes* included.
 */
ankerl::unordered_dense::set<std::string>
CollectImplementedMethodNames(const std::string& typeName, const SymbolTable& table, const RuleIndex& index)
{
    ankerl::unordered_dense::set<std::string> names;

    for (const auto& ancestor : GetInheritedTypeHierarchy(typeName, table))
    {
        const auto ancestorSymbols = table.FindSymbolsPtr(ancestor);
        const bool isInterface =
            ancestorSymbols && std::any_of(ancestorSymbols->begin(), ancestorSymbols->end(),
                                           [](const Symbol& sym) { return sym.type == SymbolType::Interface; });
        if (isInterface)
        {
            continue;
        }

        for (const auto& name : index.Members(ancestor).methodNames)
        {
            names.insert(name);
        }
    }
    return names;
}

/** @brief Collects all interfaces that must be checked for implementation. */
void CollectInterfacesToCheck(const std::vector<std::string>& bases, const SymbolTable& table,
                              ankerl::unordered_dense::set<std::string>& interfacesToCheck)
{
    for (const auto& baseName : bases)
    {
        const std::string cleanBase = CleanBaseType(baseName);
        const auto baseSymbols = table.FindSymbolsPtr(cleanBase);
        if (!baseSymbols)
        {
            continue;
        }

        for (const auto& base : *baseSymbols)
        {
            if (base.type == SymbolType::Interface)
            {
                interfacesToCheck.insert(cleanBase);
            }
            else if (base.type == SymbolType::Class && base.GetClass().modifiers.isMixin)
            {
                for (const auto& mixinAncestor : GetInheritedTypeHierarchy(cleanBase, table))
                {
                    const auto mixinAncestorSyms = table.FindSymbolsPtr(mixinAncestor);
                    if (mixinAncestorSyms &&
                        std::any_of(mixinAncestorSyms->begin(), mixinAncestorSyms->end(),
                                    [](const Symbol& s) { return s.type == SymbolType::Interface; }))
                    {
                        interfacesToCheck.insert(mixinAncestor);
                    }
                }
            }
        }
    }
}

/** @brief Reports diagnostics for unimplemented interface methods. */
void ReportMissingInterfaceMethods(const Symbol& sym,
                                   const ankerl::unordered_dense::set<std::string>& interfacesToCheck,
                                   const ankerl::unordered_dense::set<std::string>& implemented,
                                   const DiagnosticContext& ctx)
{
    for (const auto& cleanBase : interfacesToCheck)
    {
        for (const auto& methodName : ctx.request.GetRuleIndex().Members(cleanBase).methodNames)
        {
            if (!implemented.contains(methodName))
            {
                ctx.LogRule("CheckInterfaceImplementation", "as-err-interface-impl-missing", sym);
                ctx.Emit(sym, "as-err-interface-impl-missing", {sym.name, methodName, cleanBase});
            }
        }
    }
}

/**
 * @brief Reports interface methods a class declares nowhere in its hierarchy.
 */
void CheckInterfaceImplementation(const Symbol& sym, const ClassSignature& sig, const DiagnosticContext& ctx)
{
    if (sig.modifiers.isMixin)
    {
        return;
    }

    const SymbolTable& table = ctx.request.symbolTable;
    const std::string container = sym.qualifiedName.empty() ? sym.name : sym.qualifiedName;

    if (!HierarchyIsFullyVisible(container, table))
    {
        return;
    }

    const auto implemented = CollectImplementedMethodNames(container, table, ctx.request.GetRuleIndex());
    ankerl::unordered_dense::set<std::string> interfacesToCheck;
    CollectInterfacesToCheck(sig.bases, table, interfacesToCheck);
    ReportMissingInterfaceMethods(sym, interfacesToCheck, implemented, ctx);
}

/** @brief Reports methods overriding final methods of a base class or its mixins. */
void ReportFinalMethodOverrides(const std::string& container, const std::string& cleanBase,
                                const std::string& reportingBase, const DiagnosticContext& ctx)
{
    const SymbolTable& table = ctx.request.symbolTable;
    for (const auto& methodName : ctx.request.GetRuleIndex().Members(cleanBase).finalMethodNames)
    {
        const std::string derivedName = container.empty() ? methodName : container + "::" + methodName;
        if (auto methodsPtr = table.FindSymbolsPtr(derivedName))
        {
            for (const auto& mSym : *methodsPtr)
            {
                if (mSym.type == SymbolType::Function)
                {
                    ctx.LogRule("CheckFinalOverrides", "as-err-override-final-method", mSym);
                    ctx.Emit(mSym, "as-err-override-final-method", methodName, reportingBase);
                }
            }
        }
    }
}

/** @brief Reports a method that replaces one a visible base class declared final. */
void CheckFinalOverrides(const Symbol& sym, const ClassSignature& sig, const DiagnosticContext& ctx)
{
    const SymbolTable& table = ctx.request.symbolTable;
    const std::string container = sym.qualifiedName.empty() ? sym.name : sym.qualifiedName;

    for (const auto& baseName : sig.bases)
    {
        const std::string cleanBase = CleanBaseType(baseName);
        if (cleanBase.empty() || !table.HasSymbolAnywhere(cleanBase) || IsMixinClass(cleanBase, table))
        {
            continue;
        }

        ReportFinalMethodOverrides(container, cleanBase, cleanBase, ctx);

        if (auto baseSyms = table.FindSymbolsPtr(cleanBase))
        {
            for (const auto& bSym : *baseSyms)
            {
                if (bSym.type == SymbolType::Class && std::holds_alternative<ClassSignature>(bSym.signature))
                {
                    for (const auto& mixinName : bSym.GetClass().includedMixins)
                    {
                        ReportFinalMethodOverrides(container, mixinName, cleanBase, ctx);
                    }
                }
            }
        }
    }
}

/** @brief Input parameters bundled for checking base classes. */
struct BaseCheckRequest
{
    const Symbol& sym;
    const ClassSignature& sig;
    const DiagnosticContext& ctx;
};

/** @brief Checks single base class properties (final, mixin class inherit). */
bool CheckBaseClassSymbol(const BaseCheckRequest& req, const Symbol& base, const std::string& cleanBase)
{
    if (base.type != SymbolType::Class)
    {
        return false;
    }

    if (!base.GetClass().modifiers.isMixin)
    {
        if (req.sig.modifiers.isMixin)
        {
            req.ctx.LogRule("CheckBases", "as-err-mixin-inherit-class", req.sym);
            req.ctx.Emit(req.sym, "as-err-mixin-inherit-class", req.sym.name, cleanBase);
        }
    }

    if (base.GetClass().modifiers.isFinal)
    {
        req.ctx.LogRule("CheckBases", "as-err-inherit-final", req.sym);
        req.ctx.Emit(req.sym, "as-err-inherit-final", cleanBase);
    }

    return !base.GetClass().modifiers.isMixin;
}

/** @brief Rules about the base list: existence, kind, count and finality. */
void CheckBases(const Symbol& sym, const ClassSignature& sig, const DiagnosticContext& ctx)
{
    uint32_t classBaseCount = 0;
    const BaseCheckRequest req{sym, sig, ctx};

    for (const auto& baseName : sig.bases)
    {
        const std::string cleanBase = CleanBaseType(baseName);
        if (cleanBase.empty())
        {
            continue;
        }

        const auto baseSymbols = ctx.request.symbolTable.FindSymbolsPtr(cleanBase);
        if (!baseSymbols || baseSymbols->empty())
        {
            continue;
        }

        bool sawClassBase = false;
        for (const auto& base : *baseSymbols)
        {
            if (base.type == SymbolType::Class)
            {
                sawClassBase = CheckBaseClassSymbol(req, base, cleanBase);
                break;
            }
        }

        if (sawClassBase && !sig.modifiers.isMixin && ++classBaseCount > 1)
        {
            ctx.LogRule("CheckBases", "as-err-multi-class-inherit", sym);
            ctx.Emit(sym, "as-err-multi-class-inherit", sym.name);
        }
    }

    if (HasInheritanceCycle(sym.qualifiedName.empty() ? sym.name : sym.qualifiedName, ctx.request.symbolTable))
    {
        ctx.LogRule("CheckBases", "as-err-circular-inherit", sym);
        ctx.Emit(sym, "as-err-circular-inherit", sym.name);
    }
}

/** @brief Collects getter and setter functions declared in the specified container. */
void CollectPropertyAccessors(const std::string& container, const DiagnosticContext& ctx,
                              ankerl::unordered_dense::map<std::string, const Symbol*>& getters,
                              ankerl::unordered_dense::map<std::string, const Symbol*>& setters)
{
    ctx.request.symbolTable.ForEachSymbolInFile(
        ctx.request.fileUri,
        [&]([[maybe_unused]] const std::string& qName, const std::vector<Symbol>& syms)
        {
            for (const auto& s : syms)
            {
                if (s.containerName == container && s.type == SymbolType::Function &&
                    std::holds_alternative<FunctionSignature>(s.signature))
                {
                    const auto& fn = s.GetFunction();
                    if (fn.modifiers.isProperty)
                    {
                        if (s.name.starts_with("get_") && s.name.size() > 4)
                        {
                            getters[s.name.substr(4)] = &s;
                        }
                        else if (s.name.starts_with("set_") && s.name.size() > 4)
                        {
                            setters[s.name.substr(4)] = &s;
                        }
                    }
                }
            }
        });
}

/** @brief Validates matching types between getter returns and setter parameter. */
void ValidatePropertyAccessorPairs(const ankerl::unordered_dense::map<std::string, const Symbol*>& getters,
                                   const ankerl::unordered_dense::map<std::string, const Symbol*>& setters,
                                   const DiagnosticContext& ctx)
{
    for (const auto& [propName, getSym] : getters)
    {
        auto it = setters.find(propName);
        if (it != setters.end())
        {
            const Symbol* setSym = it->second;
            std::string getRet = CleanBaseType(getSym->GetFunction().returnType);
            std::string setParam = !setSym->GetFunction().parameters.empty()
                                       ? CleanBaseType(setSym->GetFunction().parameters.back().typeName)
                                       : "";
            if (!getRet.empty() && !setParam.empty() && getRet != setParam)
            {
                ctx.LogRule("CheckPropertyAccessors", "as-err-property-type-mismatch", *setSym);
                ctx.Emit(*setSym, "as-err-property-type-mismatch", {propName, getRet, setParam});
            }
        }
    }
}

/** @brief Reports property accessor get/set type mismatches within a class/interface. */
void CheckPropertyAccessors(const Symbol& sym, const DiagnosticContext& ctx)
{
    const std::string container = sym.qualifiedName.empty() ? sym.name : sym.qualifiedName;
    if (container.empty())
    {
        return;
    }

    ankerl::unordered_dense::map<std::string, const Symbol*> getters;
    ankerl::unordered_dense::map<std::string, const Symbol*> setters;

    CollectPropertyAccessors(container, ctx, getters, setters);
    ValidatePropertyAccessorPairs(getters, setters, ctx);
}

static TSNode GetChildByField(TSNode node, const char* fieldName)
{
    return ts_node_child_by_field_name(node, fieldName, static_cast<uint32_t>(strlen(fieldName)));
}

/** @brief Range bounds within the host document. */
struct InstantiationRange
{
    uint32_t startLine = 0;
    uint32_t startChar = 0;
    uint32_t endLine = 0;
    uint32_t endChar = 0;
};

/** @brief Context bundled for inspecting mixin statement resolutions. */
struct MixinCheckContext
{
    std::string hostClassName;
    const Symbol* mixinSym = nullptr;
    InstantiationRange hostRange;
    const ankerl::unordered_dense::set<std::string>& hostMembers;
    const ankerl::unordered_dense::set<std::string>& mixinSelfMembers;
    ankerl::unordered_dense::set<std::string>& reported;
};

/** @brief Execution state for checking inside a mixin function body. */
struct FunctionCheckState
{
    const ankerl::unordered_dense::set<std::string>& localNames;
    MixinCheckContext& mctx;
};

/** @brief Base context for host class hierarchies. */
struct MixinHierarchyMembers
{
    std::string hostClassName;
    ankerl::unordered_dense::set<std::string> hostMembers;
};

/** @brief Collects all mixins included via includedMixins or bases. */
std::vector<std::string> CollectIncludedMixins(const ClassSignature& sig, const SymbolTable& table)
{
    std::vector<std::string> includedMixins = sig.includedMixins;
    for (const auto& b : sig.bases)
    {
        std::string clean = CleanBaseType(b);
        if (!clean.empty() && IsMixinClass(clean, table))
        {
            if (std::find(includedMixins.begin(), includedMixins.end(), clean) == includedMixins.end())
            {
                includedMixins.push_back(clean);
            }
        }
    }
    return includedMixins;
}

/** @brief Collects member names for a container and its qualified/short variants. */
void CollectContainerMembers(const std::string& containerName, const RuleIndex& ruleIndex,
                             ankerl::unordered_dense::set<std::string>& outMembers)
{
    const auto insertMembers = [&](const auto& members)
    {
        outMembers.insert(members.allMemberNames.begin(), members.allMemberNames.end());
        outMembers.insert(members.accessorPropertyNames.begin(), members.accessorPropertyNames.end());
    };

    insertMembers(ruleIndex.Members(containerName));

    if (HasScopeQualifier(containerName))
    {
        insertMembers(ruleIndex.Members(LastScopeSegment(containerName)));
    }

    auto it = ruleIndex.qualifiedTypesByShortName.find(containerName);
    if (it != ruleIndex.qualifiedTypesByShortName.end())
    {
        for (const auto& qName : it->second)
        {
            insertMembers(ruleIndex.Members(qName));
        }
    }
}

/** @brief Collects all members across an inheritance hierarchy. */
void CollectHierarchyMembers(const std::vector<std::string>& hierarchy, const RuleIndex& ruleIndex,
                             ankerl::unordered_dense::set<std::string>& outMembers)
{
    for (const auto& ancestor : hierarchy)
    {
        CollectContainerMembers(ancestor, ruleIndex, outMembers);
    }
}

/** @brief Resolves the symbol representing a mixin class. */
const Symbol* FindMixinSymbol(const std::string& mixinName, const SymbolTable& table, std::optional<Symbol>& fallback)
{
    if (auto candidates = table.FindSymbolsPtr(mixinName))
    {
        for (const auto& c : *candidates)
        {
            if (c.type == SymbolType::Class && c.GetClass().modifiers.isMixin)
            {
                return &c;
            }
        }
    }

    const std::string_view shortName = LastScopeSegment(mixinName);
    auto shortCands = table.FindTypeSymbolsByShortName(shortName);
    for (const auto& c : shortCands)
    {
        if (c.type == SymbolType::Class && c.GetClass().modifiers.isMixin)
        {
            fallback = c;
            return &*fallback;
        }
    }
    return nullptr;
}

/** @brief Reads or retrieves the source text for a mixin symbol. */
std::string ReadMixinSource(const Symbol& mixinSym, const DiagnosticContext& ctx)
{
    if (mixinSym.fileUri == ctx.request.fileUri)
    {
        return std::string(ctx.request.sourceCode);
    }

    std::string path = utils::UriToPath(mixinSym.fileUri);
    std::ifstream file(path, std::ios::binary);
    if (file.is_open())
    {
        std::ostringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }
    return {};
}

/** @brief Finds the AST node declaring the mixin. */
TSNode FindMixinAstNode(const TSTree* mixinTree, const Symbol& mixinSym)
{
    if (!mixinTree)
    {
        return TSNode{};
    }

    TSNode rootNode = ts_tree_root_node(mixinTree);
    TSPoint pt = {mixinSym.startLine, mixinSym.startCharacter};
    TSNode mixinNode = ts_node_descendant_for_point_range(rootNode, pt, pt);
    while (!ts_node_is_null(mixinNode) && NodeType(mixinNode) != "mixin_declaration" &&
           NodeType(mixinNode) != "class_declaration")
    {
        mixinNode = ts_node_parent(mixinNode);
    }
    return mixinNode;
}

/** @brief Locates the point range where a mixin is included in the host class. */
InstantiationRange FindHostMixinRange(const Symbol& sym, const Symbol& mixinSym, const std::string& mixinName,
                                      const DiagnosticContext& ctx)
{
    InstantiationRange range{sym.startLine, sym.startCharacter, sym.endLine, sym.endCharacter};
    if (!ctx.request.tree || ctx.request.sourceCode.empty())
    {
        return range;
    }

    TSNode hostRoot = ts_tree_root_node(ctx.request.tree);
    TSPoint hostPt = {sym.startLine, sym.startCharacter};
    TSNode hostNode = ts_node_descendant_for_point_range(hostRoot, hostPt, hostPt);
    while (!ts_node_is_null(hostNode) && NodeType(hostNode) != "class_declaration")
    {
        hostNode = ts_node_parent(hostNode);
    }

    TSNode incNode = FindMixinInclusionNode(hostNode, mixinName, mixinSym.name, ctx.request.sourceCode);
    if (!ts_node_is_null(incNode))
    {
        TSPoint sp = ts_node_start_point(incNode);
        TSPoint ep = ts_node_end_point(incNode);
        return InstantiationRange{sp.row, sp.column, ep.row, ep.column};
    }
    return range;
}

/** @brief Emits a diagnostic when a mixin refers to a member not present in the host. */
void EmitMixinMemberNotFound(const MixinCheckContext& mctx, std::string_view memberName, TSNode node,
                             const DiagnosticContext& ctx)
{
    const TSPoint sPoint = ts_node_start_point(node);
    const TSPoint ePoint = ts_node_end_point(node);

    DiagnosticRelatedInformation rel;
    rel.fileUri = mctx.mixinSym->fileUri;
    rel.range = {{sPoint.row, sPoint.column}, {ePoint.row, ePoint.column}};
    rel.message = i18n::FormatMessage(ctx.request.i18n, "note-in-mixin-member", "In mixin '{}': Member '{}'",
                                      std::string_view(mctx.mixinSym->name), memberName);

    RelatedDiagnosticRequest req;
    req.range = {mctx.hostRange.startLine, mctx.hostRange.startChar, mctx.hostRange.endLine, mctx.hostRange.endChar};
    req.code = diagnostics::codes::MixinInstantiationMemberNotFound;
    req.args = {mctx.mixinSym->name, mctx.hostClassName, std::string(memberName), mctx.hostClassName};
    req.related = std::move(rel);
    req.severity = DiagnosticSeverity::Error;
    ctx.EmitWithRelated(req);
}

/** @brief Checks member expressions of form this->member. */
void CheckThisMember(TSNode cur, std::string_view mixinSource, MixinCheckContext& mctx, const DiagnosticContext& ctx)
{
    TSNode objNode = GetChildByField(cur, "object");
    TSNode propNode = GetChildByField(cur, "member");
    if (ts_node_is_null(objNode) || ts_node_is_null(propNode))
    {
        return;
    }

    std::string objName = GetNodeText(objNode, mixinSource);
    if (objName != "this")
    {
        return;
    }

    std::string propName = GetNodeText(propNode, mixinSource);
    if (!propName.empty() && !mctx.hostMembers.contains(propName) && !mctx.mixinSelfMembers.contains(propName) &&
        !mctx.hostMembers.contains("get_" + propName) && !mctx.mixinSelfMembers.contains("get_" + propName) &&
        !mctx.hostMembers.contains("set_" + propName) && !mctx.mixinSelfMembers.contains("set_" + propName) &&
        !mctx.reported.contains(propName))
    {
        mctx.reported.insert(propName);
        EmitMixinMemberNotFound(mctx, propName, propNode, ctx);
    }
}

/** @brief Checks bare function calls without qualifiers. */
void CheckCallMember(TSNode cur, std::string_view mixinSource, FunctionCheckState& state, const DiagnosticContext& ctx)
{
    TSNode fnNode = GetChildByField(cur, "function");
    if (ts_node_is_null(fnNode))
    {
        return;
    }

    std::string_view fnType = NodeType(fnNode);
    if (fnType != "identifier" && fnType != "scoped_identifier")
    {
        return;
    }

    std::string fnName = GetNodeText(fnNode, mixinSource);
    if (fnName.find("::") == std::string::npos && !fnName.empty() && !state.localNames.contains(fnName) &&
        !state.mctx.mixinSelfMembers.contains(fnName) && !state.mctx.hostMembers.contains(fnName) &&
        !ctx.request.GetRuleIndex().allNames.contains(fnName) && !IsKeyword(fnName) && !IsPrimitiveTypeName(fnName) &&
        !state.mctx.reported.contains(fnName))
    {
        state.mctx.reported.insert(fnName);
        EmitMixinMemberNotFound(state.mctx, fnName, fnNode, ctx);
    }
}

/** @brief Checks if a child node matches a named field of the parent by byte offset. */
bool IsNamedChildMatch(TSNode parent, const char* fieldName, TSNode cur)
{
    TSNode child = GetChildByField(parent, fieldName);
    return !ts_node_is_null(child) && ts_node_start_byte(child) == ts_node_start_byte(cur);
}

/** @brief Checks if an identifier is a declaration name or member field name. */
bool IsDeclarationOrFieldChild(TSNode cur, TSNode parent, std::string_view pType)
{
    if (pType == "member_expression")
    {
        return IsNamedChildMatch(parent, "member", cur);
    }
    if (pType == "call_expression")
    {
        return IsNamedChildMatch(parent, "function", cur);
    }
    if (pType == "scoped_identifier" || pType == "variable_declarator" || pType == "parameter" ||
        pType == "func_declaration")
    {
        return IsNamedChildMatch(parent, "name", cur);
    }
    return false;
}

/** @brief Determines if an identifier node is in a non-reference AST context. */
bool ShouldIgnoreMixinIdentifier(TSNode cur)
{
    TSNode parent = ts_node_parent(cur);
    if (ts_node_is_null(parent))
    {
        return true;
    }

    std::string_view pType = NodeType(parent);
    if (IsDeclarationOrFieldChild(cur, parent, pType))
    {
        return true;
    }

    if (pType == "variable_declaration")
    {
        return IsNamedChildMatch(parent, "var_type", cur);
    }

    if (pType == "parameter")
    {
        return IsNamedChildMatch(parent, "type", cur);
    }

    return pType == "type" || pType == "primitive_type" || pType == "cast_expression";
}

/** @brief Checks if a name is a built-in symbol, keyword, or primitive type. */
bool IsBuiltinOrKeyword(std::string_view name)
{
    if (name.empty() || name == "this" || name == "super" || name == "cast" || name == "null" || name == "true" ||
        name == "false" || name == "string")
    {
        return true;
    }
    return IsKeyword(name) || IsPrimitiveTypeName(name);
}

/** @brief Checks if a name matches getter/setter property accessors in mixin or host. */
bool HasPropertyAccessorMatch(const std::string& name, const FunctionCheckState& state)
{
    const std::string getProp = "get_" + name;
    const std::string setProp = "set_" + name;
    return state.mctx.mixinSelfMembers.contains(getProp) || state.mctx.mixinSelfMembers.contains(setProp) ||
           state.mctx.hostMembers.contains(getProp) || state.mctx.hostMembers.contains(setProp);
}

/** @brief Checks if an identifier resolves in local, mixin, host, or global scopes. */
bool IsValidMixinIdentifier(const std::string& name, const FunctionCheckState& state, const DiagnosticContext& ctx)
{
    if (IsBuiltinOrKeyword(name))
    {
        return true;
    }

    if (state.localNames.contains(name) || state.mctx.mixinSelfMembers.contains(name) ||
        state.mctx.hostMembers.contains(name) || HasPropertyAccessorMatch(name, state))
    {
        return true;
    }

    const auto& ruleIndex = ctx.request.GetRuleIndex();
    return ruleIndex.allNames.contains(name) || ctx.request.symbolTable.HasSymbolAnywhere(name);
}

/** @brief Checks bare identifier references inside mixin function bodies. */
void CheckIdentifierMember(TSNode cur, std::string_view mixinSource, FunctionCheckState& state,
                           const DiagnosticContext& ctx)
{
    if (ShouldIgnoreMixinIdentifier(cur))
    {
        return;
    }

    std::string name = GetNodeText(cur, mixinSource);
    if (!IsValidMixinIdentifier(name, state, ctx) && !state.mctx.reported.contains(name))
    {
        state.mctx.reported.insert(name);
        EmitMixinMemberNotFound(state.mctx, name, cur, ctx);
    }
}

/** @brief Collects parameter and local variable names within a mixin function. */
void CollectLocalNames(TSNode memNode, TSNode funcBody, std::string_view mixinSource,
                       ankerl::unordered_dense::set<std::string>& localNames)
{
    TSNode paramsNode = GetChildByField(memNode, "parameters");
    if (!ts_node_is_null(paramsNode))
    {
        const uint32_t pCount = ts_node_named_child_count(paramsNode);
        for (uint32_t p = 0; p < pCount; ++p)
        {
            TSNode paramChild = ts_node_named_child(paramsNode, p);
            TSNode pName = GetChildByField(paramChild, "name");
            if (!ts_node_is_null(pName))
            {
                localNames.insert(GetNodeText(pName, mixinSource));
            }
        }
    }

    parser::ForEachDescendantNode(funcBody,
                                  [&](TSNode cur)
                                  {
                                      if (NodeType(cur) == "variable_declarator")
                                      {
                                          TSNode vdName = GetChildByField(cur, "name");
                                          if (!ts_node_is_null(vdName))
                                          {
                                              localNames.insert(GetNodeText(vdName, mixinSource));
                                          }
                                      }
                                  });
}

/** @brief Walks statements in a mixin function body using flat TSTreeCursor. */
void CheckFuncBodyStatements(TSNode funcBody, std::string_view mixinSource, FunctionCheckState& state,
                             const DiagnosticContext& ctx)
{
    parser::ForEachDescendantNode(funcBody,
                                  [&](TSNode cur)
                                  {
                                      std::string_view curType = NodeType(cur);

                                      if (curType == "member_expression")
                                      {
                                          CheckThisMember(cur, mixinSource, state.mctx, ctx);
                                      }
                                      else if (curType == "call_expression")
                                      {
                                          CheckCallMember(cur, mixinSource, state, ctx);
                                      }
                                      else if (curType == "identifier")
                                      {
                                          CheckIdentifierMember(cur, mixinSource, state, ctx);
                                      }
                                  });
}

/** @brief Checks member declarations inside a mixin body. */
void CheckMixinBody(TSNode bodyNode, std::string_view mixinSource, MixinCheckContext& mctx,
                    const DiagnosticContext& ctx)
{
    parser::ForEachChildNode(bodyNode,
                             [&](TSNode mem)
                             {
                                 if (NodeType(mem) != "func_declaration")
                                 {
                                     return;
                                 }

                                 TSNode funcBody = GetChildByField(mem, "body");
                                 if (ts_node_is_null(funcBody))
                                 {
                                     return;
                                 }

                                 ankerl::unordered_dense::set<std::string> localNames;
                                 CollectLocalNames(mem, funcBody, mixinSource, localNames);
                                 FunctionCheckState state{localNames, mctx};
                                 CheckFuncBodyStatements(funcBody, mixinSource, state, ctx);
                             });
}

/** @brief RAII holder for parsed mixin tree. */
struct MixinAstScope
{
    const TSTree* tree = nullptr;
    document::TreePtr allocatedTree{nullptr, &ts_tree_delete};
    std::unique_ptr<parser::AngelScriptParser> ownedParser;

    MixinAstScope() = default;
    MixinAstScope(const MixinAstScope&) = delete;
    MixinAstScope& operator=(const MixinAstScope&) = delete;
    MixinAstScope(MixinAstScope&&) noexcept = default;
    MixinAstScope& operator=(MixinAstScope&&) noexcept = default;
};

/** @brief Collects direct members of a mixin symbol. */
void CollectMixinSelfMembers(const Symbol& mixinSym, const RuleIndex& ruleIndex,
                             ankerl::unordered_dense::set<std::string>& outMembers)
{
    CollectContainerMembers(mixinSym.qualifiedName, ruleIndex, outMembers);
    if (mixinSym.name != mixinSym.qualifiedName)
    {
        CollectContainerMembers(mixinSym.name, ruleIndex, outMembers);
    }
}

/** @brief Obtains the AST tree for a mixin symbol. */
MixinAstScope AcquireMixinTree(const Symbol& mixinSym, const std::string& mixinSource, const DiagnosticContext& ctx)
{
    MixinAstScope scope;
    if (mixinSym.fileUri == ctx.request.fileUri && ctx.request.tree)
    {
        scope.tree = ctx.request.tree;
    }
    else
    {
        scope.ownedParser = std::make_unique<parser::AngelScriptParser>();
        scope.allocatedTree = document::MakeTreePtr(scope.ownedParser->Parse(mixinSource));
        scope.tree = scope.allocatedTree.get();
    }
    return scope;
}

/** @brief Validates a single mixin instantiation against host class. */
void CheckSingleMixinInstantiation(const std::string& mixinName, const Symbol& sym,
                                   const MixinHierarchyMembers& baseMembers, const DiagnosticContext& ctx)
{
    std::optional<Symbol> fallbackMixin;
    const Symbol* mixinSym = FindMixinSymbol(mixinName, ctx.request.symbolTable, fallbackMixin);
    if (!mixinSym)
    {
        return;
    }

    ankerl::unordered_dense::set<std::string> mixinSelfMembers;
    CollectMixinSelfMembers(*mixinSym, ctx.request.GetRuleIndex(), mixinSelfMembers);

    std::string mixinSource = ReadMixinSource(*mixinSym, ctx);
    if (mixinSource.empty())
    {
        return;
    }

    MixinAstScope astScope = AcquireMixinTree(*mixinSym, mixinSource, ctx);
    if (!astScope.tree)
    {
        return;
    }

    TSNode mixinNode = FindMixinAstNode(astScope.tree, *mixinSym);
    if (ts_node_is_null(mixinNode))
    {
        return;
    }

    InstantiationRange hostRange = FindHostMixinRange(sym, *mixinSym, mixinName, ctx);
    TSNode bodyNode = GetChildByField(mixinNode, "body");
    if (!ts_node_is_null(bodyNode))
    {
        ankerl::unordered_dense::set<std::string> reportedMissingMembers;
        MixinCheckContext mctx{baseMembers.hostClassName, mixinSym,         hostRange,
                               baseMembers.hostMembers,   mixinSelfMembers, reportedMissingMembers};
        CheckMixinBody(bodyNode, mixinSource, mctx, ctx);
    }
}

/**
 * @brief Checks that internal statements of mixins instantiated in a host class resolve against the host.
 */
void CheckMixinInstantiations(const Symbol& sym, const ClassSignature& sig, const DiagnosticContext& ctx)
{
    if (sig.modifiers.isMixin)
    {
        return;
    }

    const std::vector<std::string> includedMixins = CollectIncludedMixins(sig, ctx.request.symbolTable);
    if (includedMixins.empty())
    {
        return;
    }

    const std::string hostClassName = sym.qualifiedName.empty() ? sym.name : sym.qualifiedName;
    const auto hostHierarchy = GetInheritedTypeHierarchy(hostClassName, ctx.request.symbolTable);

    MixinHierarchyMembers baseMembers{hostClassName, {}};
    CollectHierarchyMembers(hostHierarchy, ctx.request.GetRuleIndex(), baseMembers.hostMembers);

    for (const auto& mixinName : includedMixins)
    {
        CheckSingleMixinInstantiation(mixinName, sym, baseMembers, ctx);
    }
}
} // namespace

bool WalkInheritancePath(std::vector<std::string>& path, const std::string& current, const SymbolTable& table)
{
    if (std::find(path.begin(), path.end(), current) != path.end())
    {
        return true;
    }
    if (path.size() > 64)
    {
        return false;
    }

    path.push_back(current);

    const auto symbols = table.FindSymbolsPtr(current);
    if (symbols)
    {
        for (const auto& sym : *symbols)
        {
            const auto& bases = (sym.type == SymbolType::Class)       ? sym.GetClass().bases
                                : (sym.type == SymbolType::Interface) ? sym.GetInterface().inheritedInterfaces
                                                                      : std::vector<std::string>{};

            for (const auto& base : bases)
            {
                const std::string cleanBase = CleanBaseType(base);
                if (!cleanBase.empty() && WalkInheritancePath(path, cleanBase, table))
                {
                    path.pop_back();
                    return true;
                }
            }
        }
    }

    path.pop_back();
    return false;
}

bool HasInheritanceCycle(const std::string& typeName, const SymbolTable& table)
{
    const auto symbols = table.FindSymbolsPtr(typeName);
    if (!symbols)
    {
        return false;
    }

    std::vector<std::string> path;
    for (const auto& sym : *symbols)
    {
        const auto& bases = (sym.type == SymbolType::Class)       ? sym.GetClass().bases
                            : (sym.type == SymbolType::Interface) ? sym.GetInterface().inheritedInterfaces
                                                                  : std::vector<std::string>{};

        path.assign(1, typeName);
        for (const auto& base : bases)
        {
            const std::string cleanBase = CleanBaseType(base);
            if (!cleanBase.empty() && WalkInheritancePath(path, cleanBase, table))
            {
                return true;
            }
        }
    }

    return false;
}

void ValidateClass(const Symbol& sym, const DiagnosticContext& ctx)
{
    if (sym.type != SymbolType::Class && sym.type != SymbolType::Interface)
    {
        return;
    }

    if (IsFromPredefinedStub(sym, ctx))
    {
        return;
    }

    if (IsReservedKeyword(sym.name) || IsPrimitiveTypeName(sym.name) ||
        sym.name == ctx.request.GetEffectiveStringTypeName() || sym.name == ctx.request.GetEffectiveArrayTypeName())
    {
        ctx.LogRule("ValidateClass", "as-err-reserved-keyword-name", sym);
        ctx.Emit(sym, "as-err-reserved-keyword-name", sym.name);
        return;
    }

    if (sym.type == SymbolType::Interface)
    {
        if (HasInheritanceCycle(sym.qualifiedName.empty() ? sym.name : sym.qualifiedName, ctx.request.symbolTable))
        {
            ctx.LogRule("ValidateClass", "as-err-circular-inherit", sym);
            ctx.Emit(sym, "as-err-circular-inherit", sym.name);
        }
        return;
    }

    const auto& sig = sym.GetClass();
    if (sig.isTemplate)
    {
        ctx.LogRule("ValidateClass", "as-err-template-class-not-supported", sym);
        ctx.Emit(sym, "as-err-template-class-not-supported", sym.name);
    }

    CheckClassModifiers(sym, sig, ctx);
    CheckBases(sym, sig, ctx);
    CheckFinalOverrides(sym, sig, ctx);
    CheckInterfaceImplementation(sym, sig, ctx);
    CheckPropertyAccessors(sym, ctx);
    CheckMixinInstantiations(sym, sig, ctx);
}
} // namespace angel_lsp::analysis::rules
