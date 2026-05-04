#version 410 core

in vec3 fragColor;
in vec3 fragNormal;
out vec4 outColor;

uniform vec3 uLightDir;
uniform float uBrightenAmount;
uniform float uLightingEnabled;
uniform float uAlpha;

void main()
{
    if (uLightingEnabled < 0.5 || length(fragNormal) < 1e-5)
    {
        outColor = vec4(fragColor, uAlpha);
        return;
    }

    vec3 N = normalize(fragNormal);
    vec3 L = normalize(uLightDir);

    float diff = max(0.0, dot(N, L));
    float lighting = 1.0 + uBrightenAmount * diff;

    outColor = vec4(min(fragColor * lighting, vec3(1.0)), uAlpha);
}
