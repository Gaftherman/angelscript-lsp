#include <doctest/doctest.h>

#if __has_include(<angelscript.h>)

#include "features/breakpoints/BreakpointLocationsHandler.h"
#include "helpers/TestUtils.h"
#include "parser/AngelScriptParser.h"

#include <angel_lsp/angel_lsp_debugger.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

using namespace angel_lsp;
using namespace angel_lsp::features;
using namespace angel_lsp::parser;

TEST_CASE("Debugger DAP Protocol - Request Dispatcher Lifecycle")
{
    angel_lsp::debug::Debugger debugger;

    // 1. initialize request
    const std::string initReq =
        "{\"seq\":1,\"type\":\"request\",\"command\":\"initialize\",\"arguments\":{\"clientID\":\"vscode\"}}";
    std::string initResp = debugger.HandleDapRequest(initReq);
    CHECK(initResp.find("\"success\":true") != std::string::npos);
    CHECK(initResp.find("\"supportsConfigurationDoneRequest\":true") != std::string::npos);
    CHECK(initResp.find("\"supportsEvaluateForHovers\":true") != std::string::npos);
    CHECK(initResp.find("\"supportsConditionalBreakpoints\":true") != std::string::npos);
    CHECK(initResp.find("\"supportsHitConditionalBreakpoints\":true") != std::string::npos);
    CHECK(initResp.find("\"supportsLogPoints\":true") != std::string::npos);
    CHECK(initResp.find("\"supportsReadMemoryRequest\":true") != std::string::npos);
    CHECK(initResp.find("\"supportsDisassembleRequest\":true") != std::string::npos);

    // 2. attach request
    const std::string attachReq =
        "{\"seq\":2,\"type\":\"request\",\"command\":\"attach\",\"arguments\":{\"port\":9002}}";
    std::string attachResp = debugger.HandleDapRequest(attachReq);
    CHECK(attachResp.find("\"success\":true") != std::string::npos);

    // 3. setBreakpoints request
    const std::string section = "scripts/" + test::GenerateRandomSymbolName("Weapon") + ".as";
    const std::string setBpReq =
        "{\"seq\":3,\"type\":\"request\",\"command\":\"setBreakpoints\",\"arguments\":{\"source\":{\"path\":\"" +
        section +
        "\"},\"breakpoints\":[{\"line\":10,\"condition\":\"ammo <= 0\",\"hitCondition\":\">= 3\"},"
        "{\"line\":25,\"logMessage\":\"item is {item}\"}]}}";
    std::string setBpResp = debugger.HandleDapRequest(setBpReq);
    CHECK(setBpResp.find("\"success\":true") != std::string::npos);
    CHECK(setBpResp.find("\"verified\":true") != std::string::npos);

    // 4. setExceptionBreakpoints request
    const std::string setExReq = "{\"seq\":4,\"type\":\"request\",\"command\":\"setExceptionBreakpoints\","
                                 "\"arguments\":{\"filters\":[\"all\"]}}";
    std::string setExResp = debugger.HandleDapRequest(setExReq);
    CHECK(setExResp.find("\"success\":true") != std::string::npos);
    CHECK(debugger.GetBreakOnException());

    // 5. configurationDone request
    const std::string confDoneReq = "{\"seq\":5,\"type\":\"request\",\"command\":\"configurationDone\"}";
    std::string confDoneResp = debugger.HandleDapRequest(confDoneReq);
    CHECK(confDoneResp.find("\"success\":true") != std::string::npos);

    // 6. threads request
    const std::string threadsReq = "{\"seq\":6,\"type\":\"request\",\"command\":\"threads\"}";
    std::string threadsResp = debugger.HandleDapRequest(threadsReq);
    CHECK(threadsResp.find("\"success\":true") != std::string::npos);
    CHECK(threadsResp.find("AngelScript Main Thread") != std::string::npos);

    // 7. scopes request
    const std::string scopesReq =
        "{\"seq\":7,\"type\":\"request\",\"command\":\"scopes\",\"arguments\":{\"frameId\":0}}";
    std::string scopesResp = debugger.HandleDapRequest(scopesReq);
    CHECK(scopesResp.find("\"success\":true") != std::string::npos);
    CHECK(scopesResp.find("Locals") != std::string::npos);
    CHECK(scopesResp.find("Globals") != std::string::npos);
    CHECK(scopesResp.find("Registers") != std::string::npos);

    // 8. execution control commands (continue, next, stepIn, stepOut, pause, disconnect)
    CHECK(debugger.HandleDapRequest("{\"seq\":8,\"type\":\"request\",\"command\":\"continue\"}")
              .find("\"success\":true") != std::string::npos);
    CHECK(debugger.HandleDapRequest("{\"seq\":9,\"type\":\"request\",\"command\":\"next\"}").find("\"success\":true") !=
          std::string::npos);
    CHECK(debugger.HandleDapRequest("{\"seq\":10,\"type\":\"request\",\"command\":\"stepIn\"}")
              .find("\"success\":true") != std::string::npos);
    CHECK(debugger.HandleDapRequest("{\"seq\":11,\"type\":\"request\",\"command\":\"stepOut\"}")
              .find("\"success\":true") != std::string::npos);
    CHECK(
        debugger.HandleDapRequest("{\"seq\":12,\"type\":\"request\",\"command\":\"pause\"}").find("\"success\":true") !=
        std::string::npos);
    CHECK(debugger.HandleDapRequest("{\"seq\":13,\"type\":\"request\",\"command\":\"disconnect\"}")
              .find("\"success\":true") != std::string::npos);

    // 9. memory read / write request roundtrip
    uint8_t memoryBuffer[16] = {0};
    std::ostringstream addrSs;
    addrSs << "0x" << std::hex << reinterpret_cast<uintptr_t>(memoryBuffer);
    const std::string memAddr = addrSs.str();

    const std::string writeReq = "{\"seq\":14,\"type\":\"request\",\"command\":\"writeMemory\",\"arguments\":{"
                                 "\"memoryReference\":\"" +
                                 memAddr + "\",\"offset\":0,\"data\":\"SGVsbG8gQW5nZWw=\"}}";
    std::string writeResp = debugger.HandleDapRequest(writeReq);
    CHECK(writeResp.find("\"success\":true") != std::string::npos);
    CHECK(writeResp.find("\"bytesWritten\":11") != std::string::npos);
    CHECK(std::memcmp(memoryBuffer, "Hello Angel", 11) == 0);

    const std::string readReq = "{\"seq\":15,\"type\":\"request\",\"command\":\"readMemory\",\"arguments\":{"
                                "\"memoryReference\":\"" +
                                memAddr + "\",\"offset\":0,\"count\":11}}";
    std::string readResp = debugger.HandleDapRequest(readReq);
    CHECK(readResp.find("\"success\":true") != std::string::npos);
    CHECK(readResp.find("SGVsbG8gQW5nZWw=") != std::string::npos);

    // 10. disassemble request
    const std::string disasmReq = "{\"seq\":16,\"type\":\"request\",\"command\":\"disassemble\",\"arguments\":{"
                                  "\"memoryReference\":\"" +
                                  memAddr + "\",\"instructionCount\":5}}";
    std::string disasmResp = debugger.HandleDapRequest(disasmReq);
    CHECK(disasmResp.find("\"success\":true") != std::string::npos);
    CHECK(disasmResp.find("\"instructions\"") != std::string::npos);

    // 11. exceptionInfo request
    const std::string exInfoReq =
        "{\"seq\":17,\"type\":\"request\",\"command\":\"exceptionInfo\",\"arguments\":{\"threadId\":1}}";
    std::string exInfoResp = debugger.HandleDapRequest(exInfoReq);
    CHECK(exInfoResp.find("\"success\":true") != std::string::npos);
    CHECK(exInfoResp.find("\"exceptionId\":\"AngelScriptException\"") != std::string::npos);
}

