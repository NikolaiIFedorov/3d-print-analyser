#pragma once
#include <vector>
#include <optional>
#include "scene/scene.hpp"
#include <glm/glm.hpp>

enum class Flaw
{
    OVERHANG,
    LOGIC_ERROR,
    NONE
};

class Analysis
{
public:
    struct Layer
    {
        double z;
        std::vector<std::vector<glm::dvec2>> contours;
        std::vector<uint32_t> intersectedFaceIds;
    };

    struct OverhangRegion
    {
        size_t layerIndex;
        std::vector<glm::dvec2> unsupportedContour;
        double area;
        double maxDistance;
    };

    struct SliceResult
    {
        std::vector<Layer> layers;
        std::vector<OverhangRegion> overhangs;
        double layerHeight;
        double minZ, maxZ;
    };

    /*static SliceResult SliceModel(const Scene &scene,
                                  uint32_t solidId,
                                  double layerHeight = 0.2);*/

    static Flaw GetFlaw(const Face *face);

    static void SetCriticalAngle(float angle) { /*criticalAngle = angle*/ ; }
    static float GetCriticalAngle() { return /*criticalAngle*/ 5; }

private:
    // static float criticalAngle;

    static glm::dvec3 CalculateFaceNormal(const Face *face);
    static float CalculateOverhangAngle(const glm::dvec3 &normal);

    static std::vector<glm::dvec2> SliceFaceAtZ(const Face *face,
                                                double z);

    static std::optional<glm::dvec2> IntersectEdgeWithPlane(const Edge *edge,

                                                            double z);

    /*static void CalculateBounds(const Scene &scene, uint32_t solidId,
                                double &minZ, double &maxZ);*/
};