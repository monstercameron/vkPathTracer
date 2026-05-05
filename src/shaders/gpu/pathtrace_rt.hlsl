// pathtrace_rt.hlsl — DXR path tracing library shader (target: lib_6_3)
// Iterative path tracer: raygen loops over depth, no shader recursion needed.
// Resource layout mirrors pathtrace_cs.hlsl so the same CPU constant buffer works.

// ---- Constants (matches PathTraceConstants in D3D12GpuPathTracer.h) --------
cbuffer PCBuf : register(b0) {
    float  camera_pos_x;  float camera_pos_y;  float camera_pos_z;  float fov_tan_half;
    float  cam_fwd_x;     float cam_fwd_y;     float cam_fwd_z;     float aspect;
    float  cam_right_x;   float cam_right_y;   float cam_right_z;   uint  num_sdfs;
    float  cam_up_x;      float cam_up_y;      float cam_up_z;      uint  sample_index;
    uint   num_insts;     uint  num_mats;       uint  num_lights;    uint  width;
    uint   height;        uint  base_seed;      float env_r;         float env_g;
    float  env_b;         float max_depth_f;    uint  rays_per_pixel; float exposure;
    float  aperture_radius; float focus_distance; uint iris_blade_count; float iris_rotation_radians;
    float  iris_roundness; float anamorphic_squeeze; uint tone_map; uint output_transform;
    float  gamma; uint clamp_output; float white_balance_r; float white_balance_g;
    float  white_balance_b; float _pad0; float _pad1; float _pad2;
};

// ---- Resources -------------------------------------------------------------
RaytracingAccelerationStructure SceneTLAS : register(t0, space0);
Buffer<float>    VertBuf   : register(t1, space0);
Buffer<uint>     IndexBuf  : register(t2, space0);
Buffer<float>    MatBuf    : register(t3, space0);
Buffer<uint>     InstBuf   : register(t4, space0);   // kept for binding compat
Buffer<float>    LightBuf  : register(t5, space0);   // reserved for NEE
Buffer<uint>     TriMatBuf : register(t6, space0);
Buffer<float>    TriDataBuf : register(t7, space0);
RWByteAddressBuffer FilmBuf : register(u0, space0);

// ---- Payload (64 bytes — must match MaxPayloadSizeInBytes in PSO) -----------
struct PathPayload {
    float3 radiance;     // 12
    uint   done;         //  4  → 16
    float3 throughput;   // 12
    uint   depth;        //  4  → 32
    float3 next_origin;  // 12
    uint   rng;          //  4  → 48
    float3 next_dir;     // 12
    float  _pad;         //  4  → 64
};

static const uint kMatStride = 16u;
static const uint kInstStride = 24u;
static const uint kPackedTriStride = 12u;
static const uint kStaticInstanceId = 0x00FFFFFFu;
float3 MatAlbedo(uint idx)   { uint b=idx*kMatStride; return float3(MatBuf[b], MatBuf[b+1u], MatBuf[b+2u]); }
float3 MatEmissive(uint idx) { uint b=idx*kMatStride; return float3(MatBuf[b+3u], MatBuf[b+4u], MatBuf[b+5u]); }
float  MatRoughness(uint idx){ return MatBuf[idx*kMatStride + 6u]; }
uint   MatModel(uint idx)    { return uint(MatBuf[idx*kMatStride + 7u] + 0.5); }
float  MatMetallic(uint idx) { return MatBuf[idx*kMatStride + 8u]; }
float  MatIor(uint idx)      { return max(1.01, MatBuf[idx*kMatStride + 9u]); }
float  MatTransmission(uint idx){ return MatBuf[idx*kMatStride + 10u]; }
float  MatClearcoat(uint idx){ return MatBuf[idx*kMatStride + 11u]; }
float  MatSheen(uint idx)    { return MatBuf[idx*kMatStride + 12u]; }
float  MatAlpha(uint idx)    { return MatBuf[idx*kMatStride + 14u]; }
uint   MatEffectRaw(uint idx){ return uint(MatBuf[idx*kMatStride + 15u] + 0.5); }
uint   MatEffect(uint idx)   { return MatEffectRaw(idx) & 1023u; }
bool   MatDoubleSided(uint idx) { return (MatEffectRaw(idx) & 1024u) != 0u; }

