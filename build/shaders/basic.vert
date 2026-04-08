#version 410 core

layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aColor;
layout(location = 2) in vec3 aNormal;

out vec3 fragColor;
out vec3 fragNormal;
out vec3 fragWorldPos;

uniform mat4 uViewProjection;
uniform mat4 uModel;

void main()
{
    vec4 worldPos = uModel * vec4(aPosition, 1.0);
    gl_Position = uViewProjection * worldPos;
    fragWorldPos = worldPos.xyz;
    fragColor = aColor;
    fragNormal = mat3(uModel) * aNormal;
}