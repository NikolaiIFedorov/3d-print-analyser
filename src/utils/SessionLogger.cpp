#include "SessionLogger.hpp"
#include "log.hpp"

#include <fstream>
#include <sstream>
#include <iomanip>
#include <ctime>

SessionLogger &SessionLogger::Instance()
{
    static SessionLogger instance;
    return instance;
}

void SessionLogger::Start()
{
    startTime = std::chrono::steady_clock::now();
    PushEvent("app_start", {});
    Log::Session("Session started");
}

void SessionLogger::LogFileImport(const std::string &filename, const std::string &format)
{
    PushEvent("file_import", {
                                 {"filename", "\"" + EscapeStr(filename) + "\""},
                                 {"format", "\"" + format + "\""},
                                 {"points", std::to_string(state.points)},
                                 {"edges", std::to_string(state.edges)},
                                 {"faces", std::to_string(state.faces)},
                                 {"solids", std::to_string(state.solids)},
                             });
    Log::Session("File imported: " + filename +
                 " (" + std::to_string(state.faces) + " faces, " +
                 std::to_string(state.solids) + " solids)");
}

void SessionLogger::LogAnalysisRun()
{
    PushEvent("analysis_run", {
                                  {"overhangs", std::to_string(state.overhangs)},
                                  {"sharp_edges", std::to_string(state.sharpEdges)},
                                  {"thin_sections", std::to_string(state.thinSections)},
                                  {"small_features", std::to_string(state.smallFeatures)},
                                  {"overhang_angle", Fmt(state.overhangAngle)},
                                  {"sharp_corner_angle", Fmt(state.sharpCornerAngle)},
                                  {"thin_min_width", Fmt(state.thinMinWidth)},
                                  {"min_feature_size", Fmt(state.minFeatureSize)},
                                  {"layer_height", Fmt(state.layerHeight)},
                              });
    Log::Session("Analysis: " +
                 std::to_string(state.overhangs) + " overhangs, " +
                 std::to_string(state.sharpEdges) + " sharp edges, " +
                 std::to_string(state.thinSections) + " thin sections, " +
                 std::to_string(state.smallFeatures) + " small features");
}

void SessionLogger::LogParamChange(const std::string &param, float value)
{
    PushEvent("param_change", {
                                  {"param", "\"" + param + "\""},
                                  {"value", Fmt(value)},
                              });
    Log::Session("Param changed: " + param + " = " + Fmt(value));
}

void SessionLogger::LogBugMarker()
{
    auto fmtVec3 = [](const glm::vec3 &v)
    {
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(2)
           << "[" << v.x << ", " << v.y << ", " << v.z << "]";
        return ss.str();
    };

    PushEvent("bug_marker", {
                                // Scene
                                {"points", std::to_string(state.points)},
                                {"edges", std::to_string(state.edges)},
                                {"faces", std::to_string(state.faces)},
                                {"solids", std::to_string(state.solids)},
                                {"last_file", "\"" + EscapeStr(state.lastFilename) + "\""},
                                {"format", "\"" + state.lastFormat + "\""},
                                // Analysis params
                                {"overhang_angle", Fmt(state.overhangAngle)},
                                {"sharp_corner_angle", Fmt(state.sharpCornerAngle)},
                                {"thin_min_width", Fmt(state.thinMinWidth)},
                                {"min_feature_size", Fmt(state.minFeatureSize)},
                                {"layer_height", Fmt(state.layerHeight)},
                                // Flaw counts
                                {"overhangs", std::to_string(state.overhangs)},
                                {"sharp_edges", std::to_string(state.sharpEdges)},
                                {"thin_sections", std::to_string(state.thinSections)},
                                {"small_features", std::to_string(state.smallFeatures)},
                                // Camera
                                {"camera_target", "\"" + fmtVec3(state.cameraTarget) + "\""},
                                {"camera_ortho_size", Fmt(state.cameraOrthoSize)},
                            });
    Log::Session("BUG MARKER — full state snapshot recorded");
}

void SessionLogger::Flush(const std::string &path)
{
    std::ofstream file(path);
    if (!file.is_open())
    {
        Log::Error("Failed to write session log: " + path);
        return;
    }
    file << SerializeJson();
    Log::Session("Session log written to: " + path);
}

uint64_t SessionLogger::ElapsedMs() const
{
    auto now = std::chrono::steady_clock::now();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime).count());
}

void SessionLogger::PushEvent(const std::string &type,
                              std::vector<std::pair<std::string, std::string>> fields)
{
    events.push_back({ElapsedMs(), type, std::move(fields)});
}

std::string SessionLogger::SerializeJson() const
{
    // Wall-clock timestamp for the session_start field
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char timeBuf[32];
    std::strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%dT%H:%M:%SZ", &tm);

    std::ostringstream out;
    out << "{\n";
    out << "  \"session_start\": \"" << timeBuf << "\",\n";
    out << "  \"events\": [\n";

    for (size_t i = 0; i < events.size(); ++i)
    {
        const auto &ev = events[i];
        out << "    { \"t_ms\": " << ev.t_ms
            << ", \"type\": \"" << ev.type << "\"";

        if (!ev.fields.empty())
        {
            out << ", \"data\": { ";
            for (size_t j = 0; j < ev.fields.size(); ++j)
            {
                out << "\"" << ev.fields[j].first << "\": " << ev.fields[j].second;
                if (j + 1 < ev.fields.size())
                    out << ", ";
            }
            out << " }";
        }

        out << " }";
        if (i + 1 < events.size())
            out << ",";
        out << "\n";
    }

    out << "  ]\n";
    out << "}\n";
    return out.str();
}

std::string SessionLogger::EscapeStr(const std::string &s)
{
    std::string result;
    result.reserve(s.size());
    for (char c : s)
    {
        switch (c)
        {
        case '"':
            result += "\\\"";
            break;
        case '\\':
            result += "\\\\";
            break;
        case '\n':
            result += "\\n";
            break;
        case '\r':
            result += "\\r";
            break;
        case '\t':
            result += "\\t";
            break;
        default:
            result += c;
            break;
        }
    }
    return result;
}

std::string SessionLogger::Fmt(float v)
{
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(2) << v;
    return ss.str();
}