float Pow2Fast(float x) {
    return x * x;
}

float Pow5Fast(float x) {
    float x2 = x * x;
    return x2 * x2 * x;
}

float Pow6Fast(float x) {
    float x2 = x * x;
    return x2 * x2 * x2;
}

float Hash01(float3 p, float seed) {
    return frac(sin(dot(p, float3(12.9898, 78.233, 37.719)) + seed * 19.19) * 43758.5453);
}

float3 MatSurfaceAlbedo(uint idx, float3 p, float3 n, float3 rd) {
    float3 color = MatAlbedo(idx);
    uint effect = MatEffect(idx);
    float h = Hash01(p, float(effect));
    if (effect == 1u) {
        float rim = Pow2Fast(saturate(1.0 - abs(dot(n, -rd))));
        color = color * (0.65 + 0.25 * h) + float3(0.25, 0.22, 0.28) * rim * (0.4 + MatSheen(idx));
    } else if (effect == 2u) {
        color *= 0.45 + 0.55 * (0.5 + 0.5 * sin(p.x * 7.0 + p.z * 5.0 + h * 6.0));
    } else if (effect == 3u) {
        color *= (h > 0.72) ? 0.18 : 1.0;
    } else if (effect == 4u) {
        float vein = 0.5 + 0.5 * sin((p.x + p.y * 0.4 + p.z * 0.7) * 9.0 + h * 3.0);
        color = color * (0.55 + 0.45 * vein) + float3(0.18, 0.20, 0.23) * (1.0 - vein);
    } else if (effect == 5u) {
        color = color * (0.55 + 0.35 * h) + float3(0.45, 0.16, 0.04) * (0.25 + 0.25 * h);
    } else if (effect == 6u) {
        color *= 0.65 + 0.35 * h;
    } else if (effect == 7u) {
        color = color * 0.78 + float3(1.0, 0.62, 0.42) * 0.16;
    } else if (effect == 8u) {
        color = color * 0.55 + float3(0.35 + 0.45 * h, 0.25 + 0.35 * (1.0 - h), 0.85) * 0.45;
    } else if (effect == 9u) {
        float check = fmod(floor(p.x * 4.0) + floor(p.z * 4.0), 2.0);
        color *= (check < 0.5) ? 1.0 : 0.28;
    } else if (effect == 10u) {
        color = color * 0.45 + float3(1.0, 0.35 + 0.3 * h, 0.08) * 0.55;
    } else if (effect == 11u) {
        color *= 0.55 + 0.45 * (0.5 + 0.5 * sin(p.y * 18.0 + h * 5.0));
    } else if (effect == 12u) {
        color += float3(0.55, 0.65, 0.95) * Pow6Fast(saturate(dot(n, -rd)));
    } else if (effect == 13u) {
        color *= 0.65 + 0.35 * abs(sin(p.x * 10.0) * cos(p.z * 10.0));
    } else if (effect == 14u) {
        float rim = pow(saturate(1.0 - abs(dot(n, -rd))), 0.8);
        color = float3(0.15, 0.75, 1.0) * (0.2 + 0.8 * rim);
    } else if (effect == 15u) {
        float bands = 0.5 + 0.5 * sin(p.y * 11.0 + p.x * 3.0 + h * 7.0);
        float rim = pow(saturate(1.0 - abs(dot(n, -rd))), 1.5);
        color = color * (0.25 + 0.45 * bands) + float3(0.48, 0.56, 0.68) * (0.18 + 0.32 * rim);
    }
    return clamp(color, 0.0, 1.5);
}