TEST_CASE("Debugger DAP Protocol - Message Framing and Streaming Unframing")
{
    const std::string json = "{\"seq\":1,\"type\":\"event\",\"event\":\"initialized\"}";
    const std::string packet = angel_lsp::debug::Debugger::FrameDapMessage(json);

    CHECK(packet.rfind("Content-Length: " + std::to_string(json.size()) + "\r\n\r\n", 0) == 0);

    // Streaming partial unframing
    std::string buffer;
    CHECK_FALSE(angel_lsp::debug::Debugger::UnframeDapMessage(buffer).has_value());

    // Feed header only
    buffer += "Content-Length: " + std::to_string(json.size()) + "\r\n\r\n";
    CHECK_FALSE(angel_lsp::debug::Debugger::UnframeDapMessage(buffer).has_value());

    // Feed half the payload
    buffer += json.substr(0, json.size() / 2);
    CHECK_FALSE(angel_lsp::debug::Debugger::UnframeDapMessage(buffer).has_value());

    // Feed remaining payload
    buffer += json.substr(json.size() / 2);
    const auto unframed = angel_lsp::debug::Debugger::UnframeDapMessage(buffer);
    REQUIRE(unframed.has_value());
    CHECK(unframed.value() == json);
    CHECK(buffer.empty());
}

