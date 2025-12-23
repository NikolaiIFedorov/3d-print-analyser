#include <cstdint>
#include <vector>

struct Edge
{
    uint32_t id;

    uint32_t startPointId;
    uint32_t endPointId;
    uint32_t curveId = 0;

    std::vector<uint32_t> bridgePoints;
};