float3 PreviewReflectionEnvironment(float3 rd) {
    float t = saturate(rd.y * 0.5 + 0.5);
    float horizon = Pow2Fast(saturate(1.0 - abs(rd.y)));
    float3 floorColor = float3(0.08, 0.075, 0.065);
    float3 skyColor = float3(0.34, 0.42, 0.58);
    float3 color = lerp(floorColor, skyColor, t);
    color += float3(0.55, 0.48, 0.36) * horizon * 0.18;
    return color;
}

// ---- Forward declarations --------------------------------------------------
uint  Pcg(uint v);
float RandF(inout uint rng);
float Halton2(uint idx);
float Halton3(uint idx);
void  ApplyCameraLens(inout float3 origin, inout float3 dir, inout uint rng);

float4 LoadFilm(uint pixel) {
    return asfloat(FilmBuf.Load4(pixel * 16u));
}

void StoreFilm(uint pixel, float4 value) {
    FilmBuf.Store4(pixel * 16u, asuint(value));
}

// ---- Raygen ----------------------------------------------------------------
[shader("raygeneration")]
void RayGen() {
    uint2 pix = DispatchRaysIndex().xy;
    if (pix.x >= width || pix.y >= height) return;
    uint pixel = pix.y * width + pix.x;

    float3 cam_pos   = float3(camera_pos_x, camera_pos_y, camera_pos_z);
    float3 cam_fwd   = float3(cam_fwd_x,    cam_fwd_y,    cam_fwd_z);
    float3 cam_right = float3(cam_right_x,  cam_right_y,  cam_right_z);
    float3 cam_up    = float3(cam_up_x,     cam_up_y,     cam_up_z);

    uint   rpp   = max(1u, rays_per_pixel);
    float3 total = (float3)0;

    for (uint ri = 0u; ri < rpp; ++ri) {
        uint eff = sample_index * rpp + ri;
        uint rng = Pcg(pixel ^ Pcg(eff * 1664525u ^ base_seed) ^ (ri * 2654435761u));

        float jx = Halton2(eff);
        float jy = Halton3(eff);
        float fx = ((float)pix.x + jx) / (float)width;
        float fy = ((float)pix.y + jy) / (float)height;
        float nx = (2.0f * fx - 1.0f) * aspect * fov_tan_half;
        float ny = (1.0f - 2.0f * fy) * fov_tan_half;
        float3 dir = normalize(cam_fwd + cam_right * nx + cam_up * ny);
        float3 origin = cam_pos;
        ApplyCameraLens(origin, dir, rng);

        PathPayload payload;
        payload.radiance    = (float3)0;
        payload.throughput  = (float3)1;
        payload.done        = 0u;
        payload.depth       = 0u;
        payload.rng         = rng;
        payload.next_origin = origin;
        payload.next_dir    = dir;
        payload._pad        = 0.0f;

        uint maxD = (uint)max(1.0f, max_depth_f);
        for (uint d = 0u; d < maxD && payload.done == 0u; ++d) {
            RayDesc ray;
            ray.Origin    = payload.next_origin;
            ray.Direction = payload.next_dir;
            ray.TMin      = 1e-4f;
            ray.TMax      = 1e30f;
            TraceRay(SceneTLAS,
                     RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_CULL_BACK_FACING_TRIANGLES,
                     /*InstanceMask*/ 0xFF,
                     /*HitGroup offset*/ 0,
                     /*Geometry multiplier*/ 1,
                     /*MissShader index*/ 0,
                     ray, payload);
        }
        total += payload.radiance;
    }

    float4 prev = LoadFilm(pixel);
    StoreFilm(pixel, float4(prev.xyz + total, prev.w + (float)rpp));
}