TEST_CASE("Debugger DAP Protocol - Event Formatting and Variable Requests")
{
    angel_lsp::debug::Debugger debugger;

    const std::string stoppedEvent =
        angel_lsp::debug::Debugger::FormatDapEvent("stopped", "{\"reason\":\"breakpoint\",\"threadId\":1}", 1);
    CHECK(stoppedEvent.find("\"event\":\"stopped\"") != std::string::npos);
    CHECK(stoppedEvent.find("\"reason\":\"breakpoint\"") != std::string::npos);

    // Empty variables when not suspended
    const std::string varsReq =
        "{\"seq\":14,\"type\":\"request\",\"command\":\"variables\",\"arguments\":{\"variablesReference\":1000}}";
    std::string varsResp = debugger.HandleDapRequest(varsReq, nullptr);
    CHECK(varsResp.find("\"success\":true") != std::string::npos);
    CHECK(varsResp.find("\"variables\":[]") != std::string::npos);

    // Empty stackTrace when not suspended
    const std::string stackReq =
        "{\"seq\":15,\"type\":\"request\",\"command\":\"stackTrace\",\"arguments\":{\"threadId\":1}}";
    std::string stackResp = debugger.HandleDapRequest(stackReq, nullptr);
    CHECK(stackResp.find("\"success\":true") != std::string::npos);
    CHECK(stackResp.find("\"totalFrames\":0") != std::string::npos);

    // Evaluation on detached context produces graceful fallback
    const std::string evalReq =
        "{\"seq\":16,\"type\":\"request\",\"command\":\"evaluate\",\"arguments\":{\"expression\":\"varName\"}}";
    std::string evalResp = debugger.HandleDapRequest(evalReq, nullptr);
    CHECK(evalResp.find("\"success\":true") != std::string::npos);
    CHECK(evalResp.find("<unknown>") != std::string::npos);
}

TEST_CASE("Debugger DAP Protocol - Hit Condition Evaluation & Base64 Roundtrip")
{
    CHECK(angel_lsp::debug::Debugger::EvaluateHitCondition(5, "== 5"));
    CHECK_FALSE(angel_lsp::debug::Debugger::EvaluateHitCondition(4, "== 5"));
    CHECK(angel_lsp::debug::Debugger::EvaluateHitCondition(6, "> 5"));
    CHECK(angel_lsp::debug::Debugger::EvaluateHitCondition(4, "< 5"));
    CHECK(angel_lsp::debug::Debugger::EvaluateHitCondition(6, "% 2"));
    CHECK_FALSE(angel_lsp::debug::Debugger::EvaluateHitCondition(7, "% 2"));
    CHECK(angel_lsp::debug::Debugger::EvaluateHitCondition(10, ">= 10"));
    CHECK(angel_lsp::debug::Debugger::EvaluateHitCondition(10, "<= 10"));

    // Base64 helpers with randomized payload
    const std::string text = test::GenerateRandomSymbolName("SecretToken");
    const std::string encoded =
        angel_lsp::debug::Debugger::Base64Encode(reinterpret_cast<const uint8_t*>(text.data()), text.size());
    const auto decoded = angel_lsp::debug::Debugger::Base64Decode(encoded);
    const std::string restored(reinterpret_cast<const char*>(decoded.data()), decoded.size());
    CHECK(restored == text);
}

