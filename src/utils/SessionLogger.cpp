#include "SessionLogger.hpp"
#include "log.hpp"
#include "scene/scene.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <mutex>
#include <random>

namespace
{
bool SessionLogEagerFlushEnabled()
{
    static constexpr const char *kNames[] = {
        "CAD_SESSION_LOG_EAGER_FLUSH",
        "CAD_SESSION_LOG_FLUSH_AFTER_STRUCTURE",
    };
    for (const char *name : kNames)
    {
        const char *e = std::getenv(name);
        if (e == nullptr || e[0] == '\0')
            continue;
        if (e[0] == '0' && e[1] == '\0')
            continue;
        return true;
    }
    return false;
}
}  // namespace

SessionLogger &SessionLogger::Instance()
{
    static SessionLogger instance;
    return instance;
}

void SessionLogger::Start()
{
    startTime = std::chrono::steady_clock::now();
    {
        std::random_device rd;
        std::mt19937_64 gen(rd());
        std::uniform_int_distribution<uint64_t> dist;
        sessionRunId = std::to_string(
                           std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count()) +
                       "_" + std::to_string(dist(gen));
    }
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
    MaybeFlushAfterImport();
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
    PushEvent("bug_marker", BuildFullSessionSnapshotFields(state));
    Log::Session("BUG MARKER — full state snapshot recorded");
}

void SessionLogger::LogSessionEndSnapshot(bool echoToConsole)
{
    PushEvent("session_end", BuildFullSessionSnapshotFields(state));
    if (echoToConsole)
        Log::Session("Session end snapshot recorded");
}

namespace
{
constexpr const char *kSessionLogPath = "session_log.json";
}

void SessionLogger::LogShutdownPhase(const std::string &phase)
{
    PushEvent("shutdown_phase", {{"phase", "\"" + EscapeStr(phase) + "\""}});
    // Default: buffer only — `main::Shutdown` flushes once at the end so quitting does not rewrite
    // `session_log.json` dozens of times (IDE file watchers + disk churn looked like an instant hitch).
    // Set CAD_SESSION_LOG_SHUTDOWN_PER_PHASE_FLUSH=1 to restore per-phase flush for hang forensics.
    const char *e = std::getenv("CAD_SESSION_LOG_SHUTDOWN_PER_PHASE_FLUSH");
    const bool perPhaseFlush = e != nullptr && e[0] != '\0' && e[0] != '0';
    if (perPhaseFlush)
        Flush(kSessionLogPath, false);
}

void SessionLogger::LogStructureStagingWorkerResult(uint64_t jobId, uint64_t issuedJobId, bool cancelled,
                                                    size_t carvedSolids, size_t carveAttempts, bool hasStaging,
                                                    const std::string &firstErr, size_t targetSceneIndex)
{
    std::string errSnippet = "null";
    if (!firstErr.empty())
    {
        std::string u = firstErr;
        if (u.size() > 240)
            u.resize(240);
        errSnippet = std::string("\"") + EscapeStr(u) + "\"";
    }
    PushEvent("structure_staging_worker_result",
              {
                  {"job_id", std::to_string(jobId)},
                  {"issued_job_id", std::to_string(issuedJobId)},
                  {"job_id_matches", (jobId == issuedJobId) ? "true" : "false"},
                  {"cancelled", cancelled ? "true" : "false"},
                  {"carved_solids", std::to_string(carvedSolids)},
                  {"carve_attempts", std::to_string(carveAttempts)},
                  {"has_staging", hasStaging ? "true" : "false"},
                  {"has_first_err", firstErr.empty() ? "false" : "true"},
                  {"first_err_snippet", std::move(errSnippet)},
                  {"target_scene", std::to_string(targetSceneIndex)},
              });
}

void SessionLogger::LogStructureStagingWorkerException()
{
    PushEvent("structure_staging_worker_exception", {});
}

