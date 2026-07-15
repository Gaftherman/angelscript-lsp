#pragma once
#include <iostream>
#include <unordered_map>
#include <string>
#include <memory>
#include "document/Document.h"
#include "analysis/ValidationOracle.h"
#include "analysis/SymbolTable.h"
#include <shared_mutex>
#include <thread>
#include <condition_variable>
#include <ankerl/unordered_dense.h>

// Forward declarations to avoid heavy includes in header
namespace lsp {
    class MessageHandler;
}

namespace angel_lsp {

class Server {
public:
    Server();
    ~Server();

    void Run();

private:
    void RegisterHandlers();
    
    // Background validation
    void ScheduleValidation(std::string uri, std::string text);
    void ValidationWorkerLoop(std::stop_token st);

    std::unique_ptr<lsp::MessageHandler> messageHandler;
    std::shared_mutex m_docMutex;
    ankerl::unordered_dense::map<std::string, std::unique_ptr<Document>> m_documents;
    ankerl::unordered_dense::map<std::string, analysis::SymbolTable> m_symbolTables;
    
    class asIScriptEngine* asEngine;
    std::unique_ptr<analysis::ValidationOracle> oracle;
    bool running = true;
    
    std::jthread m_validationThread;
    std::mutex m_validationMutex;
    std::condition_variable_any m_validationCV;
    bool m_validationPending = false;
    std::string m_pendingUri;
    std::string m_pendingText;
};

} // namespace angel_lsp
