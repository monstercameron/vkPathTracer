#pragma once

namespace {

const char* kShaderSource = R"HLSL(
cbuffer FrameConstants : register(b0)
{
    float2 resolution;
    float timeSec;
    float frameIndex;
};

struct VsOut
{
    float4 pos : SV_Position;
};

VsOut VSMain(uint vertexId : SV_VertexID)
{
    float2 p;
    if (vertexId == 0)      p = float2(-1.0, -1.0);
    else if (vertexId == 1) p = float2(-1.0,  3.0);
    else                   p = float2( 3.0, -1.0);

    VsOut o;
    o.pos = float4(p, 0.0, 1.0);
    return o;
}

float Random(inout uint state)
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return float(state & 16777215u) / 16777216.0;
}

float3 CosineSampleHemisphere(inout uint state)
{
    float r1 = Random(state);
    float r2 = Random(state);
    float phi = 6.28318530718 * r2;
    float r = sqrt(r1);
    float3 s;
    s.x = r * cos(phi);
    s.y = r * sin(phi);
    s.z = sqrt(max(0.0, 1.0 - r1));
    return s;
}

// matType: 0 = diffuse, 1 = glossy, 2 = mirror
struct HitInfo
{
    float t;
    float3 p;
    float3 n;
    float3 albedo;
    float3 emission;
    float  matType;
    float  roughness;
};

bool PlaneHitX(float3 ro, float3 rd, float x, float3 normal, float yMin, float yMax, float zMin, float zMax, bool cullBackfaces, out float t, out float3 p, out float3 n)
{
    if (abs(rd.x) < 1e-6)
        return false;
    t = (x - ro.x) / rd.x;
    if (t < 0.001)
        return false;
    p = ro + rd * t;
    if (p.y < yMin || p.y > yMax || p.z < zMin || p.z > zMax)
        return false;
    n = normal;
    if (cullBackfaces && dot(n, rd) > 0.0)
        return false;
    return true;
}

bool PlaneHitY(float3 ro, float3 rd, float y, float3 normal, float xMin, float xMax, float zMin, float zMax, bool cullBackfaces, out float t, out float3 p, out float3 n)
{
    if (abs(rd.y) < 1e-6)
        return false;
    t = (y - ro.y) / rd.y;
    if (t < 0.001)
        return false;
    p = ro + rd * t;
    if (p.x < xMin || p.x > xMax || p.z < zMin || p.z > zMax)
        return false;
    n = normal;
    if (cullBackfaces && dot(n, rd) > 0.0)
        return false;
    return true;
}

bool PlaneHitZ(float3 ro, float3 rd, float z, float3 normal, float xMin, float xMax, float yMin, float yMax, bool cullBackfaces, out float t, out float3 p, out float3 n)
{
    if (abs(rd.z) < 1e-6)
        return false;
    t = (z - ro.z) / rd.z;
    if (t < 0.001)
        return false;
    p = ro + rd * t;
    if (p.x < xMin || p.x > xMax || p.y < yMin || p.y > yMax)
        return false;
    n = normal;
    if (cullBackfaces && dot(n, rd) > 0.0)
        return false;
    return true;
}

bool AabbHit(
    float3 ro,
    float3 rd,
    float3 bmin,
    float3 bmax,
    bool cullBackfaces,
    out float t,
    out float3 p,
    out float3 n)
{
    float3 inv = 1.0 / rd;
    float3 t0 = (bmin - ro) * inv;
    float3 t1 = (bmax - ro) * inv;
    float3 tsmall = min(t0, t1);
    float3 tbig = max(t0, t1);
    float3 tMin3 = tsmall;
    float3 tMax3 = tbig;

    float enter = max(max(tMin3.x, tMin3.y), tMin3.z);
    float exit = min(min(tMax3.x, tMax3.y), tMax3.z);
    if (exit < 0.001 || enter > exit)
        return false;

    t = (enter > 0.001) ? enter : exit;
    if (t < 0.001)
        return false;

    p = ro + rd * t;
    if (abs(p.x - bmin.x) < 1e-3)
        n = float3(-1.0, 0.0, 0.0);
    else if (abs(p.x - bmax.x) < 1e-3)
        n = float3(1.0, 0.0, 0.0);
    else if (abs(p.y - bmin.y) < 1e-3)
        n = float3(0.0, -1.0, 0.0);
    else if (abs(p.y - bmax.y) < 1e-3)
        n = float3(0.0, 1.0, 0.0);
    else if (abs(p.z - bmin.z) < 1e-3)
        n = float3(0.0, 0.0, -1.0);
    else
        n = float3(0.0, 0.0, 1.0);

    if (cullBackfaces && dot(n, rd) > 0.0)
        return false;
    return true;
}

