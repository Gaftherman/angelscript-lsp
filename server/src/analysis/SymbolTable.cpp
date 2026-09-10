#include "SymbolTable.h"
#include "analysis/rules/RuleIndex.h"
#include "analysis/SemanticHelpers.h"
#include "analysis/OverloadResolver.h"
#include "utils/LspLogger.h"
#include "spdlog/fmt/fmt.h"

#include <algorithm>
#include <iterator>
#include <functional>

namespace angel_lsp::analysis
{
    namespace
    {
        std::string AccessModifierToString(AccessModifier access)
        {
            switch (access)
            {
            case AccessModifier::Public:
                return "public";
            case AccessModifier::Private:
                return "private";
            case AccessModifier::Protected:
                return "protected";
            default:
                return "public";
            }
        }

        std::string ParameterModifierToString(ParameterModifier mod)
        {
            switch (mod)
            {
            case ParameterModifier::In:
                return "&in";
            case ParameterModifier::Out:
                return "&out";
            case ParameterModifier::InOut:
                return "&inout";
            case ParameterModifier::None:
                return "";
            default:
                return "";
            }
        }

        std::string JoinStrings(const std::vector<std::string> &items, const char *sep = ", ")
        {
            std::string result;
            for (size_t i = 0; i < items.size(); ++i)
            {
                if (i > 0)
                    result += sep;
                result += items[i];
            }
            return result;
        }

        std::string JoinFlags(const std::vector<std::string> &flags)
        {
            if (flags.empty())
                return "[]";
            return "[" + JoinStrings(flags) + "]";
        }

        std::vector<Symbol> &MutableBucket(std::shared_ptr<std::vector<Symbol>> &bucket)
        {
            if (!bucket)
                bucket = std::make_shared<std::vector<Symbol>>();
            else if (bucket.use_count() > 1)
                bucket = std::make_shared<std::vector<Symbol>>(*bucket);
            return *bucket;
        }

        void BuildParamStrings(const std::vector<ParameterInformation> &params,
                               std::string &outParamsStr,
                               std::vector<std::string> &outParamLines)
        {
            outParamsStr = "(";
            for (size_t i = 0; i < params.size(); ++i)
            {
                const auto &param = params[i];
                if (i > 0)
                    outParamsStr += ", ";

                std::string modStr = ParameterModifierToString(param.modifier);
                std::string paramTypeStr = modStr.empty() ? param.typeName : param.typeName + " " + modStr;

                outParamsStr += paramTypeStr;
                if (!param.name.empty())
                    outParamsStr += " " + param.name;
                if (!param.defaultValue.empty())
                    outParamsStr += " = " + param.defaultValue;

                std::vector<std::string> pFlags;
                if (param.isConst)
                    pFlags.push_back("const: true");
                if (param.isHandle)
                    pFlags.push_back("handle: true");
                if (param.modifier == ParameterModifier::In)
                    pFlags.push_back("in: true");
                else if (param.modifier == ParameterModifier::Out)
                    pFlags.push_back("out: true");
                else if (param.modifier == ParameterModifier::InOut)
                    pFlags.push_back("inout: true");

                std::string defaultStr = param.defaultValue.empty()
                                             ? ""
                                             : fmt::format(" | Default: \"{}\"", param.defaultValue);

                outParamLines.push_back(fmt::format("      \u2514\u2500 Parameter: Type: \"{}\" | Name: \"{}\"{}  | Flags: {}",
                                                    paramTypeStr, param.name, defaultStr, JoinFlags(pFlags)));
            }
            outParamsStr += ")";
        }
    }

    void SymbolTable::IndexKeyForFileLocked(const std::string &fileUri, const std::string &key)
    {
        auto &keys = m_keysByFile[fileUri];
        // Linear, and deliberately: a document's symbols arrive grouped by declaration, so the key
        // just added is almost always the one being added again for the next overload.
        if (std::find(keys.begin(), keys.end(), key) == keys.end())
        {
            keys.push_back(key);
        }
    }

    void SymbolTable::EraseDocumentLocked(const std::string &fileUri)
    {
        const auto fileEntry = m_keysByFile.find(fileUri);
        if (fileEntry == m_keysByFile.end())
        {
            return;
        }

        for (const auto &key : fileEntry->second)
        {
            const auto bucket = m_symbols.find(key);
            if (bucket == m_symbols.end())
            {
                continue;
            }

            auto &vec = MutableBucket(bucket->second);
            std::erase_if(vec, [&fileUri](const Symbol &sym)
            {
                return sym.fileUri == fileUri || sym.isSynthesized;
            });

            if (vec.empty())
            {
                m_symbols.erase(bucket);
            }
        }

        m_keysByFile.erase(fileEntry);
    }

