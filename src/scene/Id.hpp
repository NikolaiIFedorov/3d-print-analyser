#pragma once

#include <climits>
#include "utils/log.hpp"

static uint32_t lastPointId = 1;
static uint32_t lastEdgeId = 1;
static uint32_t lastCurveId = 1;
static uint32_t lastFaceId = 1;
static uint32_t lastSolidId = 1;

enum class Type
{
    POINT,
    EDGE,
    CURVE,
    FACE,
    SOLID,
    FORM_NULL
};

struct Id
{
    static uint32_t GetId(Type type)
    {
        uint32_t candidateId;

        uint32_t *lastId = nullptr;
        switch (type)
        {
        case Type::POINT:
            candidateId = POINT_START_ID;
            lastId = &lastPointId;
            break;
        case Type::EDGE:
            candidateId = EDGE_START_ID;
            lastId = &lastEdgeId;
            break;
        case Type::CURVE:
            candidateId = CURVE_START_ID;
            lastId = &lastCurveId;
            break;
        case Type::FACE:
            candidateId = FACE_START_ID;
            lastId = &lastFaceId;
            break;
        case Type::SOLID:
            candidateId = SOLID_START_ID;
            lastId = &lastSolidId;
            break;
        default:
            candidateId = -1;
            break;
        }

        uint32_t id;
        if (candidateId == -1)
        {
            id = -1;
        }
        else if (*lastId < FORM_TYPE_STEP)
        {
            id = candidateId + *lastId;
            *lastId += 1;
        }
        else
        {
        }
        return id;
    }

    static Type GetType(uint32_t id)
    {
        if (id < 0)
        {
            return Type::FORM_NULL;
        }

        if (id < EDGE_START_ID)
        {
            return Type::POINT;
        }
        else if (id < CURVE_START_ID)
        {
            return Type::EDGE;
        }
        else if (id < FACE_START_ID)
        {
            return Type::CURVE;
        }
        else if (id < SOLID_START_ID)
        {
            return Type::FACE;
        }
        else if (id < INT_MAX)
        {
            return Type::SOLID;
        }
        else
        {
            return Type::FORM_NULL;
        }
    }

private:
    static const uint32_t POINT_START_ID = 0;
    static const uint32_t EDGE_START_ID = 429496729;
    static const uint32_t CURVE_START_ID = 858993458;
    static const uint32_t FACE_START_ID = 1288490187;
    static const uint32_t SOLID_START_ID = 1717986916;

    static const uint32_t FORM_TYPE_STEP = 429496729;
};