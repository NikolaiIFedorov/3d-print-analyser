#version 410 core

layout(lines) in;
layout(triangle_strip, max_vertices = 4) out;

uniform vec2 uViewportSize;
uniform float uLineWidth;

in VS_OUT {
    vec4 color;
} gs_in[];

out vec4 fragColor;

void main()
{
    vec4 p0 = gl_in[0].gl_Position;
    vec4 p1 = gl_in[1].gl_Position;

    vec2 ndc0 = p0.xy / p0.w;
    vec2 ndc1 = p1.xy / p1.w;

    // Compute direction in screen-pixel space for correct aspect ratio
    vec2 lineDir = (ndc1 - ndc0) * uViewportSize;
    float lineLen = length(lineDir);
    if (lineLen < 0.001) return;
    lineDir /= lineLen;

    // Perpendicular in screen space, converted back to NDC
    float halfWidth = uLineWidth * 0.5;
    vec2 offset = vec2(-lineDir.y, lineDir.x) * halfWidth / uViewportSize;

    fragColor = gs_in[0].color;
    gl_Position = vec4((ndc0 + offset) * p0.w, p0.z, p0.w);
    EmitVertex();
    gl_Position = vec4((ndc0 - offset) * p0.w, p0.z, p0.w);
    EmitVertex();

    fragColor = gs_in[1].color;
    gl_Position = vec4((ndc1 + offset) * p1.w, p1.z, p1.w);
    EmitVertex();
    gl_Position = vec4((ndc1 - offset) * p1.w, p1.z, p1.w);
    EmitVertex();

    EndPrimitive();
}
