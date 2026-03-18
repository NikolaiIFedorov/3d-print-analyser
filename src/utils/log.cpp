#include "log.hpp"
#include "thread"

std::string Log::NumToStr(double douNum, bool format)
{
    std::string strNum;

    int intNum = static_cast<int>(douNum);
    if (intNum == douNum)
    {
        strNum = std::to_string(intNum);
    }
    else
    {
        strNum = std::to_string(douNum);
    }

    std::string underLine;
    std::string resetFormat;
    std::string magenta;

    if (format)
    {
        underLine = "\033[4m";
        resetFormat = "\033[0m";
        magenta = "\033[35m";
    }

    return magenta + underLine + strNum + resetFormat;
}

std::string Log::DVec3ToStr(const glm::dvec3 &msg)
{
    std::string format = ", ";
    std::string x = "x = " + NumToStr(msg.x) + format;
    std::string y = "y = " + NumToStr(msg.y) + format;
    std::string z = "z = " + NumToStr(msg.z);

    std::string str = x + y + z;

    return str;
}

std::string Log::GetLevelTag(Level level, std::string &msg)
{
    std::string level_str;
    std::string begin = "[";
    std::string end = "]\033[0m";

    std::string color = "";

    switch (level)
    {
    case Level::DEBUG:
        level_str = "@DEBU";
        color = "\033[32m";
        break;
    case Level::ERROR:
        level_str = "!EROR";
        color = "\033[31m"; // Red
        break;
    case Level::WARN:
        level_str = "#WARN";
        color = "\033[33m"; // Orange
        break;
    case Level::DESC:
        level_str = "~DESC";
        color = "\033[34m"; // Blue
        break;
    case Level::INFO:
        level_str = "?INFO"; // Green
        break;
    case Level::BACKGROUND:
        level_str = ">BACK";
        color = "\033[90m"; // Gray
        break;
    default:
        level_str = "?info";
        break;
    }

    std::string beginSpacer = "";
    if (repeatsMsg != 1)
    {
        level_str = level_str[0];

        int lenRepeats = ceil(repeatsMsg / 10) + 1;
        for (int i = lenRepeats; i < 3; i++)
            beginSpacer += " ";
    }
    else if (lastLevel == level)
    {
        level_str = level_str[0];
        beginSpacer += "    ";
    }

    std::string tag = beginSpacer + color + begin + level_str + end;
    return tag;
}

std::string Log::GetPath(const std::source_location &loc, Level level, const std::string &tag, const std::string &spacerMsg, const std::string &msg)
{
    std::string file = loc.file_name();
    uint8_t root = file.find_last_of('/');
    file.erase(0, root + 1);

    std::string line = NumToStr(loc.line());
    std::string path = file + "/" + line;

    std::string spacer;
    int msgSize = StringSize(msg);
    int pathSize = StringSize(path);
    for (int i = 0; i < 150 - msgSize - spacerMsg.size() - pathSize; i++)
        spacer.push_back(' ');

    return spacer + "\033[90m" + path + "\033[0m";
}

std::string Log::GetModifier(std::string &msg, Level level)
{
    std::string modifier;
    std::string cleanMsg;
    CleanString(msg);
    if (lastMsg != msg)
    {
        modifier = "";
        repeatsMsg = 1;
    }
    else if (lastLevel == level)
    {
        modifier = "\x1b[A";
        repeatsMsg++;
        modifier += NumToStr(repeatsMsg, false) + "x";
    }
    lastMsg = msg;
    return modifier;
}

std::string Log::ProcessMsg(const std::string &msg, bool returnLog)
{
    if (!returnLog)
        return msg;

    return msg + "\033[0m";
}

std::string Log::GetSpacer(const std::source_location &loc, bool returnLog)
{
    std::string function = loc.function_name();

    auto hash = functionToSpacing.find(function);
    if (hash == functionToSpacing.end())
    {
        spacing++;
        functionToSpacing[function] = spacing;

        hash = functionToSpacing.find(function);
    }

    int functionSpacing = hash->second;
    spacing = functionSpacing;

    if (returnLog)
    {
        functionToSpacing.erase(function);
        spacing--;
    }

    std::string spacer;
    for (int i = 0; i < functionSpacing; i++)
        spacer.push_back(' ');

    return spacer;
}

std::string Log::GetTime(double duration)
{
    std::string time;
    std::string start = "[";
    std::string end = "]";

    std::string units = "ms";

    time = start + NumToStr(duration, false) + units + end;
    return time;
}

std::string Log::GetId()
{
    idLog++;
    std::string modifiers = "\033[90m -| ";
    return modifiers + NumToStr(idLog, false) + "\033[0m";
}

