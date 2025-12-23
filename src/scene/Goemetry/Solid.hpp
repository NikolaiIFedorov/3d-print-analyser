#include <cstdint>
#include <vector>

struct Solid
{
    uint32_t id;

    std::vector<uint32_t> faceIds;
};