#version 410 core

layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aColor;
layout(location = 2) in vec3 aNormal;

uniform mat4 uViewProjection;
uniform mat4 uModel;
uniform float uLightingEnabled;
// Clip-space Z nudge: subtract `uWireZNudgeNdc * gl_Position.w` (CPU supplies 0 or ~1e-6×scale).
uniform float uWireZNudgeNdc;
// Same convention as `basic.vert`: push wireframe with scene depth layer vs grid/axes.
uniform float uClipZBiasW;

out VS_OUT {
    vec3 color;
    vec3 normal;
} vs_out;

void main()
{
    vec4 pos = uViewProjection * uModel * vec4(aPosition, 1.0);
    pos.z -= uWireZNudgeNdc * pos.w;
    float biasedZ = pos.z + uClipZBiasW * pos.w;
    // Keep depth layering bias from pushing primitives beyond clip planes when zoomed in.
    float clipMargin = max(1e-6, 1e-6 * abs(pos.w));
    pos.z = clamp(biasedZ, -pos.w + clipMargin, pos.w - clipMargin);
    gl_Position = pos;
    vs_out.color = aColor;
    if (uLightingEnabled > 0.5)
        vs_out.normal = normalize(mat3(transpose(inverse(uModel))) * aNormal);
    else
        vs_out.normal = vec3(0.0);
}
