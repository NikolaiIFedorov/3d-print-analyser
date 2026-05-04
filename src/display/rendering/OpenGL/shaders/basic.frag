#version 410 core

in vec3 fragColor;
in vec3 fragNormal;

out vec4 outColor;

uniform vec3 uLightDir;
uniform float uBrightenAmount;
uniform float uLightingEnabled;
uniform float uGridPlaneFade;
uniform float uGridOpacity;
uniform float uAlpha;

void main()
{
    if (uLightingEnabled < 0.5)
    {
        vec3 c = fragColor;
        if (uGridPlaneFade > 0.5)
        {
            // RGB from vertex; alpha from view-dependent density (see ViewportRenderer).
            outColor = vec4(c, uGridOpacity);
            return;
        }
        outColor = vec4(c, uAlpha);
        return;
    }

    vec3 N = normalize(fragNormal);
    vec3 L = normalize(uLightDir);

    // One-sided diffuse: faces pointing away from the light keep their base
    // color unchanged; faces pointing toward it brighten by up to uBrightenAmount.
    float diff = max(0.0, dot(N, L));
    float lighting = 1.0 + uBrightenAmount * diff;

    outColor = vec4(min(fragColor * lighting, vec3(1.0)), uAlpha);
}