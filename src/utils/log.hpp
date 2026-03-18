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
    static void Debug(const std::string &msg, const std::source_location &loc = std::source_location::current(), bool returnLog = false);
    static void Error(const std::string &msg, const std::source_location &loc = std::source_location::current(), bool returnLog = false);
    static void Warn(const std::string &msg, const std::source_location &loc = std::source_location::current(), bool returnLog = false);
    static void Description(const std::string &msg, const std::source_location &loc = std::source_location::current(), bool returnLog = false);
    static void Info(const std::string &msg, std::source_location loc = std::source_location::current(), bool returnLog = false);
    static void Background(const std::string &msg, const std::source_location &loc = std::source_location::current(), bool returnLog = false);

    static bool True(const std::string &msg, const std::source_location &loc = std::source_location::current());
    static bool False(const std::string &msg, const std::source_location &loc = std::source_location::current());
    static void Void(const std::string &msg, const std::source_location &loc = std::source_location::current());
    static void Msg(const std::string &msg, const std::source_location &loc, const Level level, bool returnVoid);
    static bool Msg(const std::string &msg, const std::source_location &loc, const Level level, BoolType type);

    static std::string NumToStr(double douNum, bool format = true);
    static std::string DVec3ToStr(const glm::dvec3 &msg);

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

#define LOG_DEBU(msg, ...) Log::Debug(msg);
#define LOG_ERROR(msg) Log::Error(msg);
#define LOG_WARN(msg) Log::Warn(msg);
#define LOG_DESC(msg) Log::Description(msg);
#define LOG_INFO(msg) Log::Info(msg);
#define LOG_BACK(msg) Log::Background(msg);

#define LOG_FALSE(msg) Log::False(msg);
#define LOG_TRUE(msg) Log::True(msg);
#define LOG_VOID(msg) Log::Void(msg);
#define LOG_MSG(msg, loc, level, returnType) Log::Msg(msg, loc, level, returnType);

#define LOG_FILTER_BACK(state) Log::SetBackgroundFilter(state);
#define LOG_FILTER_ALL(state) Log::SetAllFilter(state);

#define LOG_START Log::Start();
#define LOG_END Log::End();

#define LOG_SPLIT(level) Log::Split(level);

#define LOG_GET_SOURCE() Log::GetSource();