    void SymbolTable::AddSymbol(const Symbol &symbol)
    {
        std::unique_lock<std::shared_mutex> lock(m_mutex);
        const std::string &key = symbol.qualifiedName.empty() ? symbol.name : symbol.qualifiedName;
        MutableBucket(m_symbols[key]).push_back(symbol);
        IndexKeyForFileLocked(symbol.fileUri, key);
        ++m_version;
    }

    void SymbolTable::ClearDocumentSymbols(const std::string &fileUri)
    {
        std::unique_lock<std::shared_mutex> lock(m_mutex);
        EraseDocumentLocked(fileUri);
        ++m_version;
    }

    void SymbolTable::ReplaceDocumentSymbols(const std::string &fileUri, const SymbolTable &staging)
    {
        // Snapshot staging first: it is a separate object with its own lock, and reading it while
        // holding this table's write lock is what keeps the replacement a single atomic step.
        std::vector<Symbol> fresh;
        staging.ForEachSymbol(
            [&fresh](const std::string &, const std::vector<Symbol> &symbols)
            {
                fresh.insert(fresh.end(), symbols.begin(), symbols.end());
            });

        PublishDocumentSymbols(fileUri, std::move(fresh));
    }

    void SymbolTable::ReplaceDocumentSymbols(const std::string &fileUri, SymbolTable &&staging)
    {
        std::vector<Symbol> fresh;

        // Read without taking staging's lock, and through its members rather than ForEachSymbol:
        // an rvalue reference is the caller stating that nothing else holds this table, which is
        // exactly the condition that makes both of those safe. It is left empty.
        for (auto &[key, bucket] : staging.m_symbols)
        {
            if (!bucket)
                continue;

            std::vector<Symbol> &symbols = MutableBucket(bucket);
            fresh.insert(fresh.end(),
                         std::make_move_iterator(symbols.begin()),
                         std::make_move_iterator(symbols.end()));
        }
        staging.m_symbols.clear();

        PublishDocumentSymbols(fileUri, std::move(fresh));
    }

    void SymbolTable::PublishDocumentSymbols(const std::string &fileUri, std::vector<Symbol> &&fresh)
    {
        std::unique_lock<std::shared_mutex> lock(m_mutex);

        // Check if the document being erased contains any mixin classes
        bool erasedMixin = false;
        const auto fileEntry = m_keysByFile.find(fileUri);
        if (fileEntry != m_keysByFile.end())
        {
            for (const auto &key : fileEntry->second)
            {
                const auto bucket = m_symbols.find(key);
                if (bucket != m_symbols.end() && bucket->second)
                {
                    for (const auto &sym : *bucket->second)
                    {
                        if (sym.fileUri == fileUri && sym.type == SymbolType::Class &&
                            std::holds_alternative<ClassSignature>(sym.signature) &&
                            sym.GetClass().modifiers.isMixin)
                        {
                            erasedMixin = true;
                            break;
                        }
                    }
                }
                if (erasedMixin)
                {
                    break;
                }
            }
        }

        EraseDocumentLocked(fileUri);

        bool addedMixin = false;
        std::vector<std::string> freshClassKeys;
        for (auto &symbol : fresh)
        {
            const std::string &key = symbol.qualifiedName.empty() ? symbol.name : symbol.qualifiedName;

            if (symbol.type == SymbolType::Class && std::holds_alternative<ClassSignature>(symbol.signature))
            {
                if (symbol.GetClass().modifiers.isMixin)
                {
                    addedMixin = true;
                }
                freshClassKeys.push_back(key);
            }

            // Indexed before the move, not after: the index is keyed by the symbol's own file, not
            // by the argument - a symbol filed under a different URI would otherwise be indexed
            // here and never erased by its own document's clear - and after the move the symbol
            // that knew which file that was is gone.
            IndexKeyForFileLocked(symbol.fileUri, key);
            MutableBucket(m_symbols[key]).push_back(std::move(symbol));
        }

        if (erasedMixin || addedMixin)
        {
            // A mixin class was added or removed; existing classes across the entire workspace might be affected
            ResolveIncludedMixinsLocked();
        }
        else if (!freshClassKeys.empty())
        {
            // Only resolve included mixins for the classes declared in this document
            ResolveIncludedMixinsForKeysLocked(freshClassKeys);
        }

        ++m_version;
    }

