#pragma once

#include <glad/glad.h>

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <fstream>
#include <sstream>
#include <iostream>

#include <string>

class OpenGLShader
{
private:
    GLuint programID = 0;

    std::string ReadFile(const std::string &filePath);
    GLuint CompileShader(GLenum type, const std::string &source);
    bool GetCompileError(GLuint shader, const std::string &type);
    bool GetLinkError(GLuint program);

public:
    OpenGLShader() = default;
    ~OpenGLShader();

    bool LoadFromFiles(const std::string &vertexPath,
                       const std::string &fragmentPath);
    void Use();
    void Delete();

    void SetMat4(const std::string &name, const glm::mat4 &matrix);
    void SetVec3(const std::string &name, const glm::vec3 &vec);
    void SetFloat(const std::string &name, float value);

    GLuint GetProgramId() const { return programID; }
};