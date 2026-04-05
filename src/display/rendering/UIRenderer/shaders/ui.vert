#version 410 core

layout(location = 0) in vec2 aPosition;
layout(location = 1) in vec4 aColor;

out vec4 fragColor;

uniform mat4 uProjection;

void main()
{
    gl_Position = uProjection * vec4(aPosition, 0.0, 1.0);
    fragColor = aColor;
}
