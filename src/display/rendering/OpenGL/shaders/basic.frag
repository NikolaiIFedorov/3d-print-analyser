#version 410 core

in vec3 fragColor;
in vec3 fragNormal;

out vec4 outColor;

uniform vec3 uLightDir;
uniform float uBrightenAmount;
uniform float uLightingEnabled;
uniform vec3 uViewDirWorld;
uniform float uGridPlaneFade;
uniform float uPrincipalSnap;
uniform float uGridLodStep;

void main()
{
    if (uLightingEnabled < 0.5)
    {
        vec3 c = fragColor;
        if (uGridPlaneFade > 0.5)
        {
            vec3 n = vec3(0.0, 0.0, 1.0);
            float g = abs(dot(normalize(uViewDirWorld), n));
            // 0 = grazing the XY plane, 1 = looking along +Z / −Z (perpendicular to grid).
            float t = smoothstep(0.05, 0.36, g);
            // Overall light grid + grazing response; uPrincipalSnap lifts floor on canonical views.
            float aLo = mix(0.10, 0.28, uPrincipalSnap);
            float aHi = mix(0.38, 0.55, uPrincipalSnap);
            float a = mix(aLo, aHi, t);
            // When LOD spacing > 1 world unit, lines are sparser — bump alpha slightly so the grid stays readable.
            if (uGridLodStep > 1.01)
                a = min(0.93, a + 0.12 * clamp(log2(uGridLodStep) / 4.0, 0.0, 1.0));
            outColor = vec4(c, a);
            return;
        }
        outColor = vec4(c, 1.0);
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