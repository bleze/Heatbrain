#pragma once

#include <chrono>
#include <cstdio>
#include <format>
#include <string>

namespace hb::log {

enum class Level { Debug, Info, Warn, Error };

inline Level& threshold() {
    static Level level = Level::Info;
    return level;
}

template <class... Args>
void write(Level level, std::format_string<Args...> fmt, Args&&... args) {
    if (level < threshold()) return;
    static constexpr const char* names[] = {"DEBUG", "INFO ", "WARN ", "ERROR"};
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    const std::string msg = std::format(fmt, std::forward<Args>(args)...);
    std::fprintf(stderr, "%s %s %s\n", std::format("{:%Y-%m-%dT%H:%M:%SZ}", now).c_str(),
                 names[static_cast<int>(level)], msg.c_str());
}

template <class... Args>
void debug(std::format_string<Args...> f, Args&&... a) { write(Level::Debug, f, std::forward<Args>(a)...); }
template <class... Args>
void info(std::format_string<Args...> f, Args&&... a) { write(Level::Info, f, std::forward<Args>(a)...); }
template <class... Args>
void warn(std::format_string<Args...> f, Args&&... a) { write(Level::Warn, f, std::forward<Args>(a)...); }
template <class... Args>
void error(std::format_string<Args...> f, Args&&... a) { write(Level::Error, f, std::forward<Args>(a)...); }

}  // namespace hb::log
