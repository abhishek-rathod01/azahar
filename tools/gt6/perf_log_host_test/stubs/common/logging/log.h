// Host-test stand-in for common/logging/log.h
#pragma once
#include <cstdio>
#include <fmt/format.h>
#define LOG_INFO(cls, ...) std::fprintf(stderr, "[I] %s\n", fmt::format(__VA_ARGS__).c_str())
#define LOG_WARNING(cls, ...) std::fprintf(stderr, "[W] %s\n", fmt::format(__VA_ARGS__).c_str())
#define LOG_ERROR(cls, ...) std::fprintf(stderr, "[E] %s\n", fmt::format(__VA_ARGS__).c_str())