    void SymbolTable::ResolveIncludedMixinsForKeysLocked(const std::vector<std::string> &classKeys)
    {
        if (classKeys.empty())
        {
            return;
        }

        // 1. Resolve includedMixins on class signatures and collect which mixins are needed
        ankerl::unordered_dense::set<std::string> neededMixins;

        for (const auto &key : classKeys)
        {
            auto it = m_symbols.find(key);
            if (it == m_symbols.end() || !it->second)
            {
                continue;
            }

            std::vector<Symbol> &symbols = MutableBucket(it->second);
            for (auto &sym : symbols)
            {
                if (sym.type == SymbolType::Class && std::holds_alternative<ClassSignature>(sym.signature))
                {
                    auto &sig = std::get<ClassSignature>(sym.signature);
                    sig.includedMixins.clear();
                    for (const auto &b : sig.bases)
                    {
                        std::string clean = CleanBaseType(b);
                        auto baseIt = m_symbols.find(clean);
                        if (baseIt != m_symbols.end() && baseIt->second)
                        {
                            for (const auto &cand : *baseIt->second)
                            {
                                if (cand.type == SymbolType::Class &&
                                    std::holds_alternative<ClassSignature>(cand.signature) &&
                                    cand.GetClass().modifiers.isMixin)
                                {
                                    sig.includedMixins.push_back(clean);
                                    neededMixins.insert(clean);
                                    break;
                                }
                            }
                        }
                    }
                }
            }
        }

        // 2. Clean up previously synthesized symbols for these host classes
        for (const auto &key : classKeys)
        {
            auto it = m_symbols.find(key);
            if (it == m_symbols.end() || !it->second)
            {
                continue;
            }
            for (const auto &sym : *it->second)
            {
                if (sym.type == SymbolType::Class)
                {
                    const std::string hostQName = sym.qualifiedName.empty() ? sym.name : sym.qualifiedName;
                    const auto fileIt = m_keysByFile.find(sym.fileUri);
                    if (fileIt != m_keysByFile.end())
                    {
                        const std::string prefix = hostQName + "::";
                        for (const auto &k : fileIt->second)
                        {
                            if (k.starts_with(prefix))
                            {
                                auto bIt = m_symbols.find(k);
                                if (bIt != m_symbols.end() && bIt->second)
                                {
                                    auto &vec = MutableBucket(bIt->second);
                                    std::erase_if(vec, [&](const Symbol &s)
                                    {
                                        return s.isSynthesized;
                                    });
                                }
                            }
                        }
                    }
                }
            }
        }

        if (neededMixins.empty())
        {
            return;
        }

        // 3. Collect non-synthesized member functions of the needed mixins
        ankerl::unordered_dense::map<std::string, std::vector<Symbol>> mixinMembers;
        for (const auto &[k, bucket] : m_symbols)
        {
            if (!bucket)
            {
                continue;
            }
            for (const auto &sym : *bucket)
            {
                if (!sym.isSynthesized && sym.type == SymbolType::Function &&
                    !sym.containerName.empty() && neededMixins.contains(sym.containerName))
                {
                    mixinMembers[sym.containerName].push_back(sym);
                }
            }
        }

        // 4. Synthesize mixin member functions into each host class
        for (const auto &key : classKeys)
        {
            auto it = m_symbols.find(key);
            if (it == m_symbols.end() || !it->second)
            {
                continue;
            }

            for (const auto &classSym : *it->second)
            {
                if (classSym.type != SymbolType::Class || !std::holds_alternative<ClassSignature>(classSym.signature))
                {
                    continue;
                }

                const auto &sig = std::get<ClassSignature>(classSym.signature);
                if (sig.includedMixins.empty())
                {
                    continue;
                }

                const std::string hostQName = classSym.qualifiedName.empty() ? classSym.name : classSym.qualifiedName;
                const std::string &hostFileUri = classSym.fileUri;

                for (const auto &mixinName : sig.includedMixins)
                {
                    auto mIt = mixinMembers.find(mixinName);
                    if (mIt == mixinMembers.end())
                    {
                        continue;
                    }

                    for (const auto &mSym : mIt->second)
                    {
                        const std::string synthKey = hostQName + "::" + mSym.name;

                        // Check if already declared or synthesized in host class
                        auto existingBucketIt = m_symbols.find(synthKey);
                        bool alreadyPresent = false;
                        if (existingBucketIt != m_symbols.end() && existingBucketIt->second)
                        {
                            for (const auto &existingSym : *existingBucketIt->second)
                            {
                                if (mSym.type == SymbolType::Function && existingSym.type == SymbolType::Function)
                                {
                                    if (HasSameParameterList(existingSym, mSym))
                                    {
                                        alreadyPresent = true;
                                        break;
                                    }
                                }
                                else
                                {
                                    alreadyPresent = true;
                                    break;
                                }
                            }
                        }

                        if (alreadyPresent)
                        {
                            continue;
                        }

                        Symbol synth = mSym;
                        // Retain originating containerName so callers know original source
                        synth.containerName = mSym.containerName.empty() ? mixinName : mSym.containerName;
                        synth.qualifiedName = synthKey;
                        // Retain original mixin fileUri and ranges so Go-to-Definition navigates to the mixin source
                        synth.fileUri = mSym.fileUri;
                        synth.isSynthesized = true;

                        MutableBucket(m_symbols[synthKey]).push_back(std::move(synth));
                        IndexKeyForFileLocked(hostFileUri, synthKey);
                    }
                }
            }
        }
    }

