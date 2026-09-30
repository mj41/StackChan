// Host stand-in for mooncake_log (tests/sim): prints the format string only.
#pragma once
#include <cstdio>
namespace mclog {
template <typename... A> void tagInfo(const char* tag, const char* fmt, A&&...) { std::printf("[%s] %s\n", tag, fmt); }
template <typename... A> void tagWarn(const char* tag, const char* fmt, A&&...) { std::printf("[%s] warn: %s\n", tag, fmt); }
template <typename... A> void tagError(const char* tag, const char* fmt, A&&...) { std::printf("[%s] error: %s\n", tag, fmt); }
}  // namespace mclog