std::string Log::GetOutput(const std::string &msg, Level level, const std::source_location &loc, double duration, bool returnLog)
{
    std::string spacer = GetSpacer(loc, returnLog);
    std::string msgProcessed = ProcessMsg(msg, returnLog);
    std::string modifier = GetModifier(msgProcessed, level);

    std::string levelTag = GetLevelTag(level, msgProcessed);
    std::string path = GetPath(loc, level, levelTag, spacer, msg);
    std::string time = GetTime(duration);
    std::string id = GetId();

    std::string output = modifier + levelTag + spacer + msgProcessed + path + time + id + "\n";
    return output;
}

double Log::GetDuration()
{
    auto time = clockLog.now();
    const auto clockDuration = time - lastTime;

    auto msDuration = std::chrono::duration_cast<std::chrono::milliseconds>(clockDuration);

    double duration = msDuration.count();

    lastTime = time;
    return duration;
}

void Log::SetBackgroundFilter(bool state)
{
    backgroundFilter = state;
}

void Log::SetDebugFilter(bool state)
{
    debugFilter = state;
}

void Log::SetAllFilter(bool state)
{
    allFilter = state;
}

void Log::Start()
{
    Split(Level::BACKGROUND);
    Void("Program started");
    Split(Level::BACKGROUND);
}

void Log::End()
{
    Split(Level::BACKGROUND);
    Void("Program ended");
    Split(Level::BACKGROUND);

    return;
    Debug("Input id: ");
    std::string input;

    std::cin >> input;

    if (input == "End")
    {
        Void("Program complete");
    }
}

void Log::Split(Level level, bool returnLog)
{
    int option = std::rand() % 5;
    std::string splitter;
    switch (option)
    {
    case 1:
        splitter = "===";
        break;
    case 2:
        splitter = "+++";
        break;
    case 3:
        splitter = "=+=";
        break;
    case 4:
        splitter = "+=+";
        break;
    }
    Write("\033[36m#" + splitter, std::source_location::current(), level, returnLog);
}

void Log::UpdateGlobals(const std::string &msg, Level level)
{
    lastMsg = msg;
    lastLevel = level;
}

void Log::Debug(const std::string &msg, const std::source_location &loc, bool returnLog)
{
    Write(msg, loc, Level::DEBUG);
}

bool Log::True(const std::string &msg, const std::source_location &loc)
{
    Background(msg, loc, true);
    return true;
}

bool Log::False(const std::string &msg, const std::source_location &loc)
{
    Error(msg, loc, true);
    return false;
}

void Log::Void(const std::string &msg, const std::source_location &loc)
{
    Background(msg, loc, true);
}

void Log::Msg(const std::string &msg, const std::source_location &loc, const Level level, bool returnVoid)
{
    Write(msg, loc, level, returnVoid);
}

bool Log::Msg(const std::string &msg, const std::source_location &loc, const Level level, BoolType type)
{
    bool returnValue;
    switch (type)
    {
    case BoolType::TRUE:
        returnValue = true;
        break;
    case BoolType::FALSE:
        returnValue = false;
        break;
    }
    Write(msg, loc, level, true);

    return returnValue;
}

void Log::Write(const std::string &msg, const std::source_location &loc, Level level, bool returnLog)
{
    if (allFilter)
        return;
    if (debugFilter == true && level != Level::DEBUG)
        return;

    double duration = GetDuration();
    std::string output = GetOutput(msg, level, loc, duration, returnLog);

    if (level == Level::ERROR)
    {
        std::cerr << output;
    }
    else
    {
        std::cout << output;
    }

    UpdateGlobals(msg, level);
}

void Log::Error(const std::string &msg, const std::source_location &loc, bool returnLog)
{
    Write(msg, loc, Level::ERROR, returnLog);
}

void Log::Warn(const std::string &msg, const std::source_location &loc, bool returnLog)
{
    Write(msg, loc, Level::WARN, returnLog);
}

void Log::Description(const std::string &msg, const std::source_location &loc, bool returnLog)
{
    Write(msg, loc, Level::DESC, returnLog);
}

void Log::Info(const std::string &msg, std::source_location loc, bool returnLog)
{
    Write(msg, loc, Level::INFO, returnLog);
}

void Log::Background(const std::string &msg, const std::source_location &loc, bool returnLog)
{
    Write(msg, loc, Level::BACKGROUND, returnLog);
}

bool Log::Erase(std::string &str, std::string start, char end)
{
    int startIndex = str.find_first_of(start);
    if (startIndex == std::string::npos)
        return false;

    int length = 0;
    for (int i = startIndex; i < str.size(); i++)
    {
        length++;
        if (str[i] == end)
        {
            break;
        }
    }

    str.erase(startIndex, length);

    return true;
}

uint16_t Log::StringSize(std::string str)
{
    CleanString(str);
    return str.size();
}

void Log::CleanString(std::string &str)
{
    std::string ascii = "\033[";
    while (true)
    {
        if (!Erase(str, ascii, 'm'))
            break;
    }
}
