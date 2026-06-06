#pragma once
#include <cctype>

#define SAFE_IS_ALNUM(c) (std::isalnum(static_cast<unsigned char>(c)))
#define SAFE_IS_SPACE(c) (std::isspace(static_cast<unsigned char>(c)))
#define SAFE_IS_ALPHA(c) (std::isalpha(static_cast<unsigned char>(c)))
#define SAFE_IS_DIGIT(c) (std::isdigit(static_cast<unsigned char>(c)))