bool SceneHit(float3 ro, float3 rd, out HitInfo hit)
{
    bool any = false;
    hit.t = 1e30;
    hit.albedo = float3(0.0, 0.0, 0.0);
    hit.emission = float3(0.0, 0.0, 0.0);
    hit.n = float3(0.0, 0.0, 0.0);
    hit.p = float3(0.0, 0.0, 0.0);
    hit.matType = 0.0;
    hit.roughness = 1.0;

    float t;
    float3 p;
    float3 n;
    const float wallMin = -1.0;
    const float wallMax = 1.0;

    const float3 wallRed = float3(0.78, 0.17, 0.15);
    const float3 wallBlue = float3(0.17, 0.30, 0.74);
    const float3 white = float3(0.75, 0.75, 0.75);

    if (PlaneHitY(ro, rd, 0.0, float3(0.0, 1.0, 0.0), wallMin, wallMax, wallMin, wallMax, true, t, p, n))
    {
        if (t < hit.t)
        {
            hit.t = t;
            hit.p = p;
            hit.n = n;
            hit.albedo = white;
            hit.emission = 0.0;
            any = true;
        }
    }

    if (PlaneHitY(ro, rd, 1.79, float3(0.0, -1.0, 0.0), wallMin, wallMax, wallMin, wallMax, true, t, p, n))
    {
        if (t < hit.t)
        {
            hit.t = t;
            hit.p = p;
            hit.n = n;
            hit.albedo = white;
            hit.emission = 0.0;
            any = true;
        }
    }

    if (PlaneHitX(ro, rd, -1.0, float3(1.0, 0.0, 0.0), 0.0, 1.79, wallMin, wallMax, true, t, p, n))
    {
        if (t < hit.t)
        {
            hit.t = t;
            hit.p = p;
            hit.n = n;
            hit.albedo = wallRed;
            hit.emission = 0.0;
            any = true;
        }
    }

    if (PlaneHitX(ro, rd, 1.0, float3(-1.0, 0.0, 0.0), 0.0, 1.79, wallMin, wallMax, true, t, p, n))
    {
        if (t < hit.t)
        {
            hit.t = t;
            hit.p = p;
            hit.n = n;
            hit.albedo = wallBlue;
            hit.emission = 0.0;
            any = true;
        }
    }

    if (PlaneHitZ(ro, rd, -1.0, float3(0.0, 0.0, 1.0), wallMin, wallMax, 0.0, 1.79, true, t, p, n))
    {
        if (t < hit.t)
        {
            hit.t = t;
            hit.p = p;
            hit.n = n;
            hit.albedo = white;
            hit.emission = 0.0;
            any = true;
        }
    }

    // Tall box — mirror
    if (AabbHit(ro, rd, float3(-0.40, 0.0, -0.20), float3(-0.05, 0.55, 0.35), true, t, p, n))
    {
        if (t < hit.t)
        {
            hit.t = t;
            hit.p = p;
            hit.n = n;
            hit.albedo = float3(0.95, 0.95, 0.95);
            hit.emission = 0.0;
            hit.matType = 2.0;
            hit.roughness = 0.0;
            any = true;
        }
    }

    // Short box — glossy
    if (AabbHit(ro, rd, float3(0.10, 0.0, 0.05), float3(0.60, 1.05, 0.50), true, t, p, n))
    {
        if (t < hit.t)
        {
            hit.t = t;
            hit.p = p;
            hit.n = n;
            hit.albedo = float3(0.95, 0.85, 0.60);
            hit.emission = 0.0;
            hit.matType = 1.0;
            hit.roughness = 0.08;
            any = true;
        }
    }

    if (AabbHit(ro, rd, float3(-0.30, 1.74, -0.30), float3(0.30, 1.80, 0.30), true, t, p, n))
    {
        if (t < hit.t)
        {
            hit.t = t;
            hit.p = p;
            hit.n = n;
            hit.albedo = 0.0;
            hit.emission = float3(50.0, 45.0, 40.0);
            any = true;
        }
    }

    return any;
}

float3 Background(float3 rd)
{
    return 0.0;
}