    void SymbolTable::ResolveIncludedMixinsLocked()
    {
        std::vector<std::string> classKeys;
        for (const auto &[name, bucket] : m_symbols)
        {
            if (!bucket)
            {
                continue;
            }
            for (const auto &sym : *bucket)
            {
                if (sym.type == SymbolType::Class && std::holds_alternative<ClassSignature>(sym.signature))
                {
                    classKeys.push_back(name);
                    break;
                }
            }
        }
        ResolveIncludedMixinsForKeysLocked(classKeys);
    }

    void SymbolTable::ResolveIncludedMixins()
    {
        std::unique_lock<std::shared_mutex> lock(m_mutex);
        ResolveIncludedMixinsLocked();
        ++m_version;
    }


    static inline std::string_view CleanScope(const std::string &name)
    {
        if (name.rfind("::", 0) == 0)
            return std::string_view(name).substr(2);
        return name;
    }

    bool SymbolTable::HasSymbol(const std::string &qualifiedName) const
    {
        std::shared_lock<std::shared_mutex> lock(m_mutex);
        std::string_view search = CleanScope(qualifiedName);
        return m_symbols.contains(search);
    }

    std::shared_ptr<const std::vector<Symbol>> SymbolTable::FindSymbolsPtr(const std::string &qualifiedName) const
    {
        std::shared_lock<std::shared_mutex> lock(m_mutex);
        std::string_view search = CleanScope(qualifiedName);
        auto it = m_symbols.find(search);
        return it != m_symbols.end() ? it->second : nullptr;
    }

    std::vector<Symbol> SymbolTable::FindSymbols(const std::string &qualifiedName) const
    {
        std::shared_lock<std::shared_mutex> lock(m_mutex);
        std::string_view search = CleanScope(qualifiedName);
        auto it = m_symbols.find(search);
        return it != m_symbols.end() ? *it->second : std::vector<Symbol>{};
    }

    std::optional<Symbol> SymbolTable::FindFirstSymbol(const std::string &qualifiedName) const
    {
        std::shared_lock<std::shared_mutex> lock(m_mutex);
        std::string_view search = CleanScope(qualifiedName);
        auto it = m_symbols.find(search);
        if (it != m_symbols.end() && !it->second->empty())
            return it->second->front();
        return std::nullopt;
    }

    std::optional<Symbol> SymbolTable::LookupSymbol(const std::string &name) const
    {
        return FindFirstSymbol(name);
    }

    std::vector<Symbol> SymbolTable::FindTypeSymbolsByShortName(const std::string &shortName) const
    {
        std::vector<Symbol> result;
        const auto index = GetRuleIndex();
        if (!index)
        {
            return result;
        }

        const auto it = index->qualifiedTypesByShortName.find(shortName);
        if (it != index->qualifiedTypesByShortName.end())
        {
            for (const auto &qKey : it->second)
            {
                auto ptr = FindSymbolsPtr(qKey);
                if (ptr)
                {
                    result.insert(result.end(), ptr->begin(), ptr->end());
                }
            }
        }
        return result;
    }

    void SymbolTable::InsertSymbol(const std::string &name, SymbolKind kind, const std::string &type)
    {
        Symbol sym;
        sym.name = name;
        sym.type = kind;
        VariableSignature varSig;
        varSig.typeName = type;
        varSig.baseTypeName = type;
        sym.signature = varSig;
        AddSymbol(sym);
    }

