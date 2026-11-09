#pragma once

#include "analysis/types/EngineProfiles.h"

#include <string_view>

namespace angel_lsp::analysis::profiles
{
/**
 * @brief Returns the built-in AngelScript declarations for an engine profile.
 * @param[in] kind Profile to look up; Auto shares the Standard declarations.
 * @return A view of a static literal that lives for the whole program; empty for None.
 */
std::string_view RawProfileStub(EngineProfileKind kind);
} // namespace angel_lsp::analysis::profiles