float4 PSMain(VsOut input) : SV_Target
{
    uint pixelX = uint(max(input.pos.x, 0.0));
    uint pixelY = uint(max(input.pos.y, 0.0));
    uint seed = pixelX * 1973u + pixelY * 9277u + uint(frameIndex) * 26699u;
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;

    // Camera (shared across all samples)
    float angle = timeSec * 0.15;
    float3 camPos    = float3(sin(angle) * 0.35, 0.89, 2.2);
    float3 camTarget = float3(0.0, 0.89, 0.0);
    float3 forward   = normalize(camTarget - camPos);
    float3 right     = normalize(cross(float3(0.0, 1.0, 0.0), forward));
    float3 up        = normalize(cross(forward, right));

    float3 color = 0.0;

    // Light geometry (must match SceneHit)
    const float3 lightEmission  = float3(50.0, 45.0, 40.0);
    const float  lightXMin = -0.30, lightXMax = 0.30;
    const float  lightZMin = -0.30, lightZMax = 0.30;
    const float  lightYSample   = 1.77;
    const float  lightArea      = (lightXMax - lightXMin) * (lightZMax - lightZMin);
    const float3 lightNormal    = float3(0.0, -1.0, 0.0);

    uint sampleSeed = seed;
    float2 jitter = float2(Random(sampleSeed), Random(sampleSeed));
    float2 uv = ((input.pos.xy + jitter - 0.5) / resolution) * 2.0 - 1.0;
    uv.y *= -1.0;
    uv.x *= resolution.x / max(resolution.y, 1.0);

    float3 ro = camPos;
    float3 rd = normalize(forward * 1.5 + right * uv.x + up * uv.y);
    float3 throughput = 1.0;

    [loop]
    for (int bounce = 0; bounce < 8; ++bounce)
    {
        HitInfo hit;
        if (!SceneHit(ro, rd, hit))
            break;

        // Direct view of the light (primary ray only to avoid double-count)
        if (hit.emission.x > 0.0 || hit.emission.y > 0.0 || hit.emission.z > 0.0)
        {
            if (bounce == 0)
                color += throughput * hit.emission;
            break;
        }

        if (hit.matType < 0.5)
        {
            // --- Diffuse: NEE + cosine sample ---
            float r1 = Random(sampleSeed);
            float r2 = Random(sampleSeed);
            float3 lightPos = float3(lerp(lightXMin, lightXMax, r1),
                                     lightYSample,
                                     lerp(lightZMin, lightZMax, r2));
            float3 toLight  = lightPos - hit.p;
            float  distSq   = dot(toLight, toLight);
            float  dist     = sqrt(distSq);
            float3 lightDir = toLight / dist;

            float cosTheta  = max(dot(hit.n, lightDir), 0.0);
            float cosThetaL = max(dot(lightNormal, -lightDir), 0.0);

            if (cosTheta > 0.0 && cosThetaL > 0.0)
            {
                HitInfo shadowHit;
                float3  shadowOrig = hit.p + hit.n * 0.003;
                bool    shadowed   = false;
                if (SceneHit(shadowOrig, lightDir, shadowHit))
                {
                    bool hitLight = shadowHit.emission.x > 0.0 || shadowHit.emission.y > 0.0 || shadowHit.emission.z > 0.0;
                    shadowed = !hitLight && (shadowHit.t < dist - 0.01);
                }
                if (!shadowed)
                {
                    float  G    = cosTheta * cosThetaL / distSq;
                    float3 brdf = hit.albedo / 3.14159265;
                    color += throughput * lightEmission * brdf * G * lightArea;
                }
            }

            float3 local     = CosineSampleHemisphere(sampleSeed);
            float3 tangent   = normalize(cross(abs(hit.n.y) < 0.999 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0), hit.n));
            float3 bitangent = cross(hit.n, tangent);
            rd = normalize(local.x * tangent + local.y * bitangent + local.z * hit.n);
            ro = hit.p + hit.n * 0.002;
            throughput *= hit.albedo;
        }
        else if (hit.matType < 1.5)
        {
            // --- Glossy: perturbed specular reflection ---
            float3 refl      = reflect(rd, hit.n);
            float3 local     = CosineSampleHemisphere(sampleSeed);
            float3 tangent   = normalize(cross(abs(refl.y) < 0.999 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0), refl));
            float3 bitangent = cross(refl, tangent);
            float3 perturbed = normalize(refl + hit.roughness * (local.x * tangent + local.y * bitangent));
            // Ensure perturbed stays above surface
            if (dot(perturbed, hit.n) < 0.001)
                perturbed = refl;
            rd = perturbed;
            ro = hit.p + hit.n * 0.002;
            throughput *= hit.albedo;
        }
        else
        {
            // --- Mirror: perfect specular reflection ---
            rd = reflect(rd, hit.n);
            ro = hit.p + hit.n * 0.002;
            throughput *= hit.albedo;
        }

        if (dot(throughput, throughput) < 0.0001)
            break;
    } // bounce

    color = color * 2.0;
    color = color / (color + 1.0);
    color = pow(max(color, 0.0), 1.0 / 2.2);
    return float4(color, 1.0);
}
)HLSL";

}  // namespace