    bool SymbolTable::HasSymbolAnywhere(const std::string &name) const
    {
        std::string searchName = name;
        if (searchName.rfind("::", 0) == 0)
            searchName = searchName.substr(2);

        {
            std::shared_lock<std::shared_mutex> lock(m_mutex);

            // Buckets are keyed by qualified name, so these two probes already answer every
            // qualifiedName match the linear scan below used to make.
            if (m_symbols.find(searchName) != m_symbols.end() || m_symbols.find(name) != m_symbols.end())
                return true;
        }

        // What the probes above cannot answer is a match on a symbol's *short* name when it is
        // filed under a qualified one. That used to be a scan of every bucket and every symbol in
        // the workspace - and CallChecker asks this once per declarator. RuleIndex::allNames holds
        // exactly that set and is rebuilt only when the table's version moves.
        //
        // Deliberately outside the shared lock: GetRuleIndex() re-enters ForEachSymbol, which takes
        // the same non-recursive shared_mutex, and a writer arriving between the two acquisitions
        // would deadlock. See the note on ForEachSymbol.
        const auto index = GetRuleIndex();
        return index && (index->allNames.contains(searchName) || index->allNames.contains(name));
    }

    void SymbolTable::ForEachSymbol(const std::function<void(const std::string &, const std::vector<Symbol> &)> &visitor) const
    {
        // The buckets are snapshotted under the lock and visited outside it, which costs one vector
        // of (name, shared_ptr) pairs and buys freedom from a deadlock that had already been built
        // in: a visitor that looks another symbol up - and the declaration rules do that constantly
        // - re-enters FindSymbolsPtr while this shared lock is still held. std::shared_mutex is
        // writer-preferring, so the moment the workspace scanner is waiting to write, that second
        // shared_lock blocks behind it and the two threads wait on each other for good. Holding
        // shared_ptr copies also keeps every bucket alive for the whole walk even if a writer
        // replaces it mid-visit.
        std::vector<std::pair<std::string, std::shared_ptr<const std::vector<Symbol>>>> snapshot;
        {
            std::shared_lock<std::shared_mutex> lock(m_mutex);
            snapshot.reserve(m_symbols.size());
            for (const auto &[key, symbols] : m_symbols)
                snapshot.emplace_back(key, symbols);
        }

        for (const auto &[key, symbols] : snapshot)
            visitor(key, *symbols);
    }

    std::vector<Symbol> SymbolTable::GetAllSymbols() const
    {
        std::vector<Symbol> result;
        ForEachSymbol([&result](const std::string &, const std::vector<Symbol> &syms)
        {
            result.insert(result.end(), syms.begin(), syms.end());
        });
        return result;
    }

    void SymbolTable::ForEachSymbolInFile(const std::string &fileUri,
                                          const std::function<void(const std::string &, const std::vector<Symbol> &)> &visitor) const
    {
        // Snapshotted outside the lock for the same reason ForEachSymbol does it: the declaration
        // rules look other symbols up from inside the visitor.
        std::vector<std::pair<std::string, std::shared_ptr<const std::vector<Symbol>>>> snapshot;
        {
            std::shared_lock<std::shared_mutex> lock(m_mutex);
            const auto fileEntry = m_keysByFile.find(fileUri);
            if (fileEntry == m_keysByFile.end())
            {
                return;
            }

            snapshot.reserve(fileEntry->second.size());
            for (const auto &key : fileEntry->second)
            {
                const auto bucket = m_symbols.find(key);
                if (bucket != m_symbols.end())
                {
                    snapshot.emplace_back(key, bucket->second);
                }
            }
        }

        for (const auto &[key, symbols] : snapshot)
            visitor(key, *symbols);
    }

    uint64_t SymbolTable::Version() const
    {
        std::shared_lock<std::shared_mutex> lock(m_mutex);
        return m_version;
    }

    std::shared_ptr<const rules::RuleIndex> SymbolTable::GetRuleIndex() const
    {
        const uint64_t version = Version();

        std::lock_guard<std::mutex> guard(m_ruleIndexMutex);
        if (!m_ruleIndex || m_ruleIndexVersion != version)
        {
            // Built without the table's lock held: RuleIndex::Build walks the table itself.
            m_ruleIndex = rules::RuleIndex::Build(*this);
            m_ruleIndexVersion = version;
        }
        return m_ruleIndex;
    }

