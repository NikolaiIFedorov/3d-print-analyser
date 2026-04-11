#include "OpenGLShader.hpp"

#include "utils/log.hpp"

OpenGLShader::~OpenGLShader()
{
    Delete();
}

std::string OpenGLShader::ReadFile(const std::string &filePath)
{
    std::ifstream file(filePath);
    if (!file.is_open())
    {
        return "";
    }

    std::stringstream buffer;
    buffer << file.rdbuf();

    return buffer.str();
}

GLuint OpenGLShader::CompileShader(GLenum type, const std::string &source)
{
    GLuint shader = glCreateShader(type);
    const char *src = source.c_str();
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);

    return shader;
}

bool OpenGLShader::GetCompileError(GLuint shader, const std::string &type)
{
    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);

    if (!success)
    {
        GLchar infoLog[1024];
        glGetShaderInfoLog(shader, 1024, nullptr, infoLog);

        std::string error = "Shader failed to compile";
        return LOG_FALSE(error + infoLog);
    }

    return true;
}

bool OpenGLShader::GetLinkError(GLuint program)
{
    GLint success;
    glGetProgramiv(program, GL_LINK_STATUS, &success);

    if (!success)
    {
        GLchar infoLog[1024];
        glGetProgramInfoLog(program, 1024, nullptr, infoLog);
        std::string error = "Failed to link program: ";
        return LOG_FALSE(error + infoLog);
    }
    return true;
}

bool OpenGLShader::LoadFromFiles(const std::string &vertexPath,
                                 const std::string &fragmentPath)
{
    std::string vertexSource = ReadFile(vertexPath);
    std::string fragmentSource = ReadFile(fragmentPath);

    if (vertexSource.empty())
    {
        return LOG_FALSE("Vertex source is empty: " + vertexPath);
    }
    else if (fragmentSource.empty())
    {
        return LOG_FALSE("Fragment source is empty: " + fragmentPath);
    }

    GLuint vertexShader = CompileShader(GL_VERTEX_SHADER, vertexSource);
    if (!GetCompileError(vertexShader, "VERTEX"))
    {
        glDeleteShader(vertexShader);
        return LOG_FALSE("Failed to compile shader");
    }

    GLuint fragmentShader = CompileShader(GL_FRAGMENT_SHADER, fragmentSource);
    if (!GetCompileError(fragmentShader, "FRAGMENT"))
    {
        glDeleteShader(vertexShader);
        glDeleteShader(fragmentShader);
        return LOG_FALSE("Failed to compile shader");
    }

    programID = glCreateProgram();
    glAttachShader(programID, vertexShader);
    glAttachShader(programID, fragmentShader);
    glLinkProgram(programID);

    if (!GetLinkError(programID))
    {
        glDeleteShader(vertexShader);
        glDeleteShader(fragmentShader);
        glDeleteProgram(programID);
        programID = 0;
        return LOG_FALSE("Failed to link program");
    }

    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);

    return true;
}

bool OpenGLShader::LoadFromFiles(const std::string &vertexPath,
                                 const std::string &geometryPath,
                                 const std::string &fragmentPath)
{
    std::string vertexSource = ReadFile(vertexPath);
    std::string geometrySource = ReadFile(geometryPath);
    std::string fragmentSource = ReadFile(fragmentPath);

    if (vertexSource.empty())
        return LOG_FALSE("Vertex source is empty: " + vertexPath);
    if (geometrySource.empty())
        return LOG_FALSE("Geometry source is empty: " + geometryPath);
    if (fragmentSource.empty())
        return LOG_FALSE("Fragment source is empty: " + fragmentPath);

    GLuint vertexShader = CompileShader(GL_VERTEX_SHADER, vertexSource);
    if (!GetCompileError(vertexShader, "VERTEX"))
    {
        glDeleteShader(vertexShader);
        return LOG_FALSE("Failed to compile vertex shader");
    }

    GLuint geometryShader = CompileShader(GL_GEOMETRY_SHADER, geometrySource);
    if (!GetCompileError(geometryShader, "GEOMETRY"))
    {
        glDeleteShader(vertexShader);
        glDeleteShader(geometryShader);
        return LOG_FALSE("Failed to compile geometry shader");
    }

    GLuint fragmentShader = CompileShader(GL_FRAGMENT_SHADER, fragmentSource);
    if (!GetCompileError(fragmentShader, "FRAGMENT"))
    {
        glDeleteShader(vertexShader);
        glDeleteShader(geometryShader);
        glDeleteShader(fragmentShader);
        return LOG_FALSE("Failed to compile fragment shader");
    }

    programID = glCreateProgram();
    glAttachShader(programID, vertexShader);
    glAttachShader(programID, geometryShader);
    glAttachShader(programID, fragmentShader);
    glLinkProgram(programID);

    if (!GetLinkError(programID))
    {
        glDeleteShader(vertexShader);
        glDeleteShader(geometryShader);
        glDeleteShader(fragmentShader);
        glDeleteProgram(programID);
        programID = 0;
        return LOG_FALSE("Failed to link program");
    }

    glDeleteShader(vertexShader);
    glDeleteShader(geometryShader);
    glDeleteShader(fragmentShader);

    return true;
}

void OpenGLShader::Use()
{
    if (programID != 0)
    {
        glUseProgram(programID);
    }
}

void OpenGLShader::Delete()
{
    if (programID != 0)
    {
        glDeleteProgram(programID);
        programID = 0;
    }
}

void OpenGLShader::SetMat4(const std::string &name, const glm::mat4 &matrix)
{
    GLint location = glGetUniformLocation(programID, name.c_str());

    if (location == -1)
    {
        return;
    }

    glUniformMatrix4fv(location, 1, GL_FALSE, glm::value_ptr(matrix));
}

void OpenGLShader::SetVec3(const std::string &name, const glm::vec3 &vec)
{
    GLint location = glGetUniformLocation(programID, name.c_str());
    if (location != -1)
        glUniform3fv(location, 1, glm::value_ptr(vec));
}

void OpenGLShader::SetVec4(const std::string &name, const glm::vec4 &vec)
{
    GLint location = glGetUniformLocation(programID, name.c_str());
    if (location != -1)
        glUniform4fv(location, 1, glm::value_ptr(vec));
}

void OpenGLShader::SetVec2(const std::string &name, const glm::vec2 &vec)
{
    GLint location = glGetUniformLocation(programID, name.c_str());
    if (location != -1)
        glUniform2fv(location, 1, glm::value_ptr(vec));
}

void OpenGLShader::SetFloat(const std::string &name, float value)
{
    GLint location = glGetUniformLocation(programID, name.c_str());
    if (location != -1)
        glUniform1f(location, value);
}

void OpenGLShader::SetInt(const std::string &name, int value)
{
    GLint location = glGetUniformLocation(programID, name.c_str());
    if (location != -1)
        glUniform1i(location, value);
}