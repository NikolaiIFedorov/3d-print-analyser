#version 410 core

layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aColor;
layout(location = 2) in vec3 aNormal;

uniform mat4 uViewProjection;
uniform mat4 uModel;
uniform float uLightingEnabled;
uniform float uClipZBiasW;

out vec3 fragColor;
out vec3 fragNormal;

void main()
{
    vec4 pos = uViewProjection * uModel * vec4(aPosition, 1.0);
    pos.z += uClipZBiasW * pos.w;
    gl_Position = pos;
    fragColor = aColor;
    // Guard: skip normalize when lighting is off — aNormal may be zero
    // (e.g. grid/axis VAOs that don't supply a normal attribute).
    if (uLightingEnabled > 0.5)
        fragNormal = normalize(mat3(transpose(inverse(uModel))) * aNormal);
    else
        fragNormal = vec3(0.0, 0.0, 1.0);
}