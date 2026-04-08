#version 410 core

in vec3 fragColor;
in vec3 fragNormal;
in vec3 fragWorldPos;

out vec4 outColor;

uniform vec3 uLightDir;
uniform vec3 uViewPos;
uniform float uBrightenAmount;
uniform float uBlueMin;
uniform float uBlueMax;
uniform float uBlueNear;
uniform float uBlueFar;
uniform float uLightingEnabled;

void main()
{
    if (uLightingEnabled < 0.5 || length(fragNormal) < 0.001)
    {
        outColor = vec4(fragColor, 1.0);
        return;
    }

    vec3 normal = normalize(fragNormal);
    vec3 lightDir = normalize(uLightDir);

    // How much the surface faces the light (one-sided)
    float facing = max(dot(normal, lightDir), 0.0);

    // Base color + brighten by up to uBrightenAmount when lit
    vec3 color = fragColor * (1.0 + uBrightenAmount * facing);

    // Blue tint based on distance from camera (farther = more blue)
    float dist = length(uViewPos - fragWorldPos);
    float blueFactor = smoothstep(uBlueNear, uBlueFar, dist);
    color.b += mix(uBlueMin, uBlueMax, blueFactor);

    outColor = vec4(color, 1.0);
}