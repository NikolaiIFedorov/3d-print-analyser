#pragma once

#include <source_location>
#include <unordered_map>

#include <iostream>
#include <string>
#include <glm/glm.hpp>

#include <chrono>

enum class Level
{
    DEBUG,
    ERROR,
    WARN,
    INFO,
    DESC,
    BACKGROUND,
    SESSION,
};

enum class LogVerbosity
{
    QUIET,   // only warnings/errors
    NORMAL,  // warnings/errors + session events
    VERBOSE, // all levels
};

enum class BoolType
{
    TRUE,
    FALSE,
};

inline Level lastLevel;
inline bool backgroundFilter = false;
inline bool debugFilter = false;
inline bool allFilter = false;

inline std::string lastMsg;
inline int repeatsMsg = 1;
inline int spacing = -1;

inline std::unordered_map<std::string, int> functionToSpacing;

inline std::chrono::steady_clock clockLog;
inline std::chrono::steady_clock::time_point lastTime = clockLog.now();

inline uint idLog = 0;

class Log
{
public:
    static void SetVerbosity(LogVerbosity verbosity);
    static LogVerbosity GetVerbosity();

    static void Debug(const std::string &msg, const std::source_location &loc = std::source_location::current(), bool returnLog = false);
    static void Error(const std::string &msg, const std::source_location &loc = std::source_location::current(), bool returnLog = false);
    static void Warn(const std::string &msg, const std::source_location &loc = std::source_location::current(), bool returnLog = false);
    static void Description(const std::string &msg, const std::source_location &loc = std::source_location::current(), bool returnLog = false);
    static void Info(const std::string &msg, std::source_location loc = std::source_location::current(), bool returnLog = false);
    static void Background(const std::string &msg, const std::source_location &loc = std::source_location::current(), bool returnLog = false);
    static void Session(const std::string &msg, const std::source_location &loc = std::source_location::current(), bool returnLog = false);

    static bool True(const std::string &msg, const std::source_location &loc = std::source_location::current());
    static bool False(const std::string &msg, const std::source_location &loc = std::source_location::current());
    static void Void(const std::string &msg, const std::source_location &loc = std::source_location::current());
    static void Msg(const std::string &msg, const std::source_location &loc, const Level level, bool returnVoid);
    static bool Msg(const std::string &msg, const std::source_location &loc, const Level level, BoolType type);

    static std::string NumToStr(double douNum, bool format = true);
    static std::string DVec3ToStr(const glm::dvec3 &msg);

    // ── Variadic message building ─────────────────────────────────────────────
    // Converts a supported type to a display string (numbers are colored/underlined).
    // Supported: std::string, const char*, bool, any arithmetic type, glm::vec2/vec3/dvec3.
    static std::string ToStr(const std::string &v);
    static std::string ToStr(const char *v);
    static std::string ToStr(bool v);
    static std::string ToStr(const glm::vec2 &v);
    static std::string ToStr(const glm::vec3 &v);
    static std::string ToStr(const glm::dvec3 &v);

    template <typename T>
        requires(std::is_arithmetic_v<T> && !std::is_same_v<std::remove_cvref_t<T>, bool>)
    static std::string ToStr(T v)
    {
        return NumToStr(static_cast<double>(v));
    }

    // Joins all arguments as space-separated strings using ToStr.
    // Usage:  Log::BuildMsg("pos:", position, velocity)
    template <typename First, typename... Rest>
    static std::string BuildMsg(First &&first, Rest &&...rest)
    {
        std::string result = ToStr(std::forward<First>(first));
        ((result += " " + ToStr(std::forward<Rest>(rest))), ...);
        return result;
    }
    static std::string BuildMsg() { return ""; }

    static void SetBackgroundFilter(bool state);
    static void SetDebugFilter(bool state);
    static void SetAllFilter(bool state);

    static void Start();
    static void End();

    static void Split(Level level, bool returnLog = false);

private:
    static void Write(const std::string &msg, const std::source_location &loc, Level format, bool returnLog = false);

    static std::string GetOutput(const std::string &msg, Level level, const std::source_location &loc, double duration, bool returnLog);
    static std::string GetLevelTag(Level format, std::string &msg);
    static std::string GetPath(const std::source_location &loc, Level level, const std::string &tag, const std::string &spacer, const std::string &msg);
    static std::string GetModifier(std::string &msg, Level level);
    static std::string ProcessMsg(const std::string &msg, bool returnLog);
    static std::string GetSpacer(const std::source_location &loc, bool returnLog);
    static double GetDuration();
    static std::string GetTime(double duration);
    static std::string GetId();

    static void UpdateGlobals(const std::string &msg, Level level);

    static uint16_t StringSize(std::string str);
    static bool Erase(std::string &str, std::string start, char end);
    static void CleanString(std::string &msg);
};

// Variadic log macros — accepts a label string followed by any number of values:
//   LOG_INFO("pos:", position, velocity)  →  "pos: (1.00, 2.00, 3.00) (0.50, 0.00, 0.10)"
// source_location is captured at the call site via the macro.
#define LOG_DEBU(...) Log::Debug(Log::BuildMsg(__VA_ARGS__), std::source_location::current());
#define LOG_ERROR(...) Log::Error(Log::BuildMsg(__VA_ARGS__), std::source_location::current());
#define LOG_WARN(...) Log::Warn(Log::BuildMsg(__VA_ARGS__), std::source_location::current());
#define LOG_DESC(...) Log::Description(Log::BuildMsg(__VA_ARGS__), std::source_location::current());
#define LOG_INFO(...) Log::Info(Log::BuildMsg(__VA_ARGS__), std::source_location::current());
#define LOG_BACK(...) Log::Background(Log::BuildMsg(__VA_ARGS__), std::source_location::current());
#define LOG_SESSION(...) Log::Session(Log::BuildMsg(__VA_ARGS__), std::source_location::current());

#define LOG_FALSE(...) Log::False(Log::BuildMsg(__VA_ARGS__), std::source_location::current());
#define LOG_TRUE(...) Log::True(Log::BuildMsg(__VA_ARGS__), std::source_location::current());
#define LOG_VOID(...) Log::Void(Log::BuildMsg(__VA_ARGS__), std::source_location::current());
#define LOG_MSG(msg, loc, level, returnType) Log::Msg(msg, loc, level, returnType);

#define LOG_FILTER_BACK(state) Log::SetBackgroundFilter(state);
#define LOG_FILTER_ALL(state) Log::SetAllFilter(state);

#define LOG_START Log::Start();
#define LOG_END Log::End();

#define LOG_SPLIT(level) Log::Split(level);

#define LOG_GET_SOURCE() Log::GetSource();
