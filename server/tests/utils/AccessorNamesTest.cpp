#include "utils/AccessorNames.h"

#include <doctest/doctest.h>
#include <ostream>
#include <string>

using angel_lsp::utils::AccessorNameBuffer;
using angel_lsp::utils::ComposeAccessorName;
using angel_lsp::utils::ComposeGetterName;
using angel_lsp::utils::k_getterPrefix;
using angel_lsp::utils::k_setterPrefix;

TEST_SUITE("utils/AccessorNames")
{
    TEST_CASE("AccessorNameBuffer - A composed name is complete at every length around the inline limit")
    {
        AccessorNameBuffer buffer;
        for (size_t length = 0; length < 700; length += (length >= 120 && length <= 135) ? 1 : 37)
        {
            const std::string name(length, 'n');
            CHECK(ComposeAccessorName(k_getterPrefix, name, buffer) == "get_" + name);
            CHECK(ComposeAccessorName(k_setterPrefix, name, buffer) == "set_" + name);
            CHECK(ComposeGetterName(name, buffer) == "get_" + name);
        }
    }

    TEST_CASE("AccessorNameBuffer - Two buffers hold two names at once")
    {
        AccessorNameBuffer getBuffer;
        AccessorNameBuffer setBuffer;
        const std::string longName(300, 'x');
        const std::string_view getter = getBuffer.Compose(k_getterPrefix, longName);
        const std::string_view setter = setBuffer.Compose(k_setterPrefix, "short");
        CHECK(getter == "get_" + longName);
        CHECK(setter == "set_short");
    }
}