    void SymbolTable::PrintSymbols(angel_lsp::utils::LspLogger *logger) const
    {
        if (!logger)
            return;

        std::shared_lock<std::shared_mutex> lock(m_mutex);
        logger->LogInfo(fmt::format("=== SYMBOL TABLE ({} unique keys) ===", m_symbols.size()));

        for (const auto &[key, symbols] : m_symbols)
        {
            for (const auto &sym : *symbols)
            {
                const std::string typeStr = SymbolTypeToString(sym.type);
                const std::string nameStr = sym.qualifiedName.empty() ? sym.name : sym.qualifiedName;
                const std::string range = fmt::format("[L{}:C{}-L{}:C{}]",
                                                      sym.startLine + 1, sym.startCharacter + 1,
                                                      sym.endLine + 1, sym.endCharacter + 1);

                switch (sym.type)
                {
                case SymbolType::Function:
                {
                    const auto &sig = sym.GetFunction();
                    std::vector<std::string> flags;
                    if (sig.modifiers.isShared)
                        flags.push_back("shared");
                    if (sig.modifiers.isOverride)
                        flags.push_back("override");
                    if (sig.modifiers.isFinal)
                        flags.push_back("final");
                    if (sig.modifiers.isReturnReference)
                        flags.push_back("ref_return");
                    if (sig.modifiers.isDelete)
                        flags.push_back("delete");
                    if (sig.modifiers.isExternal)
                        flags.push_back("external");
                    if (sig.modifiers.isExplicit)
                        flags.push_back("explicit");

                    std::string paramsStr;
                    std::vector<std::string> paramLines;
                    BuildParamStrings(sig.parameters, paramsStr, paramLines);

                    logger->LogInfo(fmt::format("  \u2022 [{}] Access: {} | Ret: \"{}\" | Name: \"{}\" | Params: \"{}\" | Flags: {} | {}",
                                                typeStr,
                                                AccessModifierToString(sig.modifiers.access),
                                                sig.returnType, nameStr, paramsStr,
                                                JoinFlags(flags), range));

                    for (const auto &line : paramLines)
                        logger->LogInfo(line);

                    break;
                }
                case SymbolType::Variable:
                {
                    const auto &sig = sym.GetVariable();
                    std::vector<std::string> flags;
                    if (sig.modifiers.isShared)
                        flags.push_back("shared");
                    if (sig.modifiers.isConst)
                        flags.push_back("const");
                    if (sig.modifiers.isHandle)
                        flags.push_back("handle");

                    std::string defaultStr = sig.defaultValue.empty()
                                                 ? ""
                                                 : fmt::format(" | Default: \"{}\"", sig.defaultValue);

                    logger->LogInfo(fmt::format("  \u2022 [{}] Access: {} | Type: \"{}\" | Name: \"{}\"{}  | Flags: {} | {}",
                                                typeStr,
                                                AccessModifierToString(sig.modifiers.access),
                                                sig.typeName, nameStr, defaultStr,
                                                JoinFlags(flags), range));
                    break;
                }
                case SymbolType::Class:
                {
                    const auto &sig = sym.GetClass();
                    std::vector<std::string> flags;
                    if (sig.modifiers.isShared)
                        flags.push_back("shared");
                    if (sig.modifiers.isMixin)
                        flags.push_back("mixin");
                    if (sig.modifiers.isAbstract)
                        flags.push_back("abstract");
                    if (sig.modifiers.isFinal)
                        flags.push_back("final");

                    std::string basesStr = sig.bases.empty()
                                               ? ""
                                               : fmt::format(" | Bases: \"{}\"", JoinStrings(sig.bases));

                    logger->LogInfo(fmt::format("  \u2022 [{}] Access: {} | Name: \"{}\"{}  | Flags: {} | {}",
                                                typeStr,
                                                AccessModifierToString(sig.modifiers.access),
                                                nameStr, basesStr, JoinFlags(flags), range));
                    break;
                }
                case SymbolType::Interface:
                {
                    const auto &sig = sym.GetInterface();
                    std::vector<std::string> flags;
                    if (sig.modifiers.isShared)
                        flags.push_back("shared");

                    std::string basesStr = sig.inheritedInterfaces.empty()
                                               ? ""
                                               : fmt::format(" | Extends: \"{}\"", JoinStrings(sig.inheritedInterfaces));

                    logger->LogInfo(fmt::format("  \u2022 [{}] Name: \"{}\"{}  | Flags: {} | {}",
                                                typeStr, nameStr, basesStr, JoinFlags(flags), range));
                    break;
                }
                case SymbolType::Enum:
                {
                    const auto &sig = sym.GetEnum();
                    std::vector<std::string> flags;
                    if (sig.modifiers.isShared)
                        flags.push_back("shared");

                    logger->LogInfo(fmt::format("  \u2022 [{}] Name: \"{}\" | Flags: {} | {}",
                                                typeStr, nameStr, JoinFlags(flags), range));
                    break;
                }
                case SymbolType::Typedef:
                {
                    logger->LogInfo(fmt::format("  \u2022 [{}] Name: \"{}\" | {}",
                                                typeStr, nameStr, range));
                    break;
                }
                case SymbolType::Funcdef:
                {
                    logger->LogInfo(fmt::format("  \u2022 [{}] Name: \"{}\" | {}",
                                                typeStr, nameStr, range));
                    break;
                }
                case SymbolType::Property:
                {
                    logger->LogInfo(fmt::format("  \u2022 [{}] Name: \"{}\" | {}",
                                                typeStr, nameStr, range));
                    break;
                }
                case SymbolType::Namespace:
                {
                    logger->LogInfo(fmt::format("  \u2022 [{}] Name: \"{}\" | {}",
                                                typeStr, nameStr, range));
                    break;
                }
                case SymbolType::CallReference:
                {
                    const auto &sig = sym.GetCallReference();
                    std::string objStr = sig.objectExpression.empty()
                                             ? ""
                                             : fmt::format(" | Object: \"{}\"", sig.objectExpression);
                    logger->LogInfo(fmt::format("  \u2022 [{}] Callee: \"{}\"{}  | Method: {} | {}",
                                                typeStr, sig.calleeName, objStr,
                                                sig.isMethodCall ? "true" : "false", range));
                    break;
                }
                default:
                {
                    logger->LogInfo(fmt::format("  \u2022 [{}] Name: \"{}\" | {}",
                                                typeStr, nameStr, range));
                    break;
                }
                }
            }
        }
    }