TEST_CASE("Breakpoint Locations - AngelScript Oracle Bytecode Parity Execution")
{
    // Find built oracle binary
    const std::vector<fs::path> candidates = {
        fs::path(ANGELSCRIPT_REPO_ROOT) / "build" / "Debug" / "angelscript_oracle.exe",
        fs::path(ANGELSCRIPT_REPO_ROOT) / "build" / "bin" / "angelscript_oracle.exe",
        fs::path(ANGELSCRIPT_REPO_ROOT) / "build" / "Release" / "angelscript_oracle.exe",
        fs::path(ANGELSCRIPT_REPO_ROOT) / "server" / "build" / "Debug" / "angelscript_oracle.exe",
        fs::path(ANGELSCRIPT_REPO_ROOT) / "server" / "build" / "bin" / "angelscript_oracle.exe",
    };

    std::string oracleExe;
    for (const auto& c : candidates)
    {
        if (fs::exists(c))
        {
            oracleExe = c.string();
            break;
        }
    }

    if (oracleExe.empty())
    {
        MESSAGE("angelscript_oracle.exe not found - skipping live oracle comparison.");
        return;
    }

    // Generate randomized script
    const std::string clsName = test::GenerateRandomSymbolName("WeaponConfig");
    const std::string fnName = test::GenerateRandomSymbolName("Fire");
    const std::string globalVar = test::GenerateRandomSymbolName("g_active");

    // Line 0: int g_active = 100;
    // Line 1: class WeaponConfig
    // Line 2: {
    // Line 3:     int ammo;
    // Line 4:     WeaponConfig()
    // Line 5:     {
    // Line 6:         ammo = 30;
    // Line 7:     }
    // Line 8:     void Fire(int count)
    // Line 9:     {
    // Line 10:        for (int i = 0; i < count; ++i)
    // Line 11:        {
    // Line 12:            ammo = ammo - 1;
    // Line 13:        }
    // Line 14:    }
    // Line 15: }
    const std::string script = "int " + globalVar +
                               " = 100;\n"
                               "class " +
                               clsName +
                               "\n"
                               "{\n"
                               "    int ammo;\n"
                               "    " +
                               clsName +
                               "()\n"
                               "    {\n"
                               "        ammo = 30;\n"
                               "    }\n"
                               "    void " +
                               fnName +
                               "(int count)\n"
                               "    {\n"
                               "        for (int i = 0; i < count; ++i)\n"
                               "        {\n"
                               "            ammo = ammo - 1;\n"
                               "        }\n"
                               "    }\n"
                               "}\n";

    // Write temporary script to disk for oracle execution
    const fs::path tempScript = fs::temp_directory_path() / (test::GenerateRandomSymbolName("bp_oracle") + ".as");
    {
        std::ofstream out(tempScript);
        out << script;
    }

    // Execute oracle with --dump-lines --json
#if defined(_WIN32)
    const std::string cmd = "\"\"" + oracleExe + "\" \"" + tempScript.string() + "\" --dump-lines --json\"";
    FILE* pipe = _popen(cmd.c_str(), "r");
#else
    const std::string cmd = "\"" + oracleExe + "\" \"" + tempScript.string() + "\" --dump-lines --json";
    FILE* pipe = popen(cmd.c_str(), "r");
#endif
    REQUIRE(pipe != nullptr);

    std::string oracleOutput;
    char chunk[1024];
    while (fgets(chunk, sizeof(chunk), pipe) != nullptr)
    {
        oracleOutput += chunk;
    }
#if defined(_WIN32)
    _pclose(pipe);
#else
    pclose(pipe);
#endif
    fs::remove(tempScript);

    // Collect AST breakpoints from our analyzer
    AngelScriptParser parser;
    TSTree* tree = parser.Parse(script);
    REQUIRE(tree != nullptr);

    const BreakpointLocationsRequest req{ts_tree_root_node(tree), 0, 100000, nullptr};
    const auto collectedLocs = BreakpointLocationsHandler::Collect(req);
    ts_tree_delete(tree);

    REQUIRE_FALSE(collectedLocs.empty());

    std::vector<uint32_t> collectedLines;
    for (const auto& item : collectedLocs)
    {
        collectedLines.push_back(item.line);
    }

    // Extract all lines from oracle output
    std::vector<uint32_t> oracleLines;
    size_t pos = 0;
    while ((pos = oracleOutput.find("\"line\":", pos)) != std::string::npos)
    {
        pos += 7;
        while (pos < oracleOutput.size() && oracleOutput[pos] == ' ')
        {
            ++pos;
        }
        int l = std::atoi(oracleOutput.c_str() + pos);
        if (l >= 0)
        {
            oracleLines.push_back(static_cast<uint32_t>(l));
        }
    }

    REQUIRE_FALSE(oracleLines.empty());

    // Assert that every line generated by the real AngelScript compiler's bytecode table
    // is matched by a breakpoint location discovered by our Tree-Sitter AST analyzer!
    for (uint32_t line : oracleLines)
    {
        INFO("AngelScript oracle emitted bytecode on line: ", line);
        CHECK(std::find(collectedLines.begin(), collectedLines.end(), line) != collectedLines.end());
    }
}

TEST_CASE("Debugger Runtime - Attach, Breakpoints, and Lifecycle")
{
    angel_lsp::debug::Debugger debugger;
    const std::string section = "test_script.as";
    debugger.SetBreakpoint(section, 10);
    debugger.SetBreakpoint(section, 20);

    debugger.ClearBreakpoint(section, 10);
    debugger.ClearAllBreakpoints();
    debugger.Continue();
    debugger.StepIn();
    debugger.Detach();
}

#else

TEST_CASE("Debugger DAP Protocol - Parity Oracle Test Suite Notice")
{
    MESSAGE("AngelScript SDK header <angelscript.h> not present in this target (non-oracle build). DAP runtime "
            "integration tests run during oracle parity CI.");
}

#endif
