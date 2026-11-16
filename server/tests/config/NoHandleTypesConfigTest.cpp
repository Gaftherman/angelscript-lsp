#include "config/EngineRuleConfig.h"
#include "config/EngineRuleParser.h"
#include "config/ServerConfig.h"
#include "helpers/TestUtils.h"

#include <doctest/doctest.h>
#include <string>
#include <vector>

using namespace angel_lsp::config;

namespace
{
struct ArgvHelper
{
    std::vector<std::string> storage;
    std::vector<char*> argv;

    ArgvHelper(std::initializer_list<std::string> args) : storage(args)
    {
        argv.reserve(storage.size());
        for (auto& s : storage)
        {
            argv.push_back(s.data());
        }
    }

    int argc() const
    {
        return static_cast<int>(argv.size());
    }

    char** data()
    {
        return argv.data();
    }
};
} // namespace

TEST_SUITE("config/NoHandleTypes")
{
    TEST_CASE("NoHandleTypesConfig - Parses valid, scoped, and deduplicated CLI flags")
    {
        const std::string typeA = angel_lsp::test::GenerateRandomSymbolName("ValA");
        const std::string nsName = angel_lsp::test::GenerateRandomSymbolName("HostNs");
        const std::string typeB = angel_lsp::test::GenerateRandomSymbolName("ValB");
        const std::string scopedB = nsName + "::" + typeB;

        const std::string argA = "--no-handle-type=" + typeA;
        const std::string argGlobalA = "--no-handle-type=::" + typeA;
        const std::string argScopedB = "--engine-no-handle-type=" + scopedB;

        ArgvHelper args{"angel_lsp", argA, argGlobalA, argScopedB};
        const ServerConfig config = FromArgs(args.argc(), args.data());

        REQUIRE(config.engine.noHandleTypes.size() == 2);
        CHECK(config.engine.noHandleTypes[0] == typeA);
        CHECK(config.engine.noHandleTypes[1] == scopedB);
        CHECK(config.engine.invalidNoHandleTypes.empty());
    }

    TEST_CASE("NoHandleTypesConfig - Rejects invalid identifier names and records warnings")
    {
        const std::string validType = angel_lsp::test::GenerateRandomSymbolName("GoodVal");
        const std::string argValid = "--no-handle-type=" + validType;

        ArgvHelper args{
            "angel_lsp",
            "--no-handle-type=",
            "--no-handle-type=123Invalid",
            "--no-handle-type=has space",
            "--no-handle-type=Ptr@",
            "--no-handle-type=Ns::",
            "--no-handle-type=Ns::::Inner",
            argValid,
        };
        const ServerConfig config = FromArgs(args.argc(), args.data());

        REQUIRE(config.engine.noHandleTypes.size() == 1);
        CHECK(config.engine.noHandleTypes[0] == validType);
        CHECK(config.engine.invalidNoHandleTypes.size() == 6);
    }

    TEST_CASE("NoHandleTypesConfig - MergeNoHandleTypes produces set union without duplicates")
    {
        const std::string fileOnly = angel_lsp::test::GenerateRandomSymbolName("FileVal");
        const std::string sharedType = angel_lsp::test::GenerateRandomSymbolName("SharedVal");
        const std::string settingOnly = angel_lsp::test::GenerateRandomSymbolName("SettingVal");

        angel_lsp::TransparentStringSet fromFile;
        fromFile.insert(fileOnly);
        fromFile.insert(sharedType);

        const std::vector<std::string> fromSettings{sharedType, settingOnly, settingOnly};

        EngineRuleConfig effective;
        MergeNoHandleTypes(effective, fromFile, fromSettings);

        CHECK(effective.noHandleTypes.size() == 3);
        CHECK(effective.noHandleTypes.contains(fileOnly));
        CHECK(effective.noHandleTypes.contains(sharedType));
        CHECK(effective.noHandleTypes.contains(settingOnly));

        // Setting alone
        EngineRuleConfig settingAlone;
        MergeNoHandleTypes(settingAlone, {}, fromSettings);
        CHECK(settingAlone.noHandleTypes.size() == 2);
        CHECK(settingAlone.noHandleTypes.contains(sharedType));
        CHECK(settingAlone.noHandleTypes.contains(settingOnly));

        // Rules file alone
        EngineRuleConfig fileAlone;
        MergeNoHandleTypes(fileAlone, fromFile, {});
        CHECK(fileAlone.noHandleTypes.size() == 2);
        CHECK(fileAlone.noHandleTypes.contains(fileOnly));
        CHECK(fileAlone.noHandleTypes.contains(sharedType));
    }
}
