#include "analysis/rules/ClassRules.h"
#include "analysis/ASTUtils.h"
#include "analysis/DiagnosticCodes.h"
#include "analysis/SemanticHelpers.h"
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
                ctx.Emit(sym, "as-err-interface-impl-missing", sym.name, methodName, cleanBase);
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

/** @brief Reports a method that replaces one a visible base class declared final. */
void CheckFinalOverrides(const Symbol& sym, const ClassSignature& sig, const DiagnosticContext& ctx)
{
    const SymbolTable& table = ctx.request.symbolTable;
    const std::string container = sym.qualifiedName.empty() ? sym.name : sym.qualifiedName;

    for (const auto& baseName : sig.bases)
    {
        const std::string cleanBase = CleanBaseType(baseName);
        if (cleanBase.empty() || !table.HasSymbolAnywhere(cleanBase))
        {
            continue;
        }

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
                        ctx.Emit(mSym, "as-err-override-final-method", methodName, cleanBase);
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
                ctx.Emit(*setSym, "as-err-property-type-mismatch", propName, getRet, setParam);
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
    const ankerl::unordered_dense::set<std::string>& superMembers;
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
    ankerl::unordered_dense::set<std::string> superMembers;
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
    const auto& cm = ruleIndex.Members(containerName);
    outMembers.insert(cm.allMemberNames.begin(), cm.allMemberNames.end());

    auto lastScope = containerName.rfind("::");
    if (lastScope != std::string::npos)
    {
        const auto& cmShort = ruleIndex.Members(containerName.substr(lastScope + 2));
        outMembers.insert(cmShort.allMemberNames.begin(), cmShort.allMemberNames.end());
    }

    auto it = ruleIndex.qualifiedTypesByShortName.find(containerName);
    if (it != ruleIndex.qualifiedTypesByShortName.end())
    {
        for (const auto& qName : it->second)
        {
            const auto& cmQ = ruleIndex.Members(qName);
            outMembers.insert(cmQ.allMemberNames.begin(), cmQ.allMemberNames.end());
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

    std::string shortName = mixinName;
    auto lastScope = shortName.rfind("::");
    if (lastScope != std::string::npos)
    {
        shortName = shortName.substr(lastScope + 2);
    }
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

/** @brief Searches the base_class_list node for the mixin inclusion range. */
std::optional<InstantiationRange> FindMixinRangeInBases(TSNode hostNode, const Symbol& mixinSym,
                                                        const std::string& mixinName, std::string_view sourceCode)
{
    const uint32_t childCount = ts_node_child_count(hostNode);
    for (uint32_t c = 0; c < childCount; ++c)
    {
        TSNode child = ts_node_child(hostNode, c);
        if (NodeType(child) == "base_class_list")
        {
            const uint32_t bCount = ts_node_named_child_count(child);
            for (uint32_t b = 0; b < bCount; ++b)
            {
                TSNode baseChild = ts_node_named_child(child, b);
                std::string bText = GetNodeText(baseChild, sourceCode);
                if (CleanBaseType(bText) == mixinName || bText == mixinSym.name ||
                    bText.ends_with("::" + mixinSym.name))
                {
                    TSPoint sp = ts_node_start_point(baseChild);
                    TSPoint ep = ts_node_end_point(baseChild);
                    return InstantiationRange{sp.row, sp.column, ep.row, ep.column};
                }
            }
        }
    }
    return std::nullopt;
}

/** @brief Searches the class body node for the mixin inclusion range. */
std::optional<InstantiationRange> FindMixinRangeInBody(TSNode hostNode, const Symbol& mixinSym,
                                                       std::string_view sourceCode)
{
    TSNode bodyNode = GetChildByField(hostNode, "body");
    if (ts_node_is_null(bodyNode))
    {
        return std::nullopt;
    }

    const uint32_t mCount = ts_node_child_count(bodyNode);
    for (uint32_t m = 0; m < mCount; ++m)
    {
        TSNode mNode = ts_node_child(bodyNode, m);
        std::string mText = GetNodeText(mNode, sourceCode);
        if (mText.find(mixinSym.name) != std::string::npos)
        {
            TSPoint sp = ts_node_start_point(mNode);
            TSPoint ep = ts_node_end_point(mNode);
            return InstantiationRange{sp.row, sp.column, ep.row, ep.column};
        }
    }
    return std::nullopt;
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

    if (ts_node_is_null(hostNode))
    {
        return range;
    }

    if (auto r = FindMixinRangeInBases(hostNode, mixinSym, mixinName, ctx.request.sourceCode))
    {
        return *r;
    }

    if (auto r = FindMixinRangeInBody(hostNode, mixinSym, ctx.request.sourceCode))
    {
        return *r;
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
    rel.message = fmt::format("In mixin '{}': Member '{}'", mctx.mixinSym->name, memberName);

    ctx.EmitWithRelated(mctx.hostRange.startLine, mctx.hostRange.startChar, mctx.hostRange.endLine,
                        mctx.hostRange.endChar, diagnostics::codes::MixinInstantiationMemberNotFound,
                        mctx.mixinSym->name, mctx.hostClassName, std::string(memberName), mctx.hostClassName, rel,
                        DiagnosticSeverity::Error);
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
        !mctx.reported.contains(propName))
    {
        mctx.reported.insert(propName);
        EmitMixinMemberNotFound(mctx, propName, propNode, ctx);
    }
}

/** @brief Checks scoped identifiers of form BaseClass::member. */
void CheckBaseClassMember(TSNode cur, std::string_view mixinSource, MixinCheckContext& mctx,
                          const DiagnosticContext& ctx)
{
    std::string scPrefix;
    std::string nmText;
    const uint32_t namedCount = ts_node_named_child_count(cur);
    if (namedCount >= 2)
    {
        TSNode scopeChild = ts_node_named_child(cur, 0);
        TSNode nameChild = ts_node_named_child(cur, namedCount - 1);
        scPrefix = GetNodeText(scopeChild, mixinSource);
        nmText = GetNodeText(nameChild, mixinSource);
    }
    else
    {
        std::string scFull = GetNodeText(cur, mixinSource);
        auto pos = scFull.rfind("::");
        if (pos != std::string::npos)
        {
            scPrefix = scFull.substr(0, pos);
            nmText = scFull.substr(pos + 2);
        }
    }

    if (scPrefix == "BaseClass" && !nmText.empty() && !mctx.superMembers.contains(nmText) &&
        !mctx.reported.contains(nmText))
    {
        mctx.reported.insert(nmText);
        EmitMixinMemberNotFound(mctx, nmText, cur, ctx);
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

    if (ts_node_is_null(funcBody))
    {
        return;
    }

    TSTreeCursor cursor = ts_tree_cursor_new(funcBody);
    bool reachedRoot = false;

    while (!reachedRoot)
    {
        TSNode cur = ts_tree_cursor_current_node(&cursor);
        if (NodeType(cur) == "variable_declarator")
        {
            TSNode vdName = GetChildByField(cur, "name");
            if (!ts_node_is_null(vdName))
            {
                localNames.insert(GetNodeText(vdName, mixinSource));
            }
        }

        if (ts_tree_cursor_goto_first_child(&cursor))
        {
            continue;
        }
        if (ts_tree_cursor_goto_next_sibling(&cursor))
        {
            continue;
        }

        while (!reachedRoot)
        {
            if (!ts_tree_cursor_goto_parent(&cursor))
            {
                reachedRoot = true;
                break;
            }
            if (ts_tree_cursor_goto_next_sibling(&cursor))
            {
                break;
            }
        }
    }

    ts_tree_cursor_delete(&cursor);
}

/** @brief Walks statements in a mixin function body using flat TSTreeCursor. */
void CheckFuncBodyStatements(TSNode funcBody, std::string_view mixinSource, FunctionCheckState& state,
                             const DiagnosticContext& ctx)
{
    if (ts_node_is_null(funcBody))
    {
        return;
    }

    TSTreeCursor cursor = ts_tree_cursor_new(funcBody);
    bool reachedRoot = false;

    while (!reachedRoot)
    {
        TSNode cur = ts_tree_cursor_current_node(&cursor);
        std::string_view curType = NodeType(cur);

        if (curType == "member_expression")
        {
            CheckThisMember(cur, mixinSource, state.mctx, ctx);
        }
        else if (curType == "scoped_identifier")
        {
            CheckBaseClassMember(cur, mixinSource, state.mctx, ctx);
        }
        else if (curType == "call_expression")
        {
            CheckCallMember(cur, mixinSource, state, ctx);
        }

        if (ts_tree_cursor_goto_first_child(&cursor))
        {
            continue;
        }
        if (ts_tree_cursor_goto_next_sibling(&cursor))
        {
            continue;
        }

        while (!reachedRoot)
        {
            if (!ts_tree_cursor_goto_parent(&cursor))
            {
                reachedRoot = true;
                break;
            }
            if (ts_tree_cursor_goto_next_sibling(&cursor))
            {
                break;
            }
        }
    }

    ts_tree_cursor_delete(&cursor);
}

/** @brief Checks member declarations inside a mixin body. */
void CheckMixinBody(TSNode bodyNode, std::string_view mixinSource, MixinCheckContext& mctx,
                    const DiagnosticContext& ctx)
{
    const uint32_t memberCount = ts_node_child_count(bodyNode);
    for (uint32_t m = 0; m < memberCount; ++m)
    {
        TSNode mem = ts_node_child(bodyNode, m);
        if (NodeType(mem) != "func_declaration")
        {
            continue;
        }

        TSNode funcBody = GetChildByField(mem, "body");
        if (ts_node_is_null(funcBody))
        {
            continue;
        }

        ankerl::unordered_dense::set<std::string> localNames;
        CollectLocalNames(mem, funcBody, mixinSource, localNames);
        FunctionCheckState state{localNames, mctx};
        CheckFuncBodyStatements(funcBody, mixinSource, state, ctx);
    }
}

/** @brief RAII holder for parsed mixin tree. */
struct MixinAstScope
{
    const TSTree* tree = nullptr;
    TSTree* allocatedTree = nullptr;
    std::unique_ptr<parser::AngelScriptParser> ownedParser;

    ~MixinAstScope()
    {
        if (allocatedTree)
        {
            ts_tree_delete(allocatedTree);
        }
    }

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
        scope.allocatedTree = scope.ownedParser->Parse(mixinSource);
        scope.tree = scope.allocatedTree;
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
        MixinCheckContext mctx{baseMembers.hostClassName,
                               mixinSym,
                               hostRange,
                               baseMembers.hostMembers,
                               baseMembers.superMembers,
                               mixinSelfMembers,
                               reportedMissingMembers};
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
    const std::string directSuperClass = ResolveBaseClass(hostClassName, ctx.request.symbolTable);
    const std::vector<std::string> superHierarchy =
        directSuperClass.empty() ? std::vector<std::string>{}
                                 : GetInheritedTypeHierarchy(directSuperClass, ctx.request.symbolTable);

    MixinHierarchyMembers baseMembers{hostClassName, {}, {}};
    CollectHierarchyMembers(hostHierarchy, ctx.request.GetRuleIndex(), baseMembers.hostMembers);
    CollectHierarchyMembers(superHierarchy, ctx.request.GetRuleIndex(), baseMembers.superMembers);

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

    const auto strType = ctx.request.GetStringTypeName();
    const auto arrType = ctx.request.GetArrayTypeName();
    const std::string_view effectiveStrType = strType.empty() ? std::string_view("string") : strType;
    const std::string_view effectiveArrType = arrType.empty() ? std::string_view("array") : arrType;
    if (IsReservedKeyword(sym.name) || IsPrimitiveTypeName(sym.name) || sym.name == effectiveStrType ||
        sym.name == effectiveArrType)
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
