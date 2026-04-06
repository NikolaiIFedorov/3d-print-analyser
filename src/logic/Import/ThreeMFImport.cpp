#include "ThreeMFImport.hpp"
#include "scene/scene.hpp"
#include "utils/log.hpp"

#include <miniz.h>
#include <tinyxml2.h>

#include <vector>
#include <string>
#include <cstring>
#include <unordered_map>

static std::string FindModelFile(mz_zip_archive &zip)
{
    int numFiles = static_cast<int>(mz_zip_reader_get_num_files(&zip));
    for (int i = 0; i < numFiles; ++i)
    {
        char filename[256];
        mz_zip_reader_get_filename(&zip, i, filename, sizeof(filename));
        std::string name(filename);

        if (name.find(".model") != std::string::npos)
            return name;
    }
    return "";
}

static void ParseMesh(tinyxml2::XMLElement *meshElem, Scene *scene, std::vector<Face *> &faces)
{
    auto *verticesElem = meshElem->FirstChildElement("vertices");
    auto *trianglesElem = meshElem->FirstChildElement("triangles");

    if (!verticesElem || !trianglesElem)
        return;

    std::vector<Point *> points;

    for (auto *v = verticesElem->FirstChildElement("vertex"); v; v = v->NextSiblingElement("vertex"))
    {
        double x = v->DoubleAttribute("x");
        double y = v->DoubleAttribute("y");
        double z = v->DoubleAttribute("z");
        points.push_back(scene->CreatePoint(glm::dvec3(x, y, z)));
    }

    for (auto *t = trianglesElem->FirstChildElement("triangle"); t; t = t->NextSiblingElement("triangle"))
    {
        int v1 = t->IntAttribute("v1");
        int v2 = t->IntAttribute("v2");
        int v3 = t->IntAttribute("v3");

        int count = static_cast<int>(points.size());
        if (v1 < 0 || v1 >= count || v2 < 0 || v2 >= count || v3 < 0 || v3 >= count)
            continue;

        Point *p0 = points[v1];
        Point *p1 = points[v2];
        Point *p2 = points[v3];

        if (p0 == p1 || p1 == p2 || p0 == p2)
            continue;

        Edge *e1 = scene->CreateEdge(p0, p1);
        Edge *e2 = scene->CreateEdge(p1, p2);
        Edge *e3 = scene->CreateEdge(p2, p0);

        faces.push_back(scene->CreateFace({{e1, e2, e3}}));
    }
}

static void CollectMeshes(
    const std::string &objectId,
    const std::unordered_map<std::string, tinyxml2::XMLElement *> &objectMap,
    Scene *scene,
    std::vector<Face *> &faces)
{
    auto it = objectMap.find(objectId);
    if (it == objectMap.end())
    {
        LOG_WARN("3MF: object id '" + objectId + "' not found in map")
        return;
    }

    auto *object = it->second;

    auto *mesh = object->FirstChildElement("mesh");
    if (mesh)
    {
        LOG_DESC("3MF: parsing mesh from object " + objectId)
        ParseMesh(mesh, scene, faces);
    }

    auto *components = object->FirstChildElement("components");
    if (components)
    {
        for (auto *comp = components->FirstChildElement("component"); comp; comp = comp->NextSiblingElement("component"))
        {
            const char *refId = comp->Attribute("objectid");
            if (refId)
            {
                LOG_DESC("3MF: following component ref to object " + std::string(refId))
                CollectMeshes(std::string(refId), objectMap, scene, faces);
            }
        }
    }

    if (!mesh && !components)
    {
        for (auto *child = object->FirstChildElement(); child; child = child->NextSiblingElement())
        {
            LOG_DESC("3MF: object " + objectId + " has child: " + std::string(child->Name()))
        }
    }
}

bool ThreeMFImport::Import(const std::string &filePath, Scene *scene)
{
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, filePath.c_str(), 0))
        return LOG_FALSE("Failed to open 3MF archive: " + filePath);

    LOG_DESC("Importing 3MF: " + filePath)

    // Log all files in the archive
    int numFiles = static_cast<int>(mz_zip_reader_get_num_files(&zip));
    std::vector<std::string> modelFiles;
    for (int i = 0; i < numFiles; ++i)
    {
        char filename[512];
        mz_zip_reader_get_filename(&zip, i, filename, sizeof(filename));
        LOG_DESC("3MF archive file: " + std::string(filename))
        if (std::string(filename).find(".model") != std::string::npos)
            modelFiles.push_back(filename);
    }

    if (modelFiles.empty())
    {
        mz_zip_reader_end(&zip);
        return LOG_FALSE("No .model file found in 3MF archive");
    }

    // Parse all .model files and collect objects
    std::vector<std::unique_ptr<tinyxml2::XMLDocument>> docs;
    std::unordered_map<std::string, tinyxml2::XMLElement *> objectMap;
    tinyxml2::XMLElement *buildElem = nullptr;

    for (const auto &modelFile : modelFiles)
    {
        size_t xmlSize = 0;
        void *xmlData = mz_zip_reader_extract_file_to_heap(&zip, modelFile.c_str(), &xmlSize, 0);
        if (!xmlData)
            continue;

        auto doc = std::make_unique<tinyxml2::XMLDocument>();
        tinyxml2::XMLError err = doc->Parse(static_cast<const char *>(xmlData), xmlSize);
        mz_free(xmlData);

        if (err != tinyxml2::XML_SUCCESS)
            continue;

        auto *root = doc->RootElement();
        if (!root)
            continue;

        auto *resources = root->FirstChildElement("resources");
        if (resources)
        {
            for (auto *object = resources->FirstChildElement("object"); object; object = object->NextSiblingElement("object"))
            {
                const char *id = object->Attribute("id");
                if (id)
                {
                    LOG_DESC("3MF: indexing object id='" + std::string(id) + "' from " + modelFile)
                    objectMap[std::string(id)] = object;
                }
            }
        }

        if (!buildElem)
            buildElem = root->FirstChildElement("build");

        docs.push_back(std::move(doc));
    }
    mz_zip_reader_end(&zip);

    LOG_DESC("3MF: indexed " + std::to_string(objectMap.size()) + " objects from " + std::to_string(modelFiles.size()) + " model files")

    std::vector<Face *> faces;

    if (buildElem)
    {
        for (auto *item = buildElem->FirstChildElement("item"); item; item = item->NextSiblingElement("item"))
        {
            const char *objectId = item->Attribute("objectid");
            if (objectId)
            {
                LOG_DESC("3MF: build item -> object " + std::string(objectId))
                CollectMeshes(std::string(objectId), objectMap, scene, faces);
            }
        }
    }
    else
    {
        for (auto &[id, obj] : objectMap)
            CollectMeshes(id, objectMap, scene, faces);
    }

    LOG_DESC("3MF: total " + std::to_string(faces.size()) + " faces imported")

    if (!faces.empty())
        scene->CreateSolid(faces);

    return true;
}
