#pragma once

#include <string>
#include <utility>
#include <vector>
#include <chrono>
#include <glm/glm.hpp>

#include "logic/Import/STLImport.hpp"

// Snapshot of meaningful application state, updated at key events.
// Read by LogBugMarker to produce a full-state dump.
struct SessionState
{
    // Scene geometry counts
    size_t points = 0;
    size_t edges = 0;
    size_t faces = 0;
    size_t solids = 0;

    // Last imported file (filename only — no full path for privacy)
    std::string lastFilename;
    std::string lastFormat;

    // Analysis parameters
    float overhangAngle = 45.0f;
    float sharpCornerAngle = 100.0f;
    float thinMinWidth = 2.0f;
    float minFeatureSize = 0.4f;
    float layerHeight = 0.2f;

    // Flaw counts from the last analysis run
    size_t overhangs = 0;
    size_t sharpEdges = 0;
    size_t thinSections = 0;
    size_t smallFeatures = 0;

    // Camera & viewport (curated repro — filled by `Display::FillSessionReproState`)
    glm::vec3 cameraTarget{0.0f};
    float cameraOrthoSize = 1.0f;
    glm::vec3 cameraPosition{0.0f};
    float cameraDistance = 5.0f;
    float cameraQuatW = 1.0f;
    float cameraQuatX = 0.0f;
    float cameraQuatY = 0.0f;
    float cameraQuatZ = 0.0f;
    float cameraNearPlane = -100000.0f;
    float cameraFarPlane = 100000.0f;
    int windowLogicalW = 0;
    int windowLogicalH = 0;
    int windowPixelsW = 0;
    int windowPixelsH = 0;
    /// 0 = Analysis, 1 = Calibrate, 2 = Structure (matches `Display::ActiveTool`).
    uint8_t activeToolOrdinal = 0;
    bool viewportAnalysisEnabled = true;
    /// `ViewportDepthExperiment` as integer when that header is used; otherwise 0.
    uint8_t depthExperimentOrdinal = 0;
};

// Singleton session logger.
// Buffers structured events in memory, then flushes to a JSON file on demand.
// Each event is also echoed to the terminal via Log::Info / Log::Debug.
class SessionLogger
{
public:
    static SessionLogger &Instance();

    // Call once at app start.
    void Start();

    // Write buffered events to a JSON file at the given path.
    void Flush(const std::string &path, bool logToConsole = true);

    // Mutable state — updated by Display at key points; read by LogBugMarker.
    SessionState state;

    // ── Event loggers ────────────────────────────────────────────────────────
    // Each one appends an event to the buffer and echoes a summary to terminal.

    void LogFileImport(const std::string &filename, const std::string &format);
    void LogAnalysisRun();
    void LogParamChange(const std::string &param, float value);
    void LogBugMarker();

    /// Last-frame viewport + camera repro (call after `Display::FillSessionReproState`).
    /// When `echoToConsole` is false, only the JSON event is recorded (quit path: avoid blocking on a flooded terminal).
    void LogSessionEndSnapshot(bool echoToConsole = true);

    /// STL importer + coplanar merge diagnostics (written to session_log.json for offline analysis).
    void LogStlMergeDiagnostics(const std::string &filename, const STLImportStats &stl);

    /// Quit/teardown breadcrumb: appends a `shutdown_phase` event. By default the session log is **not**
    /// flushed here (see `main::Shutdown` for a single end flush); set `CAD_SESSION_LOG_SHUTDOWN_PER_PHASE_FLUSH=1`
    /// to flush `session_log.json` after every phase for mid-teardown hang forensics.
    void LogShutdownPhase(const std::string &phase);

    /// Structure CGAL carve worker finished (main thread, when the future is consumed). Compare
    /// `session_log.json` between models that freeze vs those that do not.
    void LogStructureStagingWorkerResult(uint64_t jobId, uint64_t issuedJobId, bool cancelled, size_t carvedSolids,
                                         size_t carveAttempts, bool hasStaging, const std::string &firstErr,
                                         size_t targetSceneIndex);
    void LogStructureStagingWorkerException();
    /// Carve job queued on `StructureCarveTaskRunner` (main thread, immediately after `Submit`; worker
    /// has not finished yet — use `structure_staging_worker_result` when the future is consumed).
    void LogStructureStagingJobSubmitted(uint64_t jobId, size_t targetSceneIndex, size_t solidCarveGroups,
                                          size_t eligibleFaceCount);
    /// Main-thread breadcrumbs after the worker future is consumed (`PollStructureStagingTaskIfReady`).
    /// `phase` is a short stable token (e.g. `applied_after_update_scene`, `discarded_stale_job`).
    void LogStructureStagingApplyPhase(const std::string &phase, const std::string &detail = {});

    /// If `CAD_SESSION_LOG_EAGER_FLUSH` or `CAD_SESSION_LOG_FLUSH_AFTER_STRUCTURE` is set (non-empty,
    /// first character not ASCII `0`), writes `path` (default `session_log.json` relative to **process cwd**).
    /// Used after Structure poll completes and **again immediately before `UpdateScene()`** on apply so a
    /// hang inside geometry rebuild still leaves a flushed log.
    void MaybeFlushAfterStructurePoll(const std::string &path = "session_log.json");
    /// Same env gate as `MaybeFlushAfterStructurePoll` — call after `LogFileImport` so import-only sessions
    /// create/update the log without waiting for Structure or quit.
    void MaybeFlushAfterImport(const std::string &path = "session_log.json");

private:
    SessionLogger() = default;

    struct Event
    {
        uint64_t t_ms;
        std::string type;
        // Each field: key → already-serialized JSON value (string or number literal)
        std::vector<std::pair<std::string, std::string>> fields;
    };

    std::vector<Event> events;
    std::chrono::steady_clock::time_point startTime;
    /// Unique per `Start()` so two exported `session_log.json` files are never ambiguous.
    std::string sessionRunId;

    uint64_t ElapsedMs() const;
    void PushEvent(const std::string &type,
                   std::vector<std::pair<std::string, std::string>> fields);
    std::string SerializeJson() const;

    static std::string EscapeStr(const std::string &s);
    static std::string Fmt(float v);

    static std::vector<std::pair<std::string, std::string>> BuildFullSessionSnapshotFields(const SessionState &s);
};
