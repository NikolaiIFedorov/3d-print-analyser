#version 410 core

in vec3 fragColor;
in vec3 fragNormal;
in float fragViewDist;

out vec4 outColor;

uniform vec3 uLightDir;
uniform float uBrightenAmount;
uniform float uLightingEnabled;
uniform float uGridPlaneFade;
uniform float uGridOpacity;
uniform float uAlpha;
// Structure tool: translucent shell pass only — back faces fully opaque, front faces use uAlpha.
uniform float uStructureShellBackFaceOpaque;
// Depth cueing: tint distant faces toward the background color. Range is derived from the
// camera's live ortho zoom (see RenderingExperiments::kDepthCue*), not a fixed world distance.
uniform vec3 uFogColor;
uniform float uFogNear;
uniform float uFogFar;
uniform float uFogStrength;

vec3 ApplyDepthCue(vec3 c)
{
    float t = clamp((fragViewDist - uFogNear) / max(uFogFar - uFogNear, 1e-6), 0.0, 1.0);
    return mix(c, uFogColor, t * uFogStrength);
}

void main()
{
    if (uLightingEnabled < 0.5)
    {
        vec3 c = fragColor;
        if (uGridPlaneFade > 0.5)
        {
            // RGB from vertex; alpha from tilt vs XY plane (see ViewportRenderer).
            outColor = vec4(c, uGridOpacity);
            return;
        }
        float a = uAlpha;
        if (uStructureShellBackFaceOpaque > 0.5)
            a = gl_FrontFacing ? uAlpha : 1.0;
        outColor = vec4(ApplyDepthCue(c), a);
        return;
    }

    vec3 N = normalize(fragNormal);
    vec3 L = normalize(uLightDir);

    // One-sided diffuse: faces pointing away from the light keep their base
    // color unchanged; faces pointing toward it brighten by up to uBrightenAmount.
    float diff = max(0.0, dot(N, L));
    float lighting = 1.0 + uBrightenAmount * diff;

    float a = uAlpha;
    if (uStructureShellBackFaceOpaque > 0.5)
        a = gl_FrontFacing ? uAlpha : 1.0;
    vec3 litColor = min(fragColor * lighting, vec3(1.0));
    outColor = vec4(ApplyDepthCue(litColor), a);
}