void SessionLogger::LogStructureStagingJobSubmitted(uint64_t jobId, size_t targetSceneIndex,
                                                    size_t solidCarveGroups, size_t eligibleFaceCount)
{
    PushEvent("structure_staging_job_submitted",
              {
                  {"job_id", std::to_string(jobId)},
                  {"target_scene", std::to_string(targetSceneIndex)},
                  {"solid_carve_groups", std::to_string(solidCarveGroups)},
                  {"eligible_face_count", std::to_string(eligibleFaceCount)},
              });
}

void SessionLogger::LogStructureStagingWorkerStarted(uint64_t jobId)
{
    PushEvent("structure_staging_worker_started", {{"job_id", std::to_string(jobId)}});
}

void SessionLogger::LogStructureStagingApplyPhase(const std::string &phase, const std::string &detail)
{
    std::vector<std::pair<std::string, std::string>> fields = {
        {"phase", "\"" + EscapeStr(phase) + "\""},
    };
    if (!detail.empty())
        fields.push_back({"detail", "\"" + EscapeStr(detail) + "\""});
    PushEvent("structure_staging_apply_phase", std::move(fields));
}

void SessionLogger::MaybeFlushAfterStructurePoll(const std::string &path)
{
    if (!SessionLogEagerFlushEnabled())
        return;
    Flush(path, false);
}

void SessionLogger::MaybeFlushAfterImport(const std::string &path)
{
    if (!SessionLogEagerFlushEnabled())
        return;
    Flush(path, false);
}

void SessionLogger::LogStlMergeDiagnostics(const std::string &filename, const STLImportStats &stl)
{
    if (!stl.hasMergeDiagnostics)
        return;

    auto fmtD = [](double v)
    {
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(8) << v;
        return ss.str();
    };

    const MergeCoplanarDiagnostics &d = stl.mergeDiagnostics;

    PushEvent("stl_merge_diagnostics", {
                                            {"filename", "\"" + EscapeStr(filename) + "\""},
                                            {"stl_binary", stl.isBinary ? "true" : "false"},
                                            {"triangle_count_header", std::to_string(stl.triangleCount)},
                                            {"tri_faces_emitted", std::to_string(stl.faces)},
                                            {"unique_points", std::to_string(stl.uniquePoints)},
                                            {"parse_ms", fmtD(stl.parseMs)},
                                            {"merge_ms", fmtD(stl.mergeMs)},
                                            {"stl_importer_total_ms", fmtD(stl.totalMs)},
                                            {"merge_ran", d.mergeRan ? "true" : "false"},
                                            {"faces_before_merge", std::to_string(d.facesBefore)},
                                            {"faces_after_merge", std::to_string(d.facesAfter)},
                                            {"merge_while_iterations", std::to_string(d.mergeWhileIterations)},
                                            {"merge_operations", std::to_string(d.mergeOperations)},
                                            {"boundary_loop_failures", std::to_string(d.boundaryLoopFailures)},
                                            {"edges_1_face_before", std::to_string(d.edgesOneFaceBefore)},
                                            {"edges_2_faces_before", std::to_string(d.edgesTwoFacesBefore)},
                                            {"edges_3plus_faces_before", std::to_string(d.edgesThreePlusBefore)},
                                            {"edges_1_face_after", std::to_string(d.edgesOneFaceAfter)},
                                            {"edges_2_faces_after", std::to_string(d.edgesTwoFacesAfter)},
                                            {"edges_3plus_faces_after", std::to_string(d.edgesThreePlusAfter)},
                                            {"bbox_diagonal", fmtD(d.bboxDiagonal)},
                                            {"plane_tol_used", fmtD(d.planeTolUsed)},
                                        });
    Log::Session("STL merge diagnostics recorded for " + filename);
}