// ---- Miss ------------------------------------------------------------------
[shader("miss")]
void Miss(inout PathPayload payload) {
    float3 env = float3(env_r, env_g, env_b);
    float envLuma = max(env.x, max(env.y, env.z));
    float3 missEnv = (payload._pad > 0.5 && envLuma <= 1.0e-5)
        ? PreviewReflectionEnvironment(WorldRayDirection())
        : env;
    payload.radiance += payload.throughput * missEnv;
    payload.done = 1u;
}

// ---- ClosestHit ------------------------------------------------------------
[shader("closesthit")]
void ClosestHit(inout PathPayload payload, in BuiltInTriangleIntersectionAttributes attr) {
    uint instanceId = InstanceID();
    uint triIdx = PrimitiveIndex();
    if (instanceId != kStaticInstanceId) {
        uint ib = instanceId * kInstStride;
        triIdx += InstBuf[ib + 0u];
    }
    uint tb = triIdx * kPackedTriStride;
    uint matIdx = uint(TriDataBuf[tb + 3u] + 0.5f);
    float3 e1 = float3(TriDataBuf[tb + 4u], TriDataBuf[tb + 5u], TriDataBuf[tb + 6u]);
    float3 e2 = float3(TriDataBuf[tb + 8u], TriDataBuf[tb + 9u], TriDataBuf[tb + 10u]);
    float3x4 o2w = ObjectToWorld3x4();
    float3 we1 = float3(dot(o2w[0].xyz, e1), dot(o2w[1].xyz, e1), dot(o2w[2].xyz, e1));
    float3 we2 = float3(dot(o2w[0].xyz, e2), dot(o2w[1].xyz, e2), dot(o2w[2].xyz, e2));
    float3 n = normalize(cross(we1, we2));
    if (dot(n, WorldRayDirection()) > 0.0f) n = -n;
    float3 hitPos = WorldRayOrigin() + WorldRayDirection() * RayTCurrent();
    float3 albedo   = MatSurfaceAlbedo(matIdx, hitPos, n, WorldRayDirection());
    float3 emissive = MatEmissive(matIdx);
    payload.radiance  += payload.throughput * emissive;
    float roughness = MatRoughness(matIdx);
    uint model = MatModel(matIdx);
    bool isMirror = (model == 2u) || (roughness <= 0.001);
    bool isMetallic = (model == 4u) || (MatMetallic(matIdx) > 0.65);
    bool isTransmissive = (model == 5u) || (MatTransmission(matIdx) > 0.05);
    bool isDiffuse = (roughness >= 0.999) && !isMirror && !isMetallic && !isTransmissive;
    bool isSheen = (model == 6u);
    bool isClearcoat = (model == 7u) || (MatClearcoat(matIdx) > 0.05);
    bool isToon = (model == 8u);
    uint rng = payload.rng;
    if (num_lights > 0u) {
        uint li = uint(RandF(rng) * float(num_lights));
        li = min(li, num_lights - 1u);
        uint lb = li * 8u;
        float3 lpos = float3(LightBuf[lb], LightBuf[lb + 1u], LightBuf[lb + 2u]);
        float3 lcol = float3(LightBuf[lb + 3u], LightBuf[lb + 4u], LightBuf[lb + 5u]);
        float lint = LightBuf[lb + 6u];
        float3 toLight = lpos - hitPos;
        float dist2 = dot(toLight, toLight);
        float dist = sqrt(dist2);
        if (dist > 1.0e-4) {
            float3 ldir = toLight / dist;
            float cosL = dot(n, ldir);
            if (cosL > 0.0) {
                float3 irrad = lcol * (lint / (dist2 + 1.0e-4));
                float3 direct = float3(0.0, 0.0, 0.0);
                if (!isMirror && !isTransmissive) {
                    direct += albedo * (1.0 / 3.14159265) * irrad * cosL * float(num_lights);
                }
                if (isMirror || isMetallic || isClearcoat || isTransmissive || roughness < 0.65) {
                    float3 viewDir = normalize(-WorldRayDirection());
                    float3 halfDir = normalize(ldir + viewDir);
                    float effectiveRoughness = max(0.025, roughness * (isMetallic ? 0.65 : 1.0));
                    float a2 = effectiveRoughness * effectiveRoughness;
                    float specPower = clamp(2.0 / max(0.0005, a2 * a2) - 2.0, 4.0, 96.0);
                    float spec = pow(saturate(dot(n, halfDir)), specPower);
                    float cosView = saturate(dot(n, viewDir));
                    float ior = MatIor(matIdx);
                    float f0 = (1.0 - ior) / (1.0 + ior);
                    f0 *= f0;
                    float fresnel = f0 + (1.0 - f0) * Pow5Fast(1.0 - cosView);
                    float specStrength = saturate((1.0 - roughness) * 0.8 +
                                                  MatMetallic(matIdx) * 0.45 +
                                                  MatClearcoat(matIdx) * 0.35 +
                                                  (isMirror ? 0.75 : 0.0) +
                                                  (isTransmissive ? fresnel : 0.0));
                    float3 specTint = lerp(float3(1.0, 1.0, 1.0), albedo, isMetallic ? 0.85 : 0.12);
                    direct += specTint * irrad * spec * cosL * float(num_lights) * max(0.15, specStrength);
                }
                if (isTransmissive) {
                    direct += albedo * irrad * cosL * float(num_lights) * (0.08 + 0.22 * MatAlpha(matIdx));
                }
                payload.radiance += payload.throughput * direct;
            }
        }
    }
    if (payload.depth >= 2u) {
        float q = max(payload.throughput.x, max(payload.throughput.y, payload.throughput.z));
        q = clamp(q, 0.05f, 0.95f);
        if (RandF(rng) > q) { payload.rng = rng; payload.done = 1u; return; }
        payload.throughput /= q;
    }
    float3 nextDir;
    if (isMirror) {
        nextDir = WorldRayDirection() - 2.0 * dot(n, WorldRayDirection()) * n;
    } else if (isTransmissive) {
        float cosN = saturate(dot(n, -WorldRayDirection()));
        float ior = MatIor(matIdx);
        float r0 = (1.0 - ior) / (1.0 + ior);
        r0 *= r0;
        float fresnel = r0 + (1.0 - r0) * Pow5Fast(1.0 - cosN);
        if (RandF(rng) < min(0.98, fresnel + MatClearcoat(matIdx) * 0.15)) {
            nextDir = WorldRayDirection() - 2.0 * dot(n, WorldRayDirection()) * n;
        } else {
            nextDir = refract(WorldRayDirection(), n, 1.0 / ior);
            if (dot(nextDir, nextDir) <= 1.0e-8) {
                nextDir = WorldRayDirection() - 2.0 * dot(n, WorldRayDirection()) * n;
            } else {
                nextDir = normalize(nextDir);
            }
        }
    } else {
        float r1 = RandF(rng); float r2 = RandF(rng);
        float phi = 6.28318530718f * r1;
        float sinTheta = sqrt(1.0f - r2); float cosTheta = sqrt(r2);
        float3 t = (abs(n.x) > 0.9f) ? float3(0,1,0) : float3(1,0,0);
        float3 bitan = normalize(cross(n, t)); float3 tan2 = cross(bitan, n);
        float3 diffuseDir = normalize(cos(phi)*sinTheta*tan2 + sin(phi)*sinTheta*bitan + cosTheta*n);
        if (isDiffuse || isToon) {
            nextDir = diffuseDir;
        } else {
            float effectiveRoughness = max(0.025, roughness * (isMetallic ? 0.75 : 1.0) * (isClearcoat ? 0.65 : 1.0));
            float exponent = max(0.0, 2.0 / Pow2Fast(effectiveRoughness * effectiveRoughness) - 2.0);
            float r3 = RandF(rng); float r4 = RandF(rng);
            float p2 = 6.28318530718f * r3;
            float ct = pow(r4, 1.0 / (exponent + 1.0));
            float st = sqrt(max(0.0, 1.0 - ct * ct));
            float3 refl = normalize(WorldRayDirection() - 2.0 * dot(n, WorldRayDirection()) * n);
            float3 ref = (abs(refl.x) > 0.9f) ? float3(0,1,0) : float3(1,0,0);
            float3 tangent = normalize(cross(ref, refl));
            float3 bitangent = cross(refl, tangent);
            nextDir = normalize(tangent * (st * cos(p2)) + bitangent * (st * sin(p2)) + refl * ct);
            if (dot(nextDir, n) <= 0.0 || (isSheen && RandF(rng) < MatSheen(matIdx) * 0.35)) {
                nextDir = diffuseDir;
            }
        }
    }
    float3 bounceWeight = albedo;
    if (isMetallic || isMirror) {
        bounceWeight = albedo * (0.65 + 0.35 * MatMetallic(matIdx));
    } else if (isTransmissive) {
        bounceWeight = albedo * (0.25 + 0.55 * MatAlpha(matIdx)) + float3(0.2, 0.2, 0.2);
    }
    if (isToon) {
        bounceWeight *= (dot(nextDir, n) > 0.55) ? 1.0 : 0.45;
    }
    payload.throughput *= bounceWeight;
    payload._pad = (isMirror || isMetallic || isTransmissive || isClearcoat || roughness < 0.65) ? 1.0 : 0.0;
    payload.rng = rng;
    payload.next_origin = hitPos + n * 1e-4f;
    payload.next_dir    = normalize(nextDir);
    payload.depth++;
}