    namespace
    {
        inline void HashCombine(uint64_t &h, uint64_t val)
        {
            h ^= val + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        }

        inline void HashString(uint64_t &h, std::string_view s)
        {
            uint64_t strHash = ankerl::unordered_dense::hash<std::string_view>{}(s);
            HashCombine(h, strHash);
        }

        inline void HashModifiers(uint64_t &h, const SymbolModifiers &m)
        {
            uint32_t flags = static_cast<uint32_t>(m.access)
                | (static_cast<uint32_t>(m.isConst) << 2)
                | (static_cast<uint32_t>(m.isHandle) << 3)
                | (static_cast<uint32_t>(m.isShared) << 4)
                | (static_cast<uint32_t>(m.isMixin) << 5)
                | (static_cast<uint32_t>(m.isAbstract) << 6)
                | (static_cast<uint32_t>(m.isFinal) << 7)
                | (static_cast<uint32_t>(m.isOverride) << 8)
                | (static_cast<uint32_t>(m.isExplicit) << 9)
                | (static_cast<uint32_t>(m.isProperty) << 10)
                | (static_cast<uint32_t>(m.isDelete) << 11)
                | (static_cast<uint32_t>(m.isExternal) << 12)
                | (static_cast<uint32_t>(m.isReturnReference) << 13);
            HashCombine(h, flags);
        }

        inline void HashParameter(uint64_t &h, const ParameterInformation &p)
        {
            HashString(h, p.name);
            HashString(h, p.typeName);
            HashCombine(h, static_cast<uint64_t>(p.typeKind));
            uint32_t flags = (p.isArray ? 1 : 0)
                | ((p.isConst ? 1 : 0) << 1)
                | ((p.isReference ? 1 : 0) << 2)
                | ((p.isHandle ? 1 : 0) << 3)
                | (static_cast<uint32_t>(p.modifier) << 4);
            HashCombine(h, flags);
            HashString(h, p.defaultValue);
        }

