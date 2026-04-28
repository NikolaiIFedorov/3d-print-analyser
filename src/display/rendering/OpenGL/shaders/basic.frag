#version 410 core

in vec3 fragColor;
in vec3 fragNormal;

out vec4 outColor;

uniform vec3 uLightDir;
uniform float uBrightenAmount;
uniform float uLightingEnabled;
uniform vec3 uViewDirWorld;
uniform float uGridPlaneFade;

void main()
{
    if (uLightingEnabled < 0.5)
    {
        vec3 c = fragColor;
        float k = 1.0;
        if (uGridPlaneFade > 0.5)
        {
            vec3 n = vec3(0.0, 0.0, 1.0);
            float g = abs(dot(normalize(uViewDirWorld), n));
            k = smoothstep(0.06, 0.32, g);
        }
        outColor = vec4(c * k, 1.0);
        return;
    }

    vec3 N = normalize(fragNormal);
    vec3 L = normalize(uLightDir);

    // One-sided diffuse: faces pointing away from the light keep their base
    // color unchanged; faces pointing toward it brighten by up to uBrightenAmount.
    float diff = max(0.0, dot(N, L));
    float lighting = 1.0 + uBrightenAmount * diff;

    outColor = vec4(min(fragColor * lighting, vec3(1.0)), 1.0);
}