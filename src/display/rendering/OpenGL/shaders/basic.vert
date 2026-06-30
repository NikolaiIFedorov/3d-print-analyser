#version 410 core

layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aColor;
layout(location = 2) in vec3 aNormal;
layout(location = 3) in float aFaceSize;

uniform mat4 uViewProjection;
uniform mat4 uModel;
uniform float uLightingEnabled;
uniform float uClipZBiasW;
uniform float uPriorityBiasScale;
uniform vec3 uViewPos;

out vec3 fragColor;
out vec3 fragNormal;
out float fragViewDist;

void main()
{
    vec4 worldPos = uModel * vec4(aPosition, 1.0);
    fragViewDist = length(worldPos.xyz - uViewPos);

    vec4 pos = uViewProjection * worldPos;
    // Smaller faceSize (smaller on-screen footprint) gets pulled closer to the camera so it
    // wins z-fighting against the larger coplanar face it sits on. Clamp to avoid blow-up on
    // degenerate/zero-area slivers.
    float priorityBias = uPriorityBiasScale * pos.w / max(aFaceSize, 1e-3);
    float biasedZ = pos.z + uClipZBiasW * pos.w + priorityBias;
    // Keep depth layering bias from pushing primitives beyond clip planes when zoomed in.
    float clipMargin = max(1e-6, 1e-6 * abs(pos.w));
    pos.z = clamp(biasedZ, -pos.w + clipMargin, pos.w - clipMargin);
    gl_Position = pos;
    fragColor = aColor;
    // Guard: skip normalize when lighting is off — aNormal may be zero
    // (e.g. grid/axis VAOs that don't supply a normal attribute).
    if (uLightingEnabled > 0.5)
        fragNormal = normalize(mat3(transpose(inverse(uModel))) * aNormal);
    else
        fragNormal = vec3(0.0, 0.0, 1.0);
}