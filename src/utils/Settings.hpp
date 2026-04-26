#pragma once

#include <filesystem>
#include "tinyxml2.h"

// All user-configurable settings that persist between sessions.
// Loaded at startup; saved at shutdown.
struct Settings
{
    // Analysis
    float overhangAngle = 45.0f;
    float sharpCornerAngle = 100.0f;
    float minFeatureSize = 0.4f;
    float thinMinWidth = 2.0f;
    float layerHeight = 0.2f;

    // Appearance
    float accentHue = 220.0f;
    float accentSat = 0.35f;
    bool accentUseSystem = true;
    int themeMode = 0; // 0=System, 1=Light, 2=Dark

    // Viewport
    float gridExtent = 256.0f;

    // Navigation
    float mouseSensitivity = 30.0f;

    // Returns the platform-appropriate path to settings.xml.
    // Uses SDL_GetPrefPath so the directory is created if it doesn't exist.
    static std::filesystem::path DefaultPath();

    // Persist to disk. Returns true on success.
    bool Save(const std::filesystem::path &path) const
    {
        tinyxml2::XMLDocument doc;

        tinyxml2::XMLElement *root = doc.NewElement("Settings");
        doc.InsertFirstChild(root);

        auto writeFloat = [&](const char *name, float val)
        {
            tinyxml2::XMLElement *el = doc.NewElement(name);
            el->SetAttribute("value", val);
            root->InsertEndChild(el);
        };
        auto writeInt = [&](const char *name, int val)
        {
            tinyxml2::XMLElement *el = doc.NewElement(name);
            el->SetAttribute("value", val);
            root->InsertEndChild(el);
        };
        auto writeBool = [&](const char *name, bool val)
        {
            tinyxml2::XMLElement *el = doc.NewElement(name);
            el->SetAttribute("value", val);
            root->InsertEndChild(el);
        };

        writeFloat("overhangAngle", overhangAngle);
        writeFloat("sharpCornerAngle", sharpCornerAngle);
        writeFloat("minFeatureSize", minFeatureSize);
        writeFloat("thinMinWidth", thinMinWidth);
        writeFloat("layerHeight", layerHeight);
        writeFloat("accentHue", accentHue);
        writeFloat("accentSat", accentSat);
        writeBool("accentUseSystem", accentUseSystem);
        writeInt("themeMode", themeMode);
        writeFloat("gridExtent", gridExtent);
        writeFloat("mouseSensitivity", mouseSensitivity);

        return doc.SaveFile(path.c_str()) == tinyxml2::XML_SUCCESS;
    }

    // Load from disk, replacing only fields that are present in the file.
    // Unknown or missing fields keep their default values, so adding new settings
    // is backwards-compatible with old settings files.
    // Returns true if the file was found and parsed without errors.
    bool Load(const std::filesystem::path &path)
    {
        tinyxml2::XMLDocument doc;
        if (doc.LoadFile(path.c_str()) != tinyxml2::XML_SUCCESS)
            return false;

        tinyxml2::XMLElement *root = doc.FirstChildElement("Settings");
        if (!root)
            return false;

        auto readFloat = [&](const char *name, float &out)
        {
            if (tinyxml2::XMLElement *el = root->FirstChildElement(name))
                el->QueryFloatAttribute("value", &out);
        };
        auto readInt = [&](const char *name, int &out)
        {
            if (tinyxml2::XMLElement *el = root->FirstChildElement(name))
                el->QueryIntAttribute("value", &out);
        };
        auto readBool = [&](const char *name, bool &out)
        {
            if (tinyxml2::XMLElement *el = root->FirstChildElement(name))
                el->QueryBoolAttribute("value", &out);
        };

        readFloat("overhangAngle", overhangAngle);
        readFloat("sharpCornerAngle", sharpCornerAngle);
        readFloat("minFeatureSize", minFeatureSize);
        readFloat("thinMinWidth", thinMinWidth);
        readFloat("layerHeight", layerHeight);
        readFloat("accentHue", accentHue);
        readFloat("accentSat", accentSat);
        readBool("accentUseSystem", accentUseSystem);
        readInt("themeMode", themeMode);
        readFloat("gridExtent", gridExtent);
        readFloat("mouseSensitivity", mouseSensitivity);

        return true;
    }
};