        void HashSymbolInterface(uint64_t &h, const Symbol &sym)
        {
            HashCombine(h, static_cast<uint64_t>(sym.type));
            HashString(h, sym.name);
            HashString(h, sym.containerName);
            HashString(h, sym.qualifiedName);

            if (std::holds_alternative<FunctionSignature>(sym.signature))
            {
                const auto &fn = std::get<FunctionSignature>(sym.signature);
                HashString(h, fn.returnType);
                HashCombine(h, static_cast<uint64_t>(fn.returnTypeKind));
                uint32_t fnFlags = (fn.returnIsArray ? 1 : 0)
                    | ((fn.returnIsConst ? 1 : 0) << 1)
                    | ((fn.isInterfaceMethod ? 1 : 0) << 2);
                HashCombine(h, fnFlags);
                HashModifiers(h, fn.modifiers);
                HashCombine(h, fn.parameters.size());
                for (const auto &p : fn.parameters)
                {
                    HashParameter(h, p);
                }
            }
            else if (std::holds_alternative<VariableSignature>(sym.signature))
            {
                const auto &v = std::get<VariableSignature>(sym.signature);
                HashString(h, v.typeName);
                HashCombine(h, static_cast<uint64_t>(v.typeKind));
                uint32_t vFlags = (v.isArray ? 1 : 0)
                    | ((v.isVirtualProperty ? 1 : 0) << 1)
                    | ((v.hasGet ? 1 : 0) << 2)
                    | ((v.hasSet ? 1 : 0) << 3);
                HashCombine(h, vFlags);
                HashModifiers(h, v.modifiers);
            }
            else if (std::holds_alternative<ClassSignature>(sym.signature))
            {
                const auto &cls = std::get<ClassSignature>(sym.signature);
                HashModifiers(h, cls.modifiers);
                for (const auto &b : cls.bases)
                {
                    HashString(h, b);
                }
                for (const auto &m : cls.includedMixins)
                {
                    HashString(h, m);
                }
                for (const auto &t : cls.templateParams)
                {
                    HashString(h, t);
                }
            }
            else if (std::holds_alternative<InterfaceSignature>(sym.signature))
            {
                const auto &iface = std::get<InterfaceSignature>(sym.signature);
                HashModifiers(h, iface.modifiers);
                for (const auto &b : iface.inheritedInterfaces)
                {
                    HashString(h, b);
                }
            }
            else if (std::holds_alternative<EnumSignature>(sym.signature))
            {
                const auto &enm = std::get<EnumSignature>(sym.signature);
                HashModifiers(h, enm.modifiers);
                for (const auto &m : enm.members)
                {
                    HashString(h, m.name);
                    HashString(h, m.value);
                }
            }
            else if (std::holds_alternative<TypedefSignature>(sym.signature))
            {
                const auto &td = std::get<TypedefSignature>(sym.signature);
                HashString(h, td.baseType);
                HashCombine(h, static_cast<uint64_t>(td.typeKind));
            }
            else if (std::holds_alternative<FuncdefSignature>(sym.signature))
            {
                const auto &fd = std::get<FuncdefSignature>(sym.signature);
                HashString(h, fd.returnType);
                HashCombine(h, static_cast<uint64_t>(fd.returnTypeKind));
                HashModifiers(h, fd.modifiers);
                HashCombine(h, fd.parameters.size());
                for (const auto &p : fd.parameters)
                {
                    HashParameter(h, p);
                }
            }
        }
    }

    uint64_t SymbolTable::ComputeDocumentInterfaceHash(const std::string &fileUri) const
    {
        std::shared_lock<std::shared_mutex> lock(m_mutex);
        return ComputeDocumentInterfaceHashLocked(fileUri);
    }

    uint64_t SymbolTable::ComputeDocumentInterfaceHashLocked(const std::string &fileUri) const
    {
        const auto fileEntry = m_keysByFile.find(fileUri);
        if (fileEntry == m_keysByFile.end())
        {
            return 0;
        }

        std::vector<std::string> keys = fileEntry->second;
        std::sort(keys.begin(), keys.end());

        uint64_t h = 0xcbf29ce484222325ULL;
        for (const auto &key : keys)
        {
            const auto bucket = m_symbols.find(key);
            if (bucket == m_symbols.end() || !bucket->second)
            {
                continue;
            }

            std::vector<const Symbol *> docSymbols;
            for (const auto &sym : *bucket->second)
            {
                if (sym.fileUri == fileUri)
                {
                    docSymbols.push_back(&sym);
                }
            }

            std::sort(docSymbols.begin(), docSymbols.end(), [](const Symbol *a, const Symbol *b)
            {
                if (a->type != b->type)
                {
                    return a->type < b->type;
                }
                if (std::holds_alternative<FunctionSignature>(a->signature) &&
                    std::holds_alternative<FunctionSignature>(b->signature))
                {
                    const auto &fnA = std::get<FunctionSignature>(a->signature);
                    const auto &fnB = std::get<FunctionSignature>(b->signature);
                    if (fnA.parameters.size() != fnB.parameters.size())
                    {
                        return fnA.parameters.size() < fnB.parameters.size();
                    }
                    for (size_t i = 0; i < fnA.parameters.size(); ++i)
                    {
                        if (fnA.parameters[i].typeName != fnB.parameters[i].typeName)
                        {
                            return fnA.parameters[i].typeName < fnB.parameters[i].typeName;
                        }
                    }
                }
                return a->name < b->name;
            });

            for (const auto *sym : docSymbols)
            {
                HashSymbolInterface(h, *sym);
            }
        }

        return h;
    }
}