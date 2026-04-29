#version 410 core

layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aColor;
layout(location = 2) in vec3 aNormal;

uniform mat4 uViewProjection;
uniform mat4 uModel;
// Clip-space Z nudge: subtract `uWireZNudgeNdc * gl_Position.w` (CPU supplies 0 or ~1e-6×scale).
uniform float uWireZNudgeNdc;

out VS_OUT {
    vec3 color;
} vs_out;

void main()
{
    gl_Position = uViewProjection * uModel * vec4(aPosition, 1.0);
    gl_Position.z -= uWireZNudgeNdc * gl_Position.w;
    vs_out.color = aColor;
}
