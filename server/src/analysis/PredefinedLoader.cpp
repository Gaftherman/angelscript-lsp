/**
 * @file PredefinedLoader.cpp
 * @brief Implementation of workspace predefined loader into AST SymbolTable.
 * @ingroup Analysis
 */

#include "PredefinedLoader.h"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <spdlog/fmt/fmt.h>
#include "utils/LspLogger.h"
#include "document/Document.h"
#include "analysis/SymbolCollector.h"

#ifdef max
#undef max
#endif

#ifdef min
#undef min
#endif

namespace analysis
{
    static std::string UrlDecode(std::string_view in)
    {
        std::string out;
        out.reserve(in.size());
        for (size_t i = 0; i < in.size(); ++i)
        {
            if (in[i] == '%' && i + 2 < in.size())
            {
                int hexVal = 0;
                std::stringstream ss;
                ss << std::hex << in.substr(i + 1, 2);
                if (ss >> hexVal)
                {
                    out += static_cast<char>(hexVal);
                    i += 2;
                    continue;
                }
            }
            out += in[i];
        }
        return out;
    }

    bool PredefinedLoader::LoadFromSource(std::string_view source, SymbolTable &table, std::string_view stringType, std::string_view arrayType, std::function<void(const std::string &, int)> logger, std::string_view customUri)
    {
        std::string uriStr = customUri.empty() ? "file:///as.predefined" : std::string(customUri);
        Document doc(uriStr, std::string(source));

        TSNode root = doc.RootNode();
        if (!ts_node_is_null(root))
        {
            auto checkSyntax = [&](auto self, TSNode node) -> void
            {
                if (ts_node_is_error(node) || ts_node_is_missing(node))
                {
                    TSPoint start = ts_node_start_point(node);
                    std::string_view sv = doc.SourceAt(node);
                    std::string msg = fmt::format("[Predefined] Syntax error in '{}' at line {}:{}, token: '{}'", uriStr, start.row + 1, start.column + 1, sv);
                    if (logger)
                    {
                        logger(msg, 1);
                    }
                    else
                    {
                        angel_lsp::utils::LspLogger::Error(msg);
                    }
                }
                uint32_t childCount = ts_node_child_count(node);
                for (uint32_t i = 0; i < childCount; ++i)
                {
                    self(self, ts_node_child(node, i));
                }
            };
            checkSyntax(checkSyntax, root);
        }

        SymbolCollector::CollectGlobals(doc, table);

        if (logger)
        {
            logger(fmt::format("[Predefined] Successfully extracted predefined symbols into SymbolTable from: '{}'", uriStr), 0);
        }

        return true;
    }

    bool PredefinedLoader::LoadFromFile(std::string_view filePath, SymbolTable &table, std::string_view stringType, std::string_view arrayType, std::function<void(const std::string &, int)> logger)
    {
        std::string pathStr(filePath);
        std::ifstream file(pathStr);
        if (!file.is_open())
        {
            return false;
        }

        std::stringstream buffer;
        buffer << file.rdbuf();

        std::string fileUri = pathStr;
        std::replace(fileUri.begin(), fileUri.end(), '/', '/');
        std::replace(fileUri.begin(), fileUri.end(), '\\', '/');
        if (fileUri.find("file://") != 0)
        {
            if (fileUri.empty() || fileUri[0] != '/')
            {
                fileUri = "/" + fileUri;
            }
            fileUri = "file://" + fileUri;
        }

        return LoadFromSource(buffer.str(), table, stringType, arrayType, logger, fileUri);
    }

    bool PredefinedLoader::FindInWorkspace(std::string_view rootUri, SymbolTable &table, std::string_view stringType, std::string_view arrayType, std::function<void(const std::string &, int)> logger)
    {
        if (rootUri.empty())
        {
            return false;
        }

        std::string pathStr(rootUri);
        if (pathStr.rfind("file:///", 0) == 0)
        {
            pathStr = pathStr.substr(8);
        }
        else if (pathStr.rfind("file://", 0) == 0)
        {
            pathStr = pathStr.substr(7);
        }

        pathStr = UrlDecode(pathStr);
        std::replace(pathStr.begin(), pathStr.end(), '/', '\\');

        std::vector<std::string> candidates = {
            "as.predefined",
            "game.as.predefined",
            "scripts/as.predefined",
            "script/as.predefined"};

        namespace fs = std::filesystem;
        fs::path rootPath(pathStr);

        if (!fs::exists(rootPath) || !fs::is_directory(rootPath))
        {
            return false;
        }

        bool loadedAny = false;

        for (const auto &cand : candidates)
        {
            fs::path candPath = rootPath / cand;
            if (fs::exists(candPath) && fs::is_regular_file(candPath))
            {
                if (LoadFromFile(candPath.string(), table, stringType, arrayType, logger))
                {
                    loadedAny = true;
                    if (logger)
                    {
                        logger(fmt::format("[Predefined] Loaded workspace predefined file: '{}'", candPath.string()), 0);
                    }
                }
            }
        }

        if (!loadedAny)
        {
            try
            {
                for (const auto &entry : fs::directory_iterator(rootPath))
                {
                    if (entry.is_regular_file())
                    {
                        std::string fname = entry.path().filename().string();
                        if (fname.ends_with(".as.predefined") || fname.ends_with(".predefined"))
                        {
                            if (LoadFromFile(entry.path().string(), table, stringType, arrayType, logger))
                            {
                                loadedAny = true;
                                if (logger)
                                {
                                    logger(fmt::format("[Predefined] Loaded workspace predefined file: '{}'", entry.path().string()), 0);
                                }
                            }
                        }
                    }
                }
            }
            catch (...)
            {
            }
        }

        return loadedAny;
    }

} // namespace analysis
