#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

int main(int argc, char** argv) {
    // Set up console logger for tests
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    auto logger = std::make_shared<spdlog::logger>("test_logger", console_sink);
    spdlog::set_default_logger(logger);
    spdlog::set_level(spdlog::level::debug); // Set to debug for tests
    
    doctest::Context context;
    context.applyCommandLine(argc, argv);
    
    int res = context.run(); // run
    
    if(context.shouldExit()) {
        return res;
    }
    
    return res;
}
