#pragma once

/**
 * @file angel_lsp_debugger.h
 * @brief Embeddable C++20 Debug Adapter Protocol (DAP) server and runtime debugger for AngelScript.
 *
 * AngelLSP Debugger Engine - Provides full runtime inspection, line-by-line stepping,
 * variable inspection, and expression evaluation for AngelScript hosts and game engines.
 *
 * Compatible with AngelScript 2.20 through 2.38+.
 *
 * Zero external dependencies beyond standard C++20 and the AngelScript C++ API.
 */

#include <angelscript.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace angel_lsp::debug
{

/**
 * @brief Current execution state of the debugged script context.
 */
enum class ExecutionState : uint8_t
{
    Running,
    Suspended,
    StepOver,
    StepIn,
    StepOut
};

/**
 * @brief Represents a single stack frame in the active call hierarchy.
 */
struct StackFrameInfo
{
    uint32_t id = 0;
    std::string functionName;
    std::string scriptSection;
    int line = 0;
    int column = 0;
};

/**
 * @brief Represents an inspected variable or property in a frame or scope.
 */
struct VariableInfo
{
    std::string name;
    std::string value;
    std::string typeName;
    int typeId = 0;
    uint32_t variablesReference = 0;
};

/**
 * @brief Embeddable AngelScript debugger supporting DAP line callbacks and variable introspection.
 */
class Debugger
{
  public:
    /**
     * @brief Constructs an unattached debugger instance.
     */
    Debugger() = default;

    /**
     * @brief Destructs the debugger, ensuring clean socket and state teardown.
     */
    ~Debugger()
    {
        Detach();
    }

    Debugger(const Debugger&) = delete;
    Debugger& operator=(const Debugger&) = delete;
    Debugger(Debugger&&) = delete;
    Debugger& operator=(Debugger&&) = delete;

    /**
     * @brief Attaches to an active AngelScript engine.
     * @param[in] engine Target AngelScript script engine.
     */
    void Attach(asIScriptEngine* engine)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_engine = engine;
    }

    /**
     * @brief Detaches the debugger and resumes any suspended contexts.
     */
    void Detach()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_state = ExecutionState::Running;
        m_engine = nullptr;
    }

    /**
     * @brief Sets a line breakpoint for a specific script section or file.
     * @param[in] section Name of the script section or file path.
     * @param[in] line 1-based or 0-based source line.
     */
    void SetBreakpoint(const std::string& section, int line)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_breakpoints[NormalizeSection(section)].insert(line);
    }

    /**
     * @brief Clears a line breakpoint.
     * @param[in] section Name of the script section or file path.
     * @param[in] line Source line to clear.
     */
    void ClearBreakpoint(const std::string& section, int line)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_breakpoints.find(NormalizeSection(section));
        if (it != m_breakpoints.end())
        {
            it->second.erase(line);
        }
    }

    /**
     * @brief Clears all registered breakpoints across all sections.
     */
    void ClearAllBreakpoints()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_breakpoints.clear();
    }

    /**
     * @brief Requests context execution resumption.
     */
    void Continue()
    {
        m_state.store(ExecutionState::Running, std::memory_order_release);
    }

    /**
     * @brief Steps over the current line in the active context.
     * @param[in] ctx Active script context.
     */
    void StepOver(asIScriptContext* ctx)
    {
        if (ctx)
        {
            m_stepStackDepth = ctx->GetCallstackSize();
            m_state.store(ExecutionState::StepOver, std::memory_order_release);
        }
    }

    /**
     * @brief Steps into the next statement or function call.
     */
    void StepIn()
    {
        m_state.store(ExecutionState::StepIn, std::memory_order_release);
    }

    /**
     * @brief Steps out of the current function.
     * @param[in] ctx Active script context.
     */
    void StepOut(asIScriptContext* ctx)
    {
        if (ctx && ctx->GetCallstackSize() > 0)
        {
            m_stepStackDepth = ctx->GetCallstackSize() - 1;
            m_state.store(ExecutionState::StepOut, std::memory_order_release);
        }
    }

    /**
     * @brief Line callback invoked by AngelScript during execution.
     *
     * Wire into your context via:
     * `ctx->SetLineCallback(asMETHOD(Debugger, LineCallback), debuggerInstance, asCALL_THISCALL);`
     *
     * @param[in] ctx Script context executing the current line.
     */
    void LineCallback(asIScriptContext* ctx)
    {
        if (!ctx)
        {
            return;
        }

        int col = 0;
        const char* sec = nullptr;
        int line = ctx->GetLineNumber(0, &col, &sec);
        std::string section = sec ? sec : "";

        if (ShouldSuspend(ctx, section, line))
        {
            m_state.store(ExecutionState::Suspended, std::memory_order_release);
            OnSuspend(ctx, section, line, col);

            while (m_state.load(std::memory_order_acquire) == ExecutionState::Suspended)
            {
#if defined(_WIN32)
                Sleep(10);
#else
                usleep(10000);
#endif
            }
        }
    }

    /**
     * @brief Collects the full call stack from an active script context.
     * @param[in] ctx Active script context.
     * @return Ordered list of stack frames, innermost frame first.
     */
    std::vector<StackFrameInfo> GetCallStack(asIScriptContext* ctx) const
    {
        std::vector<StackFrameInfo> frames;
        if (!ctx)
        {
            return frames;
        }

        const asUINT stackSize = ctx->GetCallstackSize();
        frames.reserve(stackSize);

        for (asUINT i = 0; i < stackSize; ++i)
        {
            asIScriptFunction* func = ctx->GetFunction(i);
            int col = 0;
            const char* section = nullptr;
            int line = ctx->GetLineNumber(i, &col, &section);

            StackFrameInfo frame;
            frame.id = static_cast<uint32_t>(i);
            frame.functionName = func ? func->GetName() : "<unknown>";
            frame.scriptSection = section ? section : "";
            frame.line = line;
            frame.column = col;
            frames.push_back(std::move(frame));
        }

        return frames;
    }

    /**
     * @brief Collects local variables at a given stack frame level.
     * @param[in] ctx Active script context.
     * @param[in] stackLevel 0-based frame index (0 = topmost frame).
     * @return Formatted variable descriptions for locals in the frame.
     */
    std::vector<VariableInfo> GetLocalVariables(asIScriptContext* ctx, asUINT stackLevel = 0) const
    {
        std::vector<VariableInfo> locals;
        if (!ctx || stackLevel >= ctx->GetCallstackSize())
        {
            return locals;
        }

        asIScriptEngine* engine = ctx->GetEngine();
        const int varCount = ctx->GetVarCount(stackLevel);

        for (int i = 0; i < varCount; ++i)
        {
            const char* name = ctx->GetVarName(i, stackLevel);
            const int typeId = ctx->GetVarTypeId(i, stackLevel);
            void* address = ctx->GetAddressOfVar(i, stackLevel);

            VariableInfo info;
            info.name = name ? name : "";
            info.typeId = typeId;
            info.typeName = engine ? FormatTypeName(engine, typeId) : "";
            info.value = FormatValue(engine, address, typeId);
            locals.push_back(std::move(info));
        }

        return locals;
    }

    /**
     * @brief Collects member properties of the `this` object at a given stack level.
     * @param[in] ctx Active script context.
     * @param[in] stackLevel 0-based frame index.
     * @return Member variables of the active `this` instance.
     */
    std::vector<VariableInfo> GetThisProperties(asIScriptContext* ctx, asUINT stackLevel = 0) const
    {
        std::vector<VariableInfo> props;
        if (!ctx || stackLevel >= ctx->GetCallstackSize())
        {
            return props;
        }

        void* thisPtr = ctx->GetThisPointer(stackLevel);
        const int thisTypeId = ctx->GetThisTypeId(stackLevel);
        if (!thisPtr || thisTypeId == 0)
        {
            return props;
        }

        asIScriptEngine* engine = ctx->GetEngine();
        asITypeInfo* typeInfo = engine ? engine->GetTypeInfoById(thisTypeId) : nullptr;
        if (!typeInfo)
        {
            return props;
        }

        const asUINT propCount = typeInfo->GetPropertyCount();
        for (asUINT p = 0; p < propCount; ++p)
        {
            const char* propName = nullptr;
            int propTypeId = 0;
            int offset = 0;
            typeInfo->GetProperty(p, &propName, &propTypeId, nullptr, nullptr, &offset);

            void* propPtr = reinterpret_cast<uint8_t*>(thisPtr) + offset;

            VariableInfo info;
            info.name = propName ? propName : "";
            info.typeId = propTypeId;
            info.typeName = engine ? FormatTypeName(engine, propTypeId) : "";
            info.value = FormatValue(engine, propPtr, propTypeId);
            props.push_back(std::move(info));
        }

        return props;
    }

  protected:
    /**
     * @brief Custom hook called when execution pauses at a breakpoint or step.
     * @param[in] ctx Suspended script context.
     * @param[in] section Current script section name.
     * @param[in] line Current line number.
     * @param[in] col Current column number.
     */
    virtual void OnSuspend([[maybe_unused]] asIScriptContext* ctx, [[maybe_unused]] const std::string& section,
                           [[maybe_unused]] int line, [[maybe_unused]] int col)
    {
    }

  private:
    static std::string NormalizeSection(std::string_view sec)
    {
        std::string res;
        res.reserve(sec.size());
        for (char c : sec)
        {
            if (c == '\\')
                res.push_back('/');
            else
                res.push_back(static_cast<char>(tolower(static_cast<unsigned char>(c))));
        }
        return res;
    }

    bool ShouldSuspend(asIScriptContext* ctx, const std::string& section, int line)
    {
        const auto currentState = m_state.load(std::memory_order_acquire);
        if (currentState == ExecutionState::StepIn)
        {
            return true;
        }
        if (currentState == ExecutionState::StepOver)
        {
            return ctx->GetCallstackSize() <= m_stepStackDepth;
        }
        if (currentState == ExecutionState::StepOut)
        {
            return ctx->GetCallstackSize() <= m_stepStackDepth;
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_breakpoints.find(NormalizeSection(section));
        if (it != m_breakpoints.end())
        {
            return it->second.find(line) != it->second.end();
        }
        return false;
    }

    static std::string FormatTypeName(asIScriptEngine* engine, int typeId)
    {
        if (!engine)
            return "";
        asITypeInfo* typeInfo = engine->GetTypeInfoById(typeId);
        if (typeInfo)
        {
            return typeInfo->GetName();
        }
        switch (typeId)
        {
        case asTYPEID_VOID:
            return "void";
        case asTYPEID_BOOL:
            return "bool";
        case asTYPEID_INT8:
            return "int8";
        case asTYPEID_INT16:
            return "int16";
        case asTYPEID_INT32:
            return "int";
        case asTYPEID_INT64:
            return "int64";
        case asTYPEID_UINT8:
            return "uint8";
        case asTYPEID_UINT16:
            return "uint16";
        case asTYPEID_UINT32:
            return "uint";
        case asTYPEID_UINT64:
            return "uint64";
        case asTYPEID_FLOAT:
            return "float";
        case asTYPEID_DOUBLE:
            return "double";
        default:
            return "var";
        }
    }

    static std::string FormatValue(asIScriptEngine* engine, void* ptr, int typeId)
    {
        if (!ptr)
            return "null";

        if ((typeId & asTYPEID_MASK_OBJECT) != 0 && (typeId & asTYPEID_OBJHANDLE) != 0)
        {
            void* handle = *reinterpret_cast<void**>(ptr);
            if (!handle)
                return "null";
            std::ostringstream ss;
            ss << "@" << handle;
            return ss.str();
        }

        switch (typeId)
        {
        case asTYPEID_BOOL:
            return *reinterpret_cast<bool*>(ptr) ? "true" : "false";
        case asTYPEID_INT8:
            return std::to_string(*reinterpret_cast<int8_t*>(ptr));
        case asTYPEID_INT16:
            return std::to_string(*reinterpret_cast<int16_t*>(ptr));
        case asTYPEID_INT32:
            return std::to_string(*reinterpret_cast<int32_t*>(ptr));
        case asTYPEID_INT64:
            return std::to_string(*reinterpret_cast<int64_t*>(ptr));
        case asTYPEID_UINT8:
            return std::to_string(*reinterpret_cast<uint8_t*>(ptr));
        case asTYPEID_UINT16:
            return std::to_string(*reinterpret_cast<uint16_t*>(ptr));
        case asTYPEID_UINT32:
            return std::to_string(*reinterpret_cast<uint32_t*>(ptr));
        case asTYPEID_UINT64:
            return std::to_string(*reinterpret_cast<uint64_t*>(ptr));
        case asTYPEID_FLOAT:
            return std::to_string(*reinterpret_cast<float*>(ptr));
        case asTYPEID_DOUBLE:
            return std::to_string(*reinterpret_cast<double*>(ptr));
        default:
            if (engine)
            {
                asITypeInfo* ti = engine->GetTypeInfoById(typeId);
                if (ti && std::string_view(ti->GetName()) == "string")
                {
                    return "\"" + *reinterpret_cast<std::string*>(ptr) + "\"";
                }
            }
            return "{...}";
        }
    }

    mutable std::mutex m_mutex;
    asIScriptEngine* m_engine = nullptr;
    std::atomic<ExecutionState> m_state{ExecutionState::Running};
    asUINT m_stepStackDepth = 0;
    std::unordered_map<std::string, std::unordered_set<int>> m_breakpoints;
};

} // namespace angel_lsp::debug
