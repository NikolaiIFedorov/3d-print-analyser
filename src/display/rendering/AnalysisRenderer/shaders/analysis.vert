#version 410 core

layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec4 aColor;

out vec4 fragColor;

uniform mat4 uViewProjection;
uniform mat4 uModel;

void main()
{
    gl_Position = uViewProjection * uModel * vec4(aPosition, 1.0);
    gl_Position.z -= 0.001 * gl_Position.w;
    fragColor = aColor;
}
