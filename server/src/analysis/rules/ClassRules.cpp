#include "analysis/rules/ClassRules.h"
#include "analysis/DiagnosticCodes.h"
#include "analysis/SemanticHelpers.h"
#include "parser/AngelScriptParser.h"
#include "utils/Utils.h"
#include "spdlog/fmt/fmt.h"

#include <algorithm>
#include <fstream>
#include <memory>
#include <vector>

namespace angel_lsp::analysis::rules
{
    namespace
    {
        /** @brief Modifier and shape rules a class declaration must satisfy on its own. */
        void CheckClassModifiers(const Symbol &sym, const ClassSignature &sig, const DiagnosticContext &ctx)
        {
            // These three used to borrow as-syntax-error, whose message reads `Syntax error: "Foo"`.
            // None of them is one: the parser accepted the declaration and handed it over intact.
            // What the user needs told is which rule the declaration broke.

            // 'external shared class X;' is allowed to have no body.
            // A forward declaration 'class X;' is allowed if a full definition with a body exists.
            if (!sig.hasBraces && !sig.modifiers.isExternal)
            {
                bool hasFullDefinition = false;
                if (auto symsPtr = ctx.request.symbolTable.FindSymbolsPtr(sym.name))
                {
                    for (const auto &s : *symsPtr)
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

            if (sig.modifiers.isExternal && !sig.modifiers.isShared)
            {
                ctx.LogRule("CheckClassModifiers", "as-err-external-not-shared", sym);
                ctx.Emit(sym, "as-err-external-not-shared", sym.name);
            }

            if (sig.modifiers.isExternal && sig.modifiers.isShared)
            {
                // Where the definition has to be depends on whether this server knows the modules.
                //
                // It has to be in a DIFFERENT module. Measured, and stricter than it looks: with
                // `external shared class Foo;` and `shared class Foo { }` in the same module the
                // compiler still answers "External shared entity 'Foo' not found". An `external`
                // declaration says "this is built elsewhere", and elsewhere means another module.
                //
                // Without angelscript.modules configured this server cannot tell one module from
                // another, so it keeps the older, laxer question - is the name declared shared
                // anywhere - and accepts the false negative that comes with it. A directory of
                // scripts may be one module or one per file, and only the host knows which; a rule
                // that guessed would report correct code as broken in whichever case it guessed
                // wrong.
                const bool knowsModules =
                    ctx.request.moduleContext.has_value() && !ctx.request.moduleContext->name.empty();

                bool hasFullSharedDefinition = false;

                if (knowsModules)
                {
                    hasFullSharedDefinition =
                        ctx.request.moduleContext->sharedElsewhere.contains(sym.name);
                }
                else if (auto symsPtr = ctx.request.symbolTable.FindSymbolsPtr(sym.name))
                {
                    for (const auto &s : *symsPtr)
                    {
                        if (s.type == SymbolType::Class && s.GetClass().hasBraces &&
                            s.GetClass().modifiers.isShared && !s.GetClass().modifiers.isExternal)
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

            // REMOVED: a check for 'override'/'explicit' on a class. It could never fire. The
            // grammar's declaration_modifier is choice("shared", "external", "abstract", "final"),
            // so `override class Foo {}` does not parse as a class declaration at all and the
            // parser pass reports it. ClassSignature::modifiers.isOverride is unreachable for the
            // same reason ClassSignature::isTemplate is - see the note further down.

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

            // A mixin is textually merged into whatever includes it, so it can declare neither a
            // base nor a nested type of its own.
            if (ctx.request.GetRuleIndex().Members(sym.qualifiedName).hasNestedType)
            {
                ctx.LogRule("CheckClassModifiers", "as-err-mixin-child-type", sym);
                ctx.Emit(sym, "as-err-mixin-child-type", sym.name);
            }
        }

        /**
         * @brief Collects the method names a type actually provides, base *classes* included.
         * @note Interfaces in the hierarchy are skipped on purpose. They declare what must be
         *       implemented, not what is - counting their own declarations as implementations makes
         *       every class trivially satisfy every interface it names.
         */
        ankerl::unordered_dense::set<std::string> CollectImplementedMethodNames(const std::string &typeName,
                                                                               const SymbolTable &table,
                                                                               const RuleIndex &index)
        {
            ankerl::unordered_dense::set<std::string> names;

            for (const auto &ancestor : GetInheritedTypeHierarchy(typeName, table))
            {
                const auto ancestorSymbols = table.FindSymbolsPtr(ancestor);
                const bool isInterface = ancestorSymbols &&
                    std::any_of(ancestorSymbols->begin(), ancestorSymbols->end(),
                                [](const Symbol &sym) { return sym.type == SymbolType::Interface; });
                if (isInterface)
                {
                    continue;
                }

                for (const auto &name : index.Members(ancestor).methodNames)
                {
                    names.insert(name);
                }
            }
            return names;
        }

        /**
         * @brief Reports interface methods a class declares nowhere in its hierarchy.
         *
         * Matched by name only, deliberately. The deleted implementation compared return type,
         * constness and every parameter's type, direction, constness and reference-ness, and looked
         * only at direct bases - so an implementation inherited from a grandparent, or one whose
         * parameter spelling differed harmlessly, was reported as missing. What a user actually
         * wants caught is a method they forgot entirely, and that survives the weaker test.
         */
        void CheckInterfaceImplementation(const Symbol &sym, const ClassSignature &sig, const DiagnosticContext &ctx)
        {
            // A mixin class cannot be instantiated on its own; if it implements interfaces,
            // the implementation of any omitted interface methods is deferred to the consuming class.
            if (sig.modifiers.isMixin)
            {
                return;
            }

            const SymbolTable &table = ctx.request.symbolTable;
            const std::string container = sym.qualifiedName.empty() ? sym.name : sym.qualifiedName;

            if (!HierarchyIsFullyVisible(container, table))
            {
                return;
            }

            const auto implemented = CollectImplementedMethodNames(container, table, ctx.request.GetRuleIndex());

            ankerl::unordered_dense::set<std::string> interfacesToCheck;

            for (const auto &baseName : sig.bases)
            {
                const std::string cleanBase = CleanBaseType(baseName);
                const auto baseSymbols = table.FindSymbolsPtr(cleanBase);
                if (!baseSymbols)
                {
                    continue;
                }

                for (const auto &base : *baseSymbols)
                {
                    if (base.type == SymbolType::Interface)
                    {
                        interfacesToCheck.insert(cleanBase);
                    }
                    else if (base.type == SymbolType::Class && base.GetClass().modifiers.isMixin)
                    {
                        for (const auto &mixinAncestor : GetInheritedTypeHierarchy(cleanBase, table))
                        {
                            const auto mixinAncestorSyms = table.FindSymbolsPtr(mixinAncestor);
                            if (mixinAncestorSyms && std::any_of(mixinAncestorSyms->begin(), mixinAncestorSyms->end(),
                                [](const Symbol &s) { return s.type == SymbolType::Interface; }))
                            {
                                interfacesToCheck.insert(mixinAncestor);
                            }
                        }
                    }
                }
            }

            for (const auto &cleanBase : interfacesToCheck)
            {
                for (const auto &methodName : ctx.request.GetRuleIndex().Members(cleanBase).methodNames)
                {
                    if (!implemented.contains(methodName))
                    {
                        ctx.LogRule("CheckInterfaceImplementation", "as-err-interface-impl-missing", sym);
                        ctx.Emit(sym, "as-err-interface-impl-missing", sym.name, methodName, cleanBase);
                    }
                }
            }
        }

        /** @brief Reports a method that replaces one a visible base class declared final. */
        void CheckFinalOverrides(const Symbol &sym, const ClassSignature &sig, const DiagnosticContext &ctx)
        {
            const SymbolTable &table = ctx.request.symbolTable;
            const std::string container = sym.qualifiedName.empty() ? sym.name : sym.qualifiedName;

            for (const auto &baseName : sig.bases)
            {
                const std::string cleanBase = CleanBaseType(baseName);
                if (cleanBase.empty() || !table.HasSymbolAnywhere(cleanBase))
                {
                    continue;
                }

                for (const auto &methodName : ctx.request.GetRuleIndex().Members(cleanBase).finalMethodNames)
                {
                    const std::string derivedName = container.empty() ? methodName
                                                                      : container + "::" + methodName;
                    if (auto methodsPtr = table.FindSymbolsPtr(derivedName))
                    {
                        for (const auto &mSym : *methodsPtr)
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

        /** @brief Rules about the base list: existence, kind, count and finality. */
        void CheckBases(const Symbol &sym, const ClassSignature &sig, const DiagnosticContext &ctx)
        {
            uint32_t classBaseCount = 0;

            for (const auto &baseName : sig.bases)
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
                for (const auto &base : *baseSymbols)
                {
                    if (base.type == SymbolType::Class)
                    {
                        if (!base.GetClass().modifiers.isMixin)
                        {
                            sawClassBase = true;
                            if (sig.modifiers.isMixin)
                            {
                                ctx.LogRule("CheckBases", "as-err-mixin-inherit-class", sym);
                                ctx.Emit(sym, "as-err-mixin-inherit-class", sym.name, cleanBase);
                            }
                        }
                        // A mixin among the bases is not an error and never was: that is how a
                        // mixin is included. `class weapon_p90 : ScriptBasePlayerWeaponEntity,
                        // CS16BASE::WeaponBase` is the idiom, hundreds of corpus files use it, and
                        // a real engine compiles it.
                        if (base.GetClass().modifiers.isFinal)
                        {
                            ctx.LogRule("CheckBases", "as-err-inherit-final", sym);
                            ctx.Emit(sym, "as-err-inherit-final", cleanBase);
                        }
                        break;
                    }
                }

                if (sawClassBase && !sig.modifiers.isMixin && ++classBaseCount > 1)
                {
                    ctx.LogRule("CheckBases", "as-err-multi-class-inherit", sym);
                    ctx.Emit(sym, "as-err-multi-class-inherit", sym.name);
                }
            }

            if (HasInheritanceCycle(sym.qualifiedName.empty() ? sym.name : sym.qualifiedName,
                                    ctx.request.symbolTable))
            {
                ctx.LogRule("CheckBases", "as-err-circular-inherit", sym);
                ctx.Emit(sym, "as-err-circular-inherit", sym.name);
            }
        }

        /** @brief Reports property accessor get/set type mismatches within a class/interface. */
        void CheckPropertyAccessors(const Symbol &sym, const ClassSignature & /*sig*/, const DiagnosticContext &ctx)
        {
            const std::string container = sym.qualifiedName.empty() ? sym.name : sym.qualifiedName;
            if (container.empty())
            {
                return;
            }

            const SymbolTable &table = ctx.request.symbolTable;

            ankerl::unordered_dense::map<std::string, const Symbol *> getters;
            ankerl::unordered_dense::map<std::string, const Symbol *> setters;

            table.ForEachSymbolInFile(ctx.request.fileUri, [&](const std::string & /*qName*/, const std::vector<Symbol> &syms) {
                for (const auto &s : syms)
                {
                    if (s.containerName == container && s.type == SymbolType::Function &&
                        std::holds_alternative<FunctionSignature>(s.signature))
                    {
                        const auto &fn = s.GetFunction();
                        if (fn.modifiers.isProperty)
                        {
                            if (s.name.starts_with("get_") && s.name.size() > 4)
                            {
                                std::string propName = s.name.substr(4);
                                getters[propName] = &s;
                            }
                            else if (s.name.starts_with("set_") && s.name.size() > 4)
                            {
                                std::string propName = s.name.substr(4);
                                setters[propName] = &s;
                            }
                        }
                    }
                }
            });

            for (const auto &[propName, getSym] : getters)
            {
                auto it = setters.find(propName);
                if (it != setters.end())
                {
                    const Symbol *setSym = it->second;
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

        static TSNode GetChildByField(TSNode node, const char *fieldName)
        {
            return ts_node_child_by_field_name(node, fieldName, static_cast<uint32_t>(strlen(fieldName)));
        }

        /**
         * @brief Checks that internal statements of mixins instantiated in a host class resolve against the host.
         */
        void CheckMixinInstantiations(const Symbol &sym, const ClassSignature &sig, const DiagnosticContext &ctx)
        {
            if (sig.modifiers.isMixin)
            {
                return;
            }

            std::vector<std::string> includedMixins = sig.includedMixins;
            for (const auto &b : sig.bases)
            {
                std::string clean = CleanBaseType(b);
                if (!clean.empty() && IsMixinClass(clean, ctx.request.symbolTable))
                {
                    if (std::find(includedMixins.begin(), includedMixins.end(), clean) == includedMixins.end())
                    {
                        includedMixins.push_back(clean);
                    }
                }
            }

            if (includedMixins.empty())
            {
                return;
            }

            const std::string hostClassName = sym.qualifiedName.empty() ? sym.name : sym.qualifiedName;

            auto hostHierarchy = GetInheritedTypeHierarchy(hostClassName, ctx.request.symbolTable);

            std::string directSuperClass = ResolveBaseClass(hostClassName, ctx.request.symbolTable);

            std::vector<std::string> superHierarchy;
            if (!directSuperClass.empty())
            {
                superHierarchy = GetInheritedTypeHierarchy(directSuperClass, ctx.request.symbolTable);
            }

            auto collectContainerMembers = [&](const std::string &containerName, ankerl::unordered_dense::set<std::string> &outMembers)
            {
                const auto &ruleIndex = ctx.request.GetRuleIndex();
                const auto &cm = ruleIndex.Members(containerName);
                outMembers.insert(cm.allMemberNames.begin(), cm.allMemberNames.end());

                auto lastScope = containerName.rfind("::");
                if (lastScope != std::string::npos)
                {
                    const auto &cmShort = ruleIndex.Members(containerName.substr(lastScope + 2));
                    outMembers.insert(cmShort.allMemberNames.begin(), cmShort.allMemberNames.end());
                }

                auto it = ruleIndex.qualifiedTypesByShortName.find(containerName);
                if (it != ruleIndex.qualifiedTypesByShortName.end())
                {
                    for (const auto &qName : it->second)
                    {
                        const auto &cmQ = ruleIndex.Members(qName);
                        outMembers.insert(cmQ.allMemberNames.begin(), cmQ.allMemberNames.end());
                    }
                }
            };

            ankerl::unordered_dense::set<std::string> hostMembers;
            for (const auto &ancestor : hostHierarchy)
            {
                collectContainerMembers(ancestor, hostMembers);
            }

            ankerl::unordered_dense::set<std::string> superMembers;
            for (const auto &ancestor : superHierarchy)
            {
                collectContainerMembers(ancestor, superMembers);
            }

            for (const auto &mixinName : includedMixins)
            {
                const Symbol *mixinSym = nullptr;
                auto candidates = ctx.request.symbolTable.FindSymbols(mixinName);
                for (const auto &c : candidates)
                {
                    if (c.type == SymbolType::Class && c.GetClass().modifiers.isMixin)
                    {
                        mixinSym = &c;
                        break;
                    }
                }
                if (!mixinSym)
                {
                    std::string shortName = mixinName;
                    auto lastScope = shortName.rfind("::");
                    if (lastScope != std::string::npos)
                    {
                        shortName = shortName.substr(lastScope + 2);
                    }
                    auto shortCands = ctx.request.symbolTable.FindTypeSymbolsByShortName(shortName);
                    for (const auto &c : shortCands)
                    {
                        if (c.type == SymbolType::Class && c.GetClass().modifiers.isMixin)
                        {
                            mixinSym = &c;
                            break;
                        }
                    }
                }

                if (!mixinSym)
                {
                    continue;
                }

                ankerl::unordered_dense::set<std::string> mixinSelfMembers;
                collectContainerMembers(mixinSym->qualifiedName, mixinSelfMembers);
                if (mixinSym->name != mixinSym->qualifiedName)
                {
                    collectContainerMembers(mixinSym->name, mixinSelfMembers);
                }

                std::string mixinSource;
                if (mixinSym->fileUri == ctx.request.fileUri)
                {
                    mixinSource = ctx.request.sourceCode;
                }
                else
                {
                    std::string path = utils::UriToPath(mixinSym->fileUri);
                    std::ifstream file(path, std::ios::binary);
                    if (file.is_open())
                    {
                        mixinSource.assign((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
                    }
                }

                if (mixinSource.empty())
                {
                    continue;
                }

                TSTree *allocatedTree = nullptr;
                const TSTree *mixinTree = nullptr;
                std::unique_ptr<parser::AngelScriptParser> ownedParser;
                if (mixinSym->fileUri == ctx.request.fileUri && ctx.request.tree)
                {
                    mixinTree = ctx.request.tree;
                }
                else
                {
                    ownedParser = std::make_unique<parser::AngelScriptParser>();
                    allocatedTree = ownedParser->Parse(mixinSource);
                    mixinTree = allocatedTree;
                }

                if (!mixinTree)
                {
                    continue;
                }

                TSNode rootNode = ts_tree_root_node(mixinTree);
                TSPoint pt = { mixinSym->startLine, mixinSym->startCharacter };
                TSNode mixinNode = ts_node_descendant_for_point_range(rootNode, pt, pt);
                while (!ts_node_is_null(mixinNode) &&
                       std::string_view(ts_node_type(mixinNode)) != "mixin_declaration" &&
                       std::string_view(ts_node_type(mixinNode)) != "class_declaration")
                {
                    mixinNode = ts_node_parent(mixinNode);
                }

                if (ts_node_is_null(mixinNode))
                {
                    if (allocatedTree != nullptr)
                    {
                        ts_tree_delete(allocatedTree);
                    }
                    continue;
                }

                uint32_t hostIncStartLine = sym.startLine;
                uint32_t hostIncStartChar = sym.startCharacter;
                uint32_t hostIncEndLine = sym.endLine;
                uint32_t hostIncEndChar = sym.endCharacter;

                if (ctx.request.tree && !ctx.request.sourceCode.empty())
                {
                    TSNode hostRoot = ts_tree_root_node(ctx.request.tree);
                    TSPoint hostPt = { sym.startLine, sym.startCharacter };
                    TSNode hostNode = ts_node_descendant_for_point_range(hostRoot, hostPt, hostPt);
                    while (!ts_node_is_null(hostNode) && std::string_view(ts_node_type(hostNode)) != "class_declaration")
                    {
                        hostNode = ts_node_parent(hostNode);
                    }

                    if (!ts_node_is_null(hostNode))
                    {
                        bool foundIncNode = false;
                        uint32_t childCount = ts_node_child_count(hostNode);
                        for (uint32_t c = 0; c < childCount; ++c)
                        {
                            TSNode child = ts_node_child(hostNode, c);
                            if (std::string_view(ts_node_type(child)) == "base_class_list")
                            {
                                uint32_t bCount = ts_node_named_child_count(child);
                                for (uint32_t b = 0; b < bCount; ++b)
                                {
                                    TSNode baseChild = ts_node_named_child(child, b);
                                    std::string bText = GetNodeText(baseChild, ctx.request.sourceCode);
                                    if (CleanBaseType(bText) == mixinName || bText == mixinSym->name || bText.ends_with("::" + mixinSym->name))
                                    {
                                        TSPoint sp = ts_node_start_point(baseChild);
                                        TSPoint ep = ts_node_end_point(baseChild);
                                        hostIncStartLine = sp.row;
                                        hostIncStartChar = sp.column;
                                        hostIncEndLine = ep.row;
                                        hostIncEndChar = ep.column;
                                        foundIncNode = true;
                                        break;
                                    }
                                }
                            }
                            if (foundIncNode) break;
                        }

                        if (!foundIncNode)
                        {
                            TSNode bodyNode = GetChildByField(hostNode, "body");
                            if (!ts_node_is_null(bodyNode))
                            {
                                uint32_t mCount = ts_node_child_count(bodyNode);
                                for (uint32_t m = 0; m < mCount; ++m)
                                {
                                    TSNode mNode = ts_node_child(bodyNode, m);
                                    std::string mText = GetNodeText(mNode, ctx.request.sourceCode);
                                    if (mText.find(mixinSym->name) != std::string::npos)
                                    {
                                        TSPoint sp = ts_node_start_point(mNode);
                                        TSPoint ep = ts_node_end_point(mNode);
                                        hostIncStartLine = sp.row;
                                        hostIncStartChar = sp.column;
                                        hostIncEndLine = ep.row;
                                        hostIncEndChar = ep.column;
                                        foundIncNode = true;
                                        break;
                                    }
                                }
                            }
                        }
                    }
                }

                TSNode bodyNode = GetChildByField(mixinNode, "body");
                if (ts_node_is_null(bodyNode))
                {
                    if (allocatedTree != nullptr)
                    {
                        ts_tree_delete(allocatedTree);
                    }
                    continue;
                }

                ankerl::unordered_dense::set<std::string> reportedMissingMembers;

                uint32_t memberCount = ts_node_child_count(bodyNode);
                for (uint32_t m = 0; m < memberCount; ++m)
                {
                    TSNode mem = ts_node_child(bodyNode, m);
                    if (std::string_view(ts_node_type(mem)) != "func_declaration")
                    {
                        continue;
                    }

                    ankerl::unordered_dense::set<std::string> localNames;
                    TSNode paramsNode = GetChildByField(mem, "parameters");
                    if (!ts_node_is_null(paramsNode))
                    {
                        uint32_t pCount = ts_node_named_child_count(paramsNode);
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

                    TSNode funcBody = GetChildByField(mem, "body");
                    if (ts_node_is_null(funcBody))
                    {
                        continue;
                    }

                    std::vector<TSNode> stack;
                    stack.push_back(funcBody);
                    while (!stack.empty())
                    {
                        TSNode cur = stack.back();
                        stack.pop_back();

                        std::string_view curType = ts_node_type(cur);
                        if (curType == "variable_declaration")
                        {
                            uint32_t vCount = ts_node_named_child_count(cur);
                            for (uint32_t v = 0; v < vCount; ++v)
                            {
                                TSNode vChild = ts_node_named_child(cur, v);
                                if (std::string_view(ts_node_type(vChild)) == "variable_declarator")
                                {
                                    TSNode vdName = GetChildByField(vChild, "name");
                                    if (!ts_node_is_null(vdName))
                                    {
                                        localNames.insert(GetNodeText(vdName, mixinSource));
                                    }
                                }
                            }
                        }

                        uint32_t cCount = ts_node_child_count(cur);
                        for (uint32_t c = 0; c < cCount; ++c)
                        {
                            stack.push_back(ts_node_child(cur, c));
                        }
                    }

                    stack.clear();
                    stack.push_back(funcBody);
                    while (!stack.empty())
                    {
                        TSNode cur = stack.back();
                        stack.pop_back();

                        std::string_view curType = ts_node_type(cur);

                        if (curType == "member_expression")
                        {
                            TSNode objNode = GetChildByField(cur, "object");
                            TSNode propNode = GetChildByField(cur, "member");
                            if (ts_node_is_null(propNode))
                            {
                                propNode = GetChildByField(cur, "property");
                            }
                            if (!ts_node_is_null(objNode) && !ts_node_is_null(propNode))
                            {
                                std::string objName = GetNodeText(objNode, mixinSource);
                                std::string propName = GetNodeText(propNode, mixinSource);

                                if (objName == "this")
                                {
                                    if (!propName.empty() &&
                                        !hostMembers.contains(propName) &&
                                        !mixinSelfMembers.contains(propName) &&
                                        !reportedMissingMembers.contains(propName))
                                    {
                                        reportedMissingMembers.insert(propName);
                                        TSPoint sPoint = ts_node_start_point(propNode);
                                        TSPoint ePoint = ts_node_end_point(propNode);

                                        DiagnosticRelatedInformation rel;
                                        rel.fileUri = mixinSym->fileUri;
                                        rel.range = { sPoint.row, sPoint.column, ePoint.row, ePoint.column };
                                        rel.message = fmt::format("In mixin '{}': Member '{}'", mixinSym->name, propName);

                                        ctx.EmitWithRelated(
                                            hostIncStartLine, hostIncStartChar, hostIncEndLine, hostIncEndChar,
                                            diagnostics::codes::MixinInstantiationMemberNotFound,
                                            mixinSym->name, hostClassName, propName, hostClassName,
                                            rel, DiagnosticSeverity::Error);
                                    }
                                }
                            }
                        }
                        else if (curType == "scoped_identifier")
                        {
                            TSNode scopeChild = GetChildByField(cur, "scope");
                            TSNode nameChild = GetChildByField(cur, "name");
                            std::string scPrefix;
                            std::string nmText;

                            if (!ts_node_is_null(scopeChild) && !ts_node_is_null(nameChild))
                            {
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

                            if (scPrefix == "BaseClass")
                            {
                                if (!nmText.empty() &&
                                    !superMembers.contains(nmText) &&
                                    !reportedMissingMembers.contains(nmText))
                                {
                                    reportedMissingMembers.insert(nmText);
                                    TSPoint sPoint = ts_node_start_point(cur);
                                    TSPoint ePoint = ts_node_end_point(cur);

                                    DiagnosticRelatedInformation rel;
                                    rel.fileUri = mixinSym->fileUri;
                                    rel.range = { sPoint.row, sPoint.column, ePoint.row, ePoint.column };
                                    rel.message = fmt::format("In mixin '{}': Member '{}'", mixinSym->name, nmText);

                                    ctx.EmitWithRelated(
                                        hostIncStartLine, hostIncStartChar, hostIncEndLine, hostIncEndChar,
                                        diagnostics::codes::MixinInstantiationMemberNotFound,
                                        mixinSym->name, hostClassName, nmText, hostClassName,
                                        rel, DiagnosticSeverity::Error);
                                }
                            }
                        }
                        else if (curType == "call_expression")
                        {
                            TSNode fnNode = GetChildByField(cur, "function");
                            if (!ts_node_is_null(fnNode))
                            {
                                std::string_view fnType = ts_node_type(fnNode);
                                if (fnType == "identifier" || fnType == "scoped_identifier")
                                {
                                    std::string fnName = GetNodeText(fnNode, mixinSource);
                                    if (fnName.find("::") == std::string::npos &&
                                        !fnName.empty() &&
                                        !localNames.contains(fnName) &&
                                        !mixinSelfMembers.contains(fnName) &&
                                        !hostMembers.contains(fnName) &&
                                        !ctx.request.GetRuleIndex().allNames.contains(fnName) &&
                                        !IsKeyword(fnName) &&
                                        !IsPrimitiveTypeName(fnName) &&
                                        !reportedMissingMembers.contains(fnName))
                                    {
                                        reportedMissingMembers.insert(fnName);
                                        TSPoint sPoint = ts_node_start_point(fnNode);
                                        TSPoint ePoint = ts_node_end_point(fnNode);

                                        DiagnosticRelatedInformation rel;
                                        rel.fileUri = mixinSym->fileUri;
                                        rel.range = { sPoint.row, sPoint.column, ePoint.row, ePoint.column };
                                        rel.message = fmt::format("In mixin '{}': Member '{}'", mixinSym->name, fnName);

                                        ctx.EmitWithRelated(
                                            hostIncStartLine, hostIncStartChar, hostIncEndLine, hostIncEndChar,
                                            diagnostics::codes::MixinInstantiationMemberNotFound,
                                            mixinSym->name, hostClassName, fnName, hostClassName,
                                            rel, DiagnosticSeverity::Error);
                                    }
                                }
                            }
                        }

                        uint32_t cCount = ts_node_child_count(cur);
                        for (uint32_t c = 0; c < cCount; ++c)
                        {
                            stack.push_back(ts_node_child(cur, c));
                        }
                    }
                }

                if (allocatedTree != nullptr)
                {
                    ts_tree_delete(allocatedTree);
                }
            }
        }
    }

    bool HasInheritanceCycle(const std::string &typeName, const SymbolTable &table)
    {
        // The path being walked, not every type seen: revisiting a type through a second branch is
        // diamond inheritance, which is ordinary, while revisiting one already on the path is a
        // genuine cycle. Conflating the two reports every interface diamond as circular.
        std::vector<std::string> path;

        const auto walk = [&](const std::string &current, const auto &self) -> bool
        {
            if (std::find(path.begin(), path.end(), current) != path.end())
            {
                return true;
            }
            if (path.size() > 64)
            {
                // Depth this large means a malformed hierarchy; stop rather than recurse forever.
                return false;
            }

            path.push_back(current);

            const auto symbols = table.FindSymbolsPtr(current);
            if (symbols)
            {
                for (const auto &sym : *symbols)
                {
                    std::vector<std::string> bases;
                    if (sym.type == SymbolType::Class)
                    {
                        bases = sym.GetClass().bases;
                    }
                    else if (sym.type == SymbolType::Interface)
                    {
                        bases = sym.GetInterface().inheritedInterfaces;
                    }

                    for (const auto &base : bases)
                    {
                        const std::string cleanBase = CleanBaseType(base);
                        if (!cleanBase.empty() && self(cleanBase, self))
                        {
                            path.pop_back();
                            return true;
                        }
                    }
                }
            }

            path.pop_back();
            return false;
        };

        const auto symbols = table.FindSymbolsPtr(typeName);
        if (!symbols)
        {
            return false;
        }

        // Started from the bases rather than from typeName itself, so the cycle reported is one
        // that actually comes back round to it.
        for (const auto &sym : *symbols)
        {
            std::vector<std::string> bases;
            if (sym.type == SymbolType::Class)
            {
                bases = sym.GetClass().bases;
            }
            else if (sym.type == SymbolType::Interface)
            {
                bases = sym.GetInterface().inheritedInterfaces;
            }

            path.assign(1, typeName);
            for (const auto &base : bases)
            {
                const std::string cleanBase = CleanBaseType(base);
                if (!cleanBase.empty() && walk(cleanBase, walk))
                {
                    return true;
                }
            }
        }

        return false;
    }

    void ValidateClass(const Symbol &sym, const DiagnosticContext &ctx)
    {
        if (sym.type != SymbolType::Class && sym.type != SymbolType::Interface)
        {
            return;
        }

        if (IsFromPredefinedStub(sym, ctx))
        {
            return;
        }

        // A type named after a keyword or a built-in is reported and nothing else is: every later
        // rule would be describing a declaration the parser never really understood.
        const auto strType = ctx.request.GetStringTypeName();
        const auto arrType = ctx.request.GetArrayTypeName();
        const std::string_view effectiveStrType = strType.empty() ? std::string_view("string") : strType;
        const std::string_view effectiveArrType = arrType.empty() ? std::string_view("array") : arrType;
        if (IsReservedKeyword(sym.name) || IsPrimitiveTypeName(sym.name) ||
            sym.name == effectiveStrType || sym.name == effectiveArrType)
        {
            ctx.LogRule("ValidateClass", "as-err-reserved-keyword-name", sym);
            ctx.Emit(sym, "as-err-reserved-keyword-name", sym.name);
            return;
        }

        if (sym.type == SymbolType::Interface)
        {
            if (HasInheritanceCycle(sym.qualifiedName.empty() ? sym.name : sym.qualifiedName,
                                    ctx.request.symbolTable))
            {
                ctx.LogRule("ValidateClass", "as-err-circular-inherit", sym);
                ctx.Emit(sym, "as-err-circular-inherit", sym.name);
            }
            return;
        }

        const auto &sig = sym.GetClass();

        // A script cannot declare a template class - the application registers template types, and a
        // predefined stub is where it writes them down. Stubs never reach here at all, thanks to the
        // early return above, which is precisely the distinction the message draws.
        //
        // Reachable only since the grammar gained the production. Before that `class Holder<T>`
        // parsed with an ERROR node over the `<T>`, so all the user got was a generic syntax error
        // pointing at the angle brackets rather than at the class.
        if (sig.isTemplate)
        {
            ctx.LogRule("ValidateClass", "as-err-template-class-not-supported", sym);
            ctx.Emit(sym, "as-err-template-class-not-supported", sym.name);
        }

        CheckClassModifiers(sym, sig, ctx);
        CheckBases(sym, sig, ctx);
        CheckFinalOverrides(sym, sig, ctx);
        CheckInterfaceImplementation(sym, sig, ctx);
        CheckPropertyAccessors(sym, sig, ctx);
        CheckMixinInstantiations(sym, sig, ctx);
    }
}
