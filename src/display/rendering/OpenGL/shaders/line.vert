#version 410 core

layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aColor;
layout(location = 2) in vec3 aNormal;

uniform mat4 uViewProjection;
uniform mat4 uModel;

out VS_OUT {
    vec3 color;
} vs_out;

void main()
{
    gl_Position = uViewProjection * uModel * vec4(aPosition, 1.0);
    // Shift lines slightly toward the camera in NDC space so they always
    // sit in front of patches. Bias must be large enough to beat depth
    // precision (~12 µm per step with 24-bit/200 000-unit range) yet small
    // enough not to pull back-face edges through thin-walled geometry.
    // 1e-6 NDC ≈ 0.1 world units = ~8 depth steps.  Features thicker than
    // 0.1 mm will never have back edges bleed through.
    gl_Position.z -= 0.000001 * gl_Position.w;
    vs_out.color = aColor;
}
