#pragma once

#include <string>
#include <vector>
#include <chrono>
#include <glm/glm.hpp>

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

    // Camera
    glm::vec3 cameraTarget{0.0f};
    float cameraOrthoSize = 1.0f;
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
    void Flush(const std::string &path);

    // Mutable state — updated by Display at key points; read by LogBugMarker.
    SessionState state;

    // ── Event loggers ────────────────────────────────────────────────────────
    // Each one appends an event to the buffer and echoes a summary to terminal.

    void LogFileImport(const std::string &filename, const std::string &format);
    void LogAnalysisRun();
    void LogParamChange(const std::string &param, float value);
    void LogBugMarker();

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

    uint64_t ElapsedMs() const;
    void PushEvent(const std::string &type,
                   std::vector<std::pair<std::string, std::string>> fields);
    std::string SerializeJson() const;

    static std::string EscapeStr(const std::string &s);
    static std::string Fmt(float v);
};