void SessionLogger::Flush(const std::string &path, bool logToConsole)
{
    std::ofstream file(path);
    if (!file.is_open())
    {
        Log::Error("Failed to write session log: " + path);
        return;
    }
    file << SerializeJson();
    if (logToConsole)
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
    std::lock_guard<std::mutex> lock(eventsMutex);
    events.push_back({ElapsedMs(), type, std::move(fields)});
}

std::string SessionLogger::SerializeJson() const
{
    std::lock_guard<std::mutex> lock(eventsMutex);
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
    out << "  \"session_run_id\": \"" << EscapeStr(sessionRunId) << "\",\n";
    out << "  \"events\": [\n";

    for (size_t i = 0; i < events.size(); ++i)
    {
        const auto &ev = events[i];
        const uint64_t dtMs = (i == 0) ? 0u : (ev.t_ms - events[i - 1].t_ms);
        out << "    { \"t_ms\": " << ev.t_ms << ", \"dt_ms\": " << dtMs
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

    out << "  ],\n";
    if (!events.empty())
        out << "  \"last_event_t_ms\": " << events.back().t_ms << ",\n";
    out << "  \"event_count\": " << events.size() << "\n";
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

static std::string Fmt6(float v)
{
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(6) << v;
    return ss.str();
}

std::vector<std::pair<std::string, std::string>> SessionLogger::BuildFullSessionSnapshotFields(const SessionState &s)
{
    auto fmtVec3 = [](const glm::vec3 &v)
    {
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(4) << "[" << v.x << ", " << v.y << ", " << v.z << "]";
        return ss.str();
    };

    const char *toolStr = (s.activeToolOrdinal == 1)   ? "calibrate"
                         : (s.activeToolOrdinal == 2) ? "structure"
                                                      : "analysis";

    return {
        {"points", std::to_string(s.points)},
        {"edges", std::to_string(s.edges)},
        {"faces", std::to_string(s.faces)},
        {"solids", std::to_string(s.solids)},
        {"last_file", "\"" + EscapeStr(s.lastFilename) + "\""},
        {"format", "\"" + s.lastFormat + "\""},
        {"overhang_angle", Fmt(s.overhangAngle)},
        {"sharp_corner_angle", Fmt(s.sharpCornerAngle)},
        {"thin_min_width", Fmt(s.thinMinWidth)},
        {"min_feature_size", Fmt(s.minFeatureSize)},
        {"layer_height", Fmt(s.layerHeight)},
        {"overhangs", std::to_string(s.overhangs)},
        {"sharp_edges", std::to_string(s.sharpEdges)},
        {"thin_sections", std::to_string(s.thinSections)},
        {"small_features", std::to_string(s.smallFeatures)},
        {"camera_target", "\"" + fmtVec3(s.cameraTarget) + "\""},
        {"camera_ortho_size", Fmt(s.cameraOrthoSize)},
        {"camera_position", "\"" + fmtVec3(s.cameraPosition) + "\""},
        {"camera_distance", Fmt(s.cameraDistance)},
        {"camera_quat_w", Fmt6(s.cameraQuatW)},
        {"camera_quat_x", Fmt6(s.cameraQuatX)},
        {"camera_quat_y", Fmt6(s.cameraQuatY)},
        {"camera_quat_z", Fmt6(s.cameraQuatZ)},
        {"camera_near_plane", Fmt(s.cameraNearPlane)},
        {"camera_far_plane", Fmt(s.cameraFarPlane)},
        {"window_logical_w", std::to_string(s.windowLogicalW)},
        {"window_logical_h", std::to_string(s.windowLogicalH)},
        {"window_pixels_w", std::to_string(s.windowPixelsW)},
        {"window_pixels_h", std::to_string(s.windowPixelsH)},
        {"active_tool", "\"" + std::string(toolStr) + "\""},
        {"viewport_analysis_enabled", s.viewportAnalysisEnabled ? "true" : "false"},
        {"depth_experiment_ordinal", std::to_string(static_cast<int>(s.depthExperimentOrdinal))},
    };
}