// ---- PCG hash / sampling helpers -------------------------------------------
uint Pcg(uint v) {
    uint s = v * 747796405u + 2891336453u;
    uint w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
    return (w >> 22u) ^ w;
}

float RandF(inout uint rng) {
    rng = Pcg(rng);
    return (float)rng / 4294967296.0f;
}

void ApplyCameraLens(inout float3 origin, inout float3 dir, inout uint rng) {
    if (aperture_radius <= 0.0f || focus_distance <= 1.0e-4f) return;
    float lens_radius_sample = sqrt(RandF(rng));
    float lens_phi = 6.28318530718f * RandF(rng);
    float aperture_boundary = 1.0f;
    float roundness = saturate(iris_roundness);
    if (iris_blade_count >= 3u && roundness < 0.999f) {
        float blades = (float)min(iris_blade_count, 64u);
        float sector = 6.28318530718f / blades;
        float local_phi = lens_phi - iris_rotation_radians;
        float wrapped = local_phi - sector * floor(local_phi / sector);
        float centered = (wrapped > sector * 0.5f) ? wrapped - sector : wrapped;
        float polygon_boundary = cos(sector * 0.5f) / max(0.1f, cos(centered));
        aperture_boundary = lerp(polygon_boundary, 1.0f, roundness);
    }
    float lens_r = aperture_radius * lens_radius_sample * aperture_boundary;
    float squeeze = max(0.01f, anamorphic_squeeze);
    float3 cam_right = float3(cam_right_x, cam_right_y, cam_right_z);
    float3 cam_up = float3(cam_up_x, cam_up_y, cam_up_z);
    float3 lens_offset =
        cam_right * (lens_r * cos(lens_phi) * squeeze) +
        cam_up * (lens_r * sin(lens_phi));
    float3 focus_point = origin + dir * focus_distance;
    origin += lens_offset;
    dir = normalize(focus_point - origin);
}

float Halton2(uint idx) {
    uint bits = idx;
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return (float)bits * 2.3283064365386963e-10f;
}

float Halton3(uint idx) {
    float result = 0.0f;
    float f      = 1.0f / 3.0f;
    uint  i      = idx;
    for (uint step = 0u; step < 21u; ++step) {
        if (i == 0u) break;
        result += f * (float)(i % 3u);
        i      /= 3u;
        f      /= 3.0f;
    }
    return result;
}
