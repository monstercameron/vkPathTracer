// pathtrace_rt.hlsl — DXR path tracing library shader (target: lib_6_3)
// Iterative path tracer: raygen loops over depth, no shader recursion needed.
// Resource layout mirrors pathtrace_cs.hlsl so the same CPU constant buffer works.

#ifndef PT_D3D12_DXR_SHADOW_RAYS
#define PT_D3D12_DXR_SHADOW_RAYS 0
#endif

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
    float  white_balance_b; uint denoiser_enabled; float denoiser_strength; float denoiser_color_sigma;
    uint   static_bvh_node_count; uint dynamic_bvh_node_count; float _rt_pad0; float _rt_pad1;
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
Buffer<uint>     TexelBuf : register(t8, space0);
Buffer<uint>     TexMetaBuf : register(t9, space0);
Buffer<float>    EnvBuf : register(t10, space0);
Buffer<uint>     EnvMetaBuf : register(t11, space0);
Buffer<float>    SdfBuf : register(t12, space0);
Buffer<float>    BvhBuf : register(t13, space0);
Buffer<float>    DynamicBvhBuf : register(t14, space0);
Buffer<float>    LocalBvhBuf : register(t15, space0);
RWByteAddressBuffer FilmBuf : register(u0, space0);

// ---- Payload (56 bytes; must match MaxPayloadSizeInBytes in PSO) ------------
struct PathPayload {
    float3 radiance;     // 12
    uint   state;        //  4  -> 16
    float3 throughput;   // 12
    uint   rng;          //  4  -> 32
    float3 next_origin;  // 12
    float3 next_dir;     // 12
};

static const uint kMatStride = 16u;
static const uint kInstStride = 24u;
static const uint kPackedTriStride = 18u;
static const uint kSdfStride = 16u;
static const uint kSdfShapeSphere = 0u;
static const uint kSdfShapeBox = 1u;
static const uint kSdfShapeRoundedBox = 2u;
static const uint kStaticInstanceId = 0x00FFFFFFu;
static const uint kInvalidTextureIndex = 0xFFFFFFFFu;
static const uint kPayloadDone = 1u;
static const uint kPayloadShadowRay = 4u;
static const uint kPayloadSdfCandidate = 8u;
static const uint kPayloadDepthShift = 8u;
static const uint kPayloadDepthMask = 0xFFu;
static const uint kInstFlagDynamicTransform = 1u;

struct Hit {
    bool ok;
    float t;
    float3 pos;
    float3 n;
    uint mat;
};

bool PayloadIsDone(PathPayload payload) {
    return (payload.state & kPayloadDone) != 0u;
}

uint PayloadDepth(PathPayload payload) {
    return (payload.state >> kPayloadDepthShift) & kPayloadDepthMask;
}

bool PayloadIsShadowRay(PathPayload payload) {
    return (payload.state & kPayloadShadowRay) != 0u;
}

void PayloadSetDone(inout PathPayload payload) {
    payload.state |= kPayloadDone;
}

void PayloadSetBounceState(inout PathPayload payload, uint depth) {
    payload.state = min(depth, kPayloadDepthMask) << kPayloadDepthShift;
}

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
uint   MatBaseColorTextureIndex(uint idx) {
    uint encoded = MatEffectRaw(idx) >> 11u;
    return encoded == 0u ? kInvalidTextureIndex : encoded - 1u;
}
uint   MatNormalTextureIndex(uint idx) {
    uint encoded = uint(MatBuf[idx*kMatStride + 13u] + 0.5);
    return encoded == 0u ? kInvalidTextureIndex : encoded - 1u;
}

uint WrapTexelCoord(int value, uint size) {
    int wrapped = value % int(size);
    if (wrapped < 0) wrapped += int(size);
    return uint(wrapped);
}

float3 DecodeRgba8(uint packed) {
    return float3(float(packed & 255u),
                  float((packed >> 8u) & 255u),
                  float((packed >> 16u) & 255u)) * (1.0 / 255.0);
}

float3 DecodeRgba8Raw(uint packed) {
    return float3(float(packed & 255u),
                  float((packed >> 8u) & 255u),
                  float((packed >> 16u) & 255u)) * (1.0 / 255.0);
}

float3 LoadTextureTexel(uint textureIndex, int x, int y) {
    uint mb = textureIndex * 4u;
    uint offset = TexMetaBuf[mb + 0u];
    uint width = max(1u, TexMetaBuf[mb + 1u]);
    uint height = max(1u, TexMetaBuf[mb + 2u]);
    uint ix = WrapTexelCoord(x, width);
    uint iy = WrapTexelCoord(y, height);
    return DecodeRgba8(TexelBuf[offset + iy * width + ix]);
}

float3 LoadTextureTexelRaw(uint textureIndex, int x, int y) {
    uint mb = textureIndex * 4u;
    uint offset = TexMetaBuf[mb + 0u];
    uint width = max(1u, TexMetaBuf[mb + 1u]);
    uint height = max(1u, TexMetaBuf[mb + 2u]);
    uint ix = WrapTexelCoord(x, width);
    uint iy = WrapTexelCoord(y, height);
    return DecodeRgba8Raw(TexelBuf[offset + iy * width + ix]);
}

float3 SampleBaseColorTexture(uint textureIndex, float2 uv) {
    uint mb = textureIndex * 4u;
    uint width = max(1u, TexMetaBuf[mb + 1u]);
    uint height = max(1u, TexMetaBuf[mb + 2u]);
    float2 wrapped = frac(uv);
    float x = wrapped.x * float(width) - 0.5;
    float y = (1.0 - wrapped.y) * float(height) - 0.5;
    int x0 = int(floor(x));
    int y0 = int(floor(y));
    float tx = frac(x);
    float ty = frac(y);
    float3 c00 = LoadTextureTexel(textureIndex, x0, y0);
    float3 c10 = LoadTextureTexel(textureIndex, x0 + 1, y0);
    float3 c01 = LoadTextureTexel(textureIndex, x0, y0 + 1);
    float3 c11 = LoadTextureTexel(textureIndex, x0 + 1, y0 + 1);
    return lerp(lerp(c00, c10, tx), lerp(c01, c11, tx), ty);
}

float3 SampleTextureRaw(uint textureIndex, float2 uv) {
    uint mb = textureIndex * 4u;
    uint width = max(1u, TexMetaBuf[mb + 1u]);
    uint height = max(1u, TexMetaBuf[mb + 2u]);
    float2 wrapped = frac(uv);
    float x = wrapped.x * float(width) - 0.5;
    float y = (1.0 - wrapped.y) * float(height) - 0.5;
    int x0 = int(floor(x));
    int y0 = int(floor(y));
    float tx = frac(x);
    float ty = frac(y);
    float3 c00 = LoadTextureTexelRaw(textureIndex, x0, y0);
    float3 c10 = LoadTextureTexelRaw(textureIndex, x0 + 1, y0);
    float3 c01 = LoadTextureTexelRaw(textureIndex, x0, y0 + 1);
    float3 c11 = LoadTextureTexelRaw(textureIndex, x0 + 1, y0 + 1);
    return lerp(lerp(c00, c10, tx), lerp(c01, c11, tx), ty);
}

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

float3 ProceduralWoodAlbedo(float3 base, float3 p, float roughness, float clearcoat, uint style) {
    // Style separates species/layout: parquet uses broad polished boards, walnut uses rough
    // dark bench grain, and sandalwood uses fine pale frame streaks instead of floor planks.
    float rough = saturate(roughness);
    float coat = saturate(clearcoat);
    float polish = saturate(coat * (1.25 - rough * 0.75));
    float parquet = (style == 17u) ? 1.0 : 0.0;
    float walnut = (style == 18u) ? 1.0 : 0.0;
    float sandal = (style == 19u) ? 1.0 : 0.0;
    float plankWidth = 0.40 + 0.16 * parquet + 0.08 * polish - 0.18 * walnut;
    float plankLength = 1.15 + 0.85 * parquet + 0.55 * polish - 0.55 * walnut;
    float px = (p.x + sandal * p.y * 0.35) / max(0.12, plankWidth);
    float pz = (p.z + walnut * p.y * 0.70 + sandal * p.x * 0.22) / max(0.30, plankLength);
    float cellX = floor(px);
    float cellZ = floor(pz);
    float localX = frac(px);
    float localZ = frac(pz);
    float seed = Hash01(float3(cellX, cellZ, 0.0), 16.0);
    float macroNoise = Hash01(floor(p * 3.7), 31.0 + float(style));
    float microNoise = Hash01(floor(p * 34.0), 47.0 + float(style));
    bool rotate = parquet > 0.5 && fmod(cellX + cellZ, 2.0) >= 1.0;
    float along = rotate ? localZ : localX;
    float across = rotate ? localX : localZ;
    float grainWobble = (macroNoise - 0.5) * (0.06 + rough * 0.12 + walnut * 0.08 + sandal * 0.04);
    float warpedAlong = along + grainWobble + sin(across * 6.0 + seed * 5.0) * 0.018;
    float grainFreq = 12.0 + rough * 8.0 + walnut * 22.0 + sandal * 36.0;
    float wave = sin((warpedAlong * grainFreq + seed * 6.2831853) +
                     sin(across * (6.0 + rough * 8.0) + seed * 4.0) * (0.25 + rough * 0.45 + walnut * 0.45));
    float fine = sin(warpedAlong * (55.0 + rough * 55.0 + sandal * 110.0) +
                     across * (9.0 + walnut * 25.0) + seed * 12.0);
    float fineWeight = 0.10 + rough * 0.18 + walnut * 0.18 + sandal * 0.22;
    float ring = smoothstep(-0.35 + polish * 0.18, 0.82, wave) * (1.0 - fineWeight) +
                 (0.5 + 0.5 * fine) * fineWeight;
    float seam = (1.0 - sandal) * (1.0 - smoothstep(0.0, 0.028 + polish * 0.018,
        min(min(localX, 1.0 - localX), min(localZ, 1.0 - localZ))));
    float3 dark = base * (0.46 - rough * 0.10 - walnut * 0.18);
    float3 amber = base * (1.14 + coat * 0.22 + sandal * 0.18) +
                   float3(0.08 + sandal * 0.18, 0.055 + sandal * 0.06, 0.025);
    float3 wood = lerp(dark, amber, ring);
    float boardTint = 0.92 + (0.08 + walnut * 0.12 + sandal * 0.06) * seed +
                      (macroNoise - 0.5) * (0.08 + rough * 0.06);
    float pores = smoothstep(0.74 - walnut * 0.08, 1.0, microNoise) *
                  (0.025 + rough * 0.055 + walnut * 0.055 + sandal * 0.018);
    wood *= boardTint;
    wood *= 1.0 - pores;
    wood *= 1.0 - seam * (0.10 + rough * 0.35 + parquet * 0.18) * (1.0 - coat * 0.45);
    float woodMix = 0.30 + rough * 0.34 - polish * 0.14 + walnut * 0.26 + sandal * 0.18;
    return base * (1.0 - woodMix) + wood * woodMix;
}

float3 MatSurfaceAlbedo(uint idx, float3 p, float3 n, float3 rd, float roughness, float clearcoat) {
    float3 color = MatAlbedo(idx);
    uint effect = MatEffect(idx);
    if (effect == 0u) {
        return clamp(color, 0.0, 1.5);
    }
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
    } else if (effect >= 16u && effect <= 19u) {
        color = ProceduralWoodAlbedo(color, p, roughness, clearcoat, effect);
    }
    return clamp(color, 0.0, 1.5);
}

float3 LoadEnvTexel(uint x, uint y, uint width) {
    uint base = (y * width + x) * 3u;
    return float3(EnvBuf[base], EnvBuf[base + 1u], EnvBuf[base + 2u]);
}

float3 SampleSceneEnvironment(float3 rd, float3 fallbackEnv) {
    uint width = EnvMetaBuf[0];
    uint height = EnvMetaBuf[1];
    uint enabled = EnvMetaBuf[2];
    if (enabled == 0u || width == 0u || height == 0u) {
        return fallbackEnv;
    }
    float3 dir = normalize(rd);
    float u = frac(0.5 + atan2(dir.z, dir.x) * 0.15915494309189535);
    float v = acos(clamp(dir.y, -1.0, 1.0)) * 0.3183098861837907;
    float x = u * float(width);
    float y = saturate(v) * float(max(1u, height) - 1u);
    uint x0 = uint(floor(x)) % width;
    uint x1 = (x0 + 1u) % width;
    uint y0 = min(uint(floor(y)), height - 1u);
    uint y1 = min(y0 + 1u, height - 1u);
    float tx = x - floor(x);
    float ty = y - floor(y);
    float3 top = lerp(LoadEnvTexel(x0, y0, width), LoadEnvTexel(x1, y0, width), tx);
    float3 bottom = lerp(LoadEnvTexel(x0, y1, width), LoadEnvTexel(x1, y1, width), tx);
    return lerp(top, bottom, ty);
}

float3 SdfBoxNormal(float3 local, float3 halfExtents) {
    float3 faceDist = abs(halfExtents - abs(local));
    if (faceDist.x <= faceDist.y && faceDist.x <= faceDist.z) {
        return float3(local.x >= 0.0f ? 1.0f : -1.0f, 0.0f, 0.0f);
    }
    if (faceDist.y <= faceDist.z) {
        return float3(0.0f, local.y >= 0.0f ? 1.0f : -1.0f, 0.0f);
    }
    return float3(0.0f, 0.0f, local.z >= 0.0f ? 1.0f : -1.0f);
}

bool IntersectSdfSphere(uint sdfIndex, float3 ro, float3 rd, inout Hit h) {
    uint base = sdfIndex * kSdfStride;
    uint shape = uint(SdfBuf[base + 0u] + 0.5f);
    if (shape != kSdfShapeSphere) return false;
    float3 center = float3(SdfBuf[base + 4u], SdfBuf[base + 5u], SdfBuf[base + 6u]);
    float3 scale = abs(float3(SdfBuf[base + 8u], SdfBuf[base + 9u], SdfBuf[base + 10u]));
    float scaleMax = max(scale.x, max(scale.y, scale.z));
    float radius = max(0.001f, SdfBuf[base + 2u] * max(scaleMax, 0.001f));
    float3 oc = ro - center;
    float halfB = dot(oc, rd);
    float c = dot(oc, oc) - radius * radius;
    float discriminant = halfB * halfB - c;
    if (discriminant < 0.0f) return false;
    float root = sqrt(discriminant);
    float t = -halfB - root;
    if (t <= 1.0e-4f) t = -halfB + root;
    if (t <= 1.0e-4f || t >= h.t) return false;
    h.ok = true;
    h.t = t;
    h.pos = ro + rd * t;
    h.n = normalize(h.pos - center);
    h.mat = uint(SdfBuf[base + 1u] + 0.5f);
    return true;
}

bool IntersectSdfBox(uint sdfIndex, float3 ro, float3 rd, inout Hit h) {
    uint base = sdfIndex * kSdfStride;
    uint shape = uint(SdfBuf[base + 0u] + 0.5f);
    if (shape != kSdfShapeBox && shape != kSdfShapeRoundedBox) return false;
    float3 center = float3(SdfBuf[base + 4u], SdfBuf[base + 5u], SdfBuf[base + 6u]);
    float3 halfExtents = max(abs(float3(SdfBuf[base + 8u], SdfBuf[base + 9u], SdfBuf[base + 10u])),
                             float3(0.001f, 0.001f, 0.001f));
    float3 bmin = center - halfExtents;
    float3 bmax = center + halfExtents;
    float3 invRd = 1.0f / rd;
    float3 t0 = (bmin - ro) * invRd;
    float3 t1 = (bmax - ro) * invRd;
    float3 tsm = min(t0, t1);
    float3 tbg = max(t0, t1);
    float tNear = max(tsm.x, max(tsm.y, tsm.z));
    float tFar = min(tbg.x, min(tbg.y, tbg.z));
    if (tFar <= 1.0e-4f || tNear > tFar) return false;
    float t = (tNear > 1.0e-4f) ? tNear : tFar;
    if (t <= 1.0e-4f || t >= h.t) return false;
    h.ok = true;
    h.t = t;
    h.pos = ro + rd * t;
    h.n = SdfBoxNormal(h.pos - center, halfExtents);
    h.mat = uint(SdfBuf[base + 1u] + 0.5f);
    return true;
}

bool IntersectSdfPrimitive(uint sdfIndex, float3 ro, float3 rd, inout Hit h) {
    uint base = sdfIndex * kSdfStride;
    uint shape = uint(SdfBuf[base + 0u] + 0.5f);
    if (shape == kSdfShapeSphere) return IntersectSdfSphere(sdfIndex, ro, rd, h);
    if (shape == kSdfShapeBox || shape == kSdfShapeRoundedBox) return IntersectSdfBox(sdfIndex, ro, rd, h);
    return false;
}

Hit IntersectSdfScene(float3 ro, float3 rd) {
    Hit h;
    h.ok = false;
    h.t = 1.0e30f;
    h.pos = float3(0.0f, 0.0f, 0.0f);
    h.n = float3(0.0f, 1.0f, 0.0f);
    h.mat = 0u;
    [loop]
    for (uint si = 0u; si < num_sdfs; ++si) {
        IntersectSdfPrimitive(si, ro, rd, h);
    }
    return h;
}

void LoadPackedTriGeometry(uint b, out float3 v0, out float3 e1, out float3 e2,
                           out bool doubleSided) {
    v0 = float3(TriDataBuf[b + 0u], TriDataBuf[b + 1u], TriDataBuf[b + 2u]);
    e1 = float3(TriDataBuf[b + 4u], TriDataBuf[b + 5u], TriDataBuf[b + 6u]);
    doubleSided = TriDataBuf[b + 7u] > 0.5f;
    e2 = float3(TriDataBuf[b + 8u], TriDataBuf[b + 9u], TriDataBuf[b + 10u]);
}

float3 SafeInstanceScale(uint ib) {
    float3 s = float3(asfloat(InstBuf[ib + 12u]), asfloat(InstBuf[ib + 13u]), asfloat(InstBuf[ib + 14u]));
    s.x = (abs(s.x) < 1.0e-6f) ? 1.0f : s.x;
    s.y = (abs(s.y) < 1.0e-6f) ? 1.0f : s.y;
    s.z = (abs(s.z) < 1.0e-6f) ? 1.0f : s.z;
    return s;
}

float4 InstanceRotation(uint ib) {
    float4 q = float4(asfloat(InstBuf[ib + 8u]), asfloat(InstBuf[ib + 9u]),
                      asfloat(InstBuf[ib + 10u]), asfloat(InstBuf[ib + 11u]));
    float len2 = max(1.0e-12f, dot(q, q));
    return q * rsqrt(len2);
}

float3 RotateQuat(float3 v, float4 q) {
    float3 t = 2.0f * cross(q.xyz, v);
    return v + q.w * t + cross(q.xyz, t);
}

float3 RotateInvQuat(float3 v, float4 q) {
    return RotateQuat(v, float4(-q.xyz, q.w));
}

float3 InstanceWorldToLocalPoint(uint ib, float3 p) {
    float3 t = float3(asfloat(InstBuf[ib + 4u]), asfloat(InstBuf[ib + 5u]), asfloat(InstBuf[ib + 6u]));
    return RotateInvQuat(p - t, InstanceRotation(ib)) / SafeInstanceScale(ib);
}

float3 InstanceWorldToLocalVector(uint ib, float3 v) {
    return RotateInvQuat(v, InstanceRotation(ib)) / SafeInstanceScale(ib);
}

bool IntersectLocalInstanceBounds(uint ib, float3 roLocal, float3 rdLocal, float maxT) {
    float3 bmin = float3(asfloat(InstBuf[ib + 16u]), asfloat(InstBuf[ib + 17u]), asfloat(InstBuf[ib + 18u]));
    float3 bmax = float3(asfloat(InstBuf[ib + 20u]), asfloat(InstBuf[ib + 21u]), asfloat(InstBuf[ib + 22u]));
    float3 invRd = float3(
        abs(rdLocal.x) > 1.0e-8f ? 1.0f / rdLocal.x : 1.0e30f,
        abs(rdLocal.y) > 1.0e-8f ? 1.0f / rdLocal.y : 1.0e30f,
        abs(rdLocal.z) > 1.0e-8f ? 1.0f / rdLocal.z : 1.0e30f);
    float3 t0 = (bmin - roLocal) * invRd;
    float3 t1 = (bmax - roLocal) * invRd;
    float tEnter = max(max(min(t0.x, t1.x), min(t0.y, t1.y)), min(t0.z, t1.z));
    float tExit = min(min(max(t0.x, t1.x), max(t0.y, t1.y)), max(t0.z, t1.z));
    return tExit >= 1.0e-4f && tEnter <= tExit && tEnter <= maxT;
}

bool IntersectWorldBounds(float3 bmin, float3 bmax, float3 ro, float3 invRd, float maxT) {
    float3 t0 = (bmin - ro) * invRd;
    float3 t1 = (bmax - ro) * invRd;
    float tEnter = max(max(min(t0.x, t1.x), min(t0.y, t1.y)), min(t0.z, t1.z));
    float tExit = min(min(max(t0.x, t1.x), max(t0.y, t1.y)), max(t0.z, t1.z));
    return tExit >= 1.0e-4f && tEnter <= tExit && tEnter <= maxT;
}

bool IntersectPackedTriAnyLocal(float3 ro, float3 rd, uint tri, bool instanceDoubleSided, float maxT) {
    uint b = tri * kPackedTriStride;
    float3 v0;
    float3 e1;
    float3 e2;
    bool packedDoubleSided;
    LoadPackedTriGeometry(b, v0, e1, e2, packedDoubleSided);
    bool doubleSided = instanceDoubleSided || packedDoubleSided;

    float3 h = cross(rd, e2);
    float a = dot(e1, h);
    if (doubleSided) {
        if (abs(a) < 1.0e-5f) return false;
    } else if (a < 1.0e-5f) {
        return false;
    }

    float f = 1.0f / a;
    float3 s = ro - v0;
    float u = f * dot(s, h);
    if (u < 0.0f || u > 1.0f) return false;
    float3 q = cross(s, e1);
    float v = f * dot(rd, q);
    if (v < 0.0f || u + v > 1.0f) return false;
    float t = f * dot(e2, q);
    return t > 1.0e-4f && t < maxT;
}

bool MaterialCastsShadow(uint matIdx) {
    return MatModel(matIdx) != 5u && MatTransmission(matIdx) <= 0.05f && dot(MatEmissive(matIdx), 1.0.xxx) <= 0.0f;
}

bool OccludedDynamicInstanceShader(uint instanceIndex, float3 ro, float3 rd, float maxT) {
    uint ib = instanceIndex * kInstStride;
    uint flags = InstBuf[ib + 3u];
    if ((flags & kInstFlagDynamicTransform) == 0u) return false;
    uint firstTri = InstBuf[ib + 0u];
    uint triCount = InstBuf[ib + 1u];
    uint matIdx = InstBuf[ib + 2u];
    uint localBvhFirst = InstBuf[ib + 7u];
    uint localBvhCount = InstBuf[ib + 15u];
    float3 roLocal = InstanceWorldToLocalPoint(ib, ro);
    float3 rdLocal = InstanceWorldToLocalVector(ib, rd);
    if (!IntersectLocalInstanceBounds(ib, roLocal, rdLocal, maxT)) return false;
    bool doubleSided = MatDoubleSided(matIdx);

    if (localBvhCount > 0u) {
        float3 invLocalRd = float3(
            abs(rdLocal.x) > 1.0e-8f ? 1.0f / rdLocal.x : 1.0e30f,
            abs(rdLocal.y) > 1.0e-8f ? 1.0f / rdLocal.y : 1.0e30f,
            abs(rdLocal.z) > 1.0e-8f ? 1.0f / rdLocal.z : 1.0e30f);
        uint stack[64];
        uint sp = 0u;
        stack[sp++] = localBvhFirst;
        uint localBvhEnd = localBvhFirst + localBvhCount;
        [loop]
        while (sp > 0u) {
            uint ni = stack[--sp];
            if (ni < localBvhFirst || ni >= localBvhEnd) continue;
            uint nb = ni * 8u;
            float3 bmin = float3(LocalBvhBuf[nb + 0u], LocalBvhBuf[nb + 1u], LocalBvhBuf[nb + 2u]);
            float3 bmax = float3(LocalBvhBuf[nb + 3u], LocalBvhBuf[nb + 4u], LocalBvhBuf[nb + 5u]);
            if (!IntersectWorldBounds(bmin, bmax, roLocal, invLocalRd, maxT)) continue;
            uint lf = asuint(LocalBvhBuf[nb + 6u]);
            uint rc = asuint(LocalBvhBuf[nb + 7u]);
            bool isLeaf = (lf & 0x80000000u) != 0u;
            if (isLeaf) {
                uint leafFirstTri = lf & 0x7FFFFFFFu;
                [loop]
                for (uint ti = 0u; ti < rc; ++ti) {
                    if (IntersectPackedTriAnyLocal(roLocal, rdLocal, leafFirstTri + ti, doubleSided, maxT)) {
                        return true;
                    }
                }
            } else if (sp + 2u <= 64u) {
                stack[sp++] = rc;
                stack[sp++] = lf;
            }
        }
        return false;
    }

    [loop]
    for (uint ti = 0u; ti < triCount; ++ti) {
        if (IntersectPackedTriAnyLocal(roLocal, rdLocal, firstTri + ti, doubleSided, maxT)) {
            return true;
        }
    }
    return false;
}

bool OccludedSceneShader(float3 ro, float3 rd, float maxT) {
    if (maxT <= 1.0e-4f) return false;
    float3 invRd = float3(
        abs(rd.x) > 1.0e-8f ? 1.0f / rd.x : 1.0e30f,
        abs(rd.y) > 1.0e-8f ? 1.0f / rd.y : 1.0e30f,
        abs(rd.z) > 1.0e-8f ? 1.0f / rd.z : 1.0e30f);

    uint stack[128];
    uint sp = 0u;
    if (static_bvh_node_count > 0u) {
        stack[sp++] = 0u;
        [loop]
        while (sp > 0u) {
            uint ni = stack[--sp];
            if (ni >= static_bvh_node_count) continue;
            uint nb = ni * 8u;
            float3 bmin = float3(BvhBuf[nb + 0u], BvhBuf[nb + 1u], BvhBuf[nb + 2u]);
            float3 bmax = float3(BvhBuf[nb + 3u], BvhBuf[nb + 4u], BvhBuf[nb + 5u]);
            if (!IntersectWorldBounds(bmin, bmax, ro, invRd, maxT)) continue;
            uint lf = asuint(BvhBuf[nb + 6u]);
            uint rc = asuint(BvhBuf[nb + 7u]);
            bool isLeaf = (lf & 0x80000000u) != 0u;
            if (isLeaf) {
                uint firstTri = lf & 0x7FFFFFFFu;
                [loop]
                for (uint ti = 0u; ti < rc; ++ti) {
                    if (IntersectPackedTriAnyLocal(ro, rd, firstTri + ti, false, maxT)) {
                        return true;
                    }
                }
            } else if (sp + 2u <= 128u) {
                stack[sp++] = rc;
                stack[sp++] = lf;
            }
        }
    }

    sp = 0u;
    if (dynamic_bvh_node_count > 0u) {
        stack[sp++] = 0u;
        [loop]
        while (sp > 0u) {
            uint ni = stack[--sp];
            if (ni >= dynamic_bvh_node_count) continue;
            uint nb = ni * 8u;
            float3 bmin = float3(DynamicBvhBuf[nb + 0u], DynamicBvhBuf[nb + 1u], DynamicBvhBuf[nb + 2u]);
            float3 bmax = float3(DynamicBvhBuf[nb + 3u], DynamicBvhBuf[nb + 4u], DynamicBvhBuf[nb + 5u]);
            if (!IntersectWorldBounds(bmin, bmax, ro, invRd, maxT)) continue;
            uint lf = asuint(DynamicBvhBuf[nb + 6u]);
            uint rc = asuint(DynamicBvhBuf[nb + 7u]);
            bool isLeaf = (lf & 0x80000000u) != 0u;
            if (isLeaf) {
                if (rc == 0u) continue;
                uint instanceIndex = lf & 0x7FFFFFFFu;
                if (OccludedDynamicInstanceShader(instanceIndex, ro, rd, maxT)) {
                    return true;
                }
            } else if (sp + 2u <= 128u) {
                stack[sp++] = rc;
                stack[sp++] = lf;
            }
        }
    }

    Hit sdfHit = IntersectSdfScene(ro, rd);
    if (sdfHit.ok && sdfHit.t < maxT) {
        uint shadowMat = (num_mats > 0u) ? min(sdfHit.mat, num_mats - 1u) : 0u;
        if (MaterialCastsShadow(shadowMat)) return true;
    }
    return false;
}

// ---- Forward declarations --------------------------------------------------
uint  Pcg(uint v);
float RandF(inout uint rng);
float Halton2(uint idx);
float Halton3(uint idx);
void  ApplyCameraLens(inout float3 origin, inout float3 dir, inout uint rng);
void  ShadeSurface(inout PathPayload payload, float3 hitPos, float3 surfaceNormal, uint matIdx, float3 rayDir);

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
        payload.state       = 0u;
        payload.rng         = rng;
        payload.next_origin = origin;
        payload.next_dir    = dir;

        uint maxD = (uint)max(1.0f, max_depth_f);
        for (uint d = 0u; d < maxD && !PayloadIsDone(payload); ++d) {
            Hit sdfHit = IntersectSdfScene(payload.next_origin, payload.next_dir);
            if (sdfHit.ok) {
                payload.state |= kPayloadSdfCandidate;
            } else {
                payload.state &= ~kPayloadSdfCandidate;
            }
            float3 rayOrigin = payload.next_origin;
            float3 rayDir = payload.next_dir;
            RayDesc ray;
            ray.Origin    = rayOrigin;
            ray.Direction = rayDir;
            ray.TMin      = 1e-4f;
            ray.TMax      = sdfHit.ok ? max(1.0e-4f, sdfHit.t - 1.0e-4f) : 1e30f;
            TraceRay(SceneTLAS,
                     RAY_FLAG_FORCE_OPAQUE,
                     /*InstanceMask*/ 0xFF,
                     /*HitGroup offset*/ 0,
                     /*Geometry multiplier*/ 1,
                     /*MissShader index*/ 0,
                     ray, payload);
            if (sdfHit.ok && PayloadIsDone(payload) && ((payload.state & kPayloadSdfCandidate) != 0u)) {
                payload.state &= ~kPayloadDone;
                ShadeSurface(payload, sdfHit.pos, sdfHit.n, sdfHit.mat, rayDir);
            } else {
                payload.state &= ~kPayloadSdfCandidate;
            }
        }
        total += payload.radiance;
    }

    float4 prev = LoadFilm(pixel);
    StoreFilm(pixel, float4(prev.xyz + total, prev.w + (float)rpp));
}

// ---- Miss ------------------------------------------------------------------
[shader("miss")]
void Miss(inout PathPayload payload) {
#if PT_D3D12_DXR_SHADOW_RAYS
    if (PayloadIsShadowRay(payload)) {
        payload.state &= ~kPayloadDone;
        return;
    }
#endif
    if ((payload.state & kPayloadSdfCandidate) != 0u) {
        PayloadSetDone(payload);
        return;
    }
    float3 env = SampleSceneEnvironment(WorldRayDirection(), float3(env_r, env_g, env_b));
    payload.radiance += payload.throughput * env;
    PayloadSetDone(payload);
}

void ShadeSurface(inout PathPayload payload, float3 hitPos, float3 surfaceNormal, uint matIdx, float3 rayDir) {
    bool enteringSurface = dot(rayDir, surfaceNormal) < 0.0f;
    float3 n = enteringSurface ? surfaceNormal : -surfaceNormal;
    float3 emissive = MatEmissive(matIdx);

    float roughness = MatRoughness(matIdx);
    uint model = MatModel(matIdx);
    float metallic = MatMetallic(matIdx);
    float transmission = MatTransmission(matIdx);
    float clearcoat = MatClearcoat(matIdx);
    float3 albedo = MatSurfaceAlbedo(matIdx, hitPos, n, rayDir, roughness, clearcoat);
    bool isMirror = (model == 2u) || (roughness <= 0.001);
    bool isMetallic = (model == 4u) || (metallic > 0.65);
    bool isTransmissive = (model == 5u) || (transmission > 0.05);
    bool isDiffuse = (roughness >= 0.999) && !isMirror && !isMetallic && !isTransmissive;
    bool isSheen = (model == 6u);
    bool isClearcoat = (model == 7u) || (clearcoat > 0.05);
    bool isToon = (model == 8u);
    uint rng = payload.rng;
    uint depth = PayloadDepth(payload);

    if (depth == 0u || num_lights == 0u) {
        payload.radiance += payload.throughput * emissive;
    }

    if (num_lights > 0u) {
        uint li = uint(RandF(rng) * float(num_lights));
        li = min(li, num_lights - 1u);
        uint lb = li * 16u;
        float3 lpos = float3(LightBuf[lb], LightBuf[lb + 1u], LightBuf[lb + 2u]);
        float3 lcol = float3(LightBuf[lb + 3u], LightBuf[lb + 4u], LightBuf[lb + 5u]);
        float lint = LightBuf[lb + 6u];
        float lrad = max(0.0f, LightBuf[lb + 7u]);
        float3 spotDir = normalize(float3(LightBuf[lb + 8u], LightBuf[lb + 9u], LightBuf[lb + 10u]));
        float spotInner = LightBuf[lb + 11u];
        float spotOuter = LightBuf[lb + 12u];
        if (lrad > 1.0e-5f) {
            float uz = 1.0f - 2.0f * RandF(rng);
            float upr = sqrt(max(0.0f, 1.0f - uz * uz));
            float phi = 6.28318530718f * RandF(rng);
            lpos += float3(upr * cos(phi), uz, upr * sin(phi)) * lrad;
        }
        float3 toLight = lpos - hitPos;
        float dist2 = dot(toLight, toLight);
        float dist = sqrt(dist2);
        if (dist > 1.0e-4) {
            float3 ldir = toLight / dist;
            float cosL = dot(n, ldir);
            float spotFactor = 1.0f;
            if (spotInner > -0.999f) {
                float cone = dot(-ldir, spotDir);
                if (cone <= spotOuter) {
                    spotFactor = 0.0f;
                } else if (cone < spotInner && spotInner > spotOuter) {
                    float t = saturate((cone - spotOuter) / (spotInner - spotOuter));
                    spotFactor = t * t * (3.0f - 2.0f * t);
                }
            }
            if (cosL > 0.0 && spotFactor > 0.0f) {
                Hit sdfShadowHit = IntersectSdfScene(hitPos + n * 0.002f, ldir);
                bool sdfShadowOcc = false;
                if (sdfShadowHit.ok && sdfShadowHit.t < dist - 0.004f) {
                    uint shadowMat = (num_mats > 0u) ? min(sdfShadowHit.mat, num_mats - 1u) : 0u;
                    sdfShadowOcc = !((MatModel(shadowMat) == 5u) || (MatTransmission(shadowMat) > 0.05f));
                }
#if PT_D3D12_DXR_SHADOW_RAYS
                PathPayload shadowPayload;
                shadowPayload.radiance = (float3)0;
                shadowPayload.throughput = (float3)0;
                shadowPayload.state = kPayloadShadowRay | kPayloadDone;
                shadowPayload.rng = 0u;
                shadowPayload.next_origin = (float3)0;
                shadowPayload.next_dir = (float3)0;
                RayDesc shadowRay;
                shadowRay.Origin = hitPos + n * 0.002f;
                shadowRay.Direction = ldir;
                shadowRay.TMin = 0.0f;
                shadowRay.TMax = max(0.0f, dist - 0.004f);
                TraceRay(SceneTLAS,
                         RAY_FLAG_FORCE_NON_OPAQUE |
                         RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
                         RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,
                         0xFF, 0, 1, 0, shadowRay, shadowPayload);
                if ((!PayloadIsDone(shadowPayload) && !sdfShadowOcc) || isTransmissive) {
#else
                bool triShadowOcc = OccludedSceneShader(hitPos + n * 0.002f, ldir, dist - 0.004f);
                if ((!triShadowOcc && !sdfShadowOcc) || isTransmissive) {
#endif
                float3 irrad = lcol * ((lint * spotFactor) / (dist2 + 1.0e-4));
                float3 direct = float3(0.0, 0.0, 0.0);
                if (!isMirror && !isTransmissive) {
                    direct += albedo * (1.0 / 3.14159265) * irrad * cosL * float(num_lights);
                }
                if (isMirror || isMetallic || isClearcoat || isTransmissive || roughness < 0.65) {
                    float3 viewDir = normalize(-rayDir);
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
                                                  metallic * 0.45 +
                                                  clearcoat * 0.35 +
                                                  (isMirror ? 0.75 : 0.0) +
                                                  (isTransmissive ? fresnel : 0.0));
                    float3 specTint = lerp(float3(1.0, 1.0, 1.0), albedo, isMetallic ? 0.85 : 0.12);
                    direct += specTint * irrad * spec * cosL * float(num_lights) * max(0.15, specStrength);
                }
                payload.radiance += payload.throughput * direct;
                }
            }
        }
    }

    if (depth >= 3u) {
        float q = max(payload.throughput.x, max(payload.throughput.y, payload.throughput.z));
        q = clamp(q, 0.1f, 0.99f);
        if (RandF(rng) > q) { payload.rng = rng; PayloadSetDone(payload); return; }
        payload.throughput /= q;
    }
    float3 nextDir;
    bool refractedBounce = false;
    if (isMirror) {
        nextDir = rayDir - 2.0 * dot(n, rayDir) * n;
    } else if (isTransmissive) {
        float cosN = saturate(dot(n, -rayDir));
        float ior = MatIor(matIdx);
        float r0 = (1.0 - ior) / (1.0 + ior);
        r0 *= r0;
        float fresnel = r0 + (1.0 - r0) * Pow5Fast(1.0 - cosN);
        if (RandF(rng) < min(0.98, fresnel + clearcoat * 0.015)) {
            nextDir = rayDir - 2.0 * dot(n, rayDir) * n;
        } else {
            float eta = enteringSurface ? (1.0 / ior) : ior;
            nextDir = refract(rayDir, n, eta);
            if (dot(nextDir, nextDir) <= 1.0e-8) {
                nextDir = rayDir - 2.0 * dot(n, rayDir) * n;
            } else {
                nextDir = normalize(nextDir);
                refractedBounce = true;
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
            float3 refl = normalize(rayDir - 2.0 * dot(n, rayDir) * n);
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
        bounceWeight = albedo * (0.65 + 0.35 * metallic);
    } else if (isTransmissive) {
        float tintStrength = saturate(MatAlpha(matIdx)) * transmission;
        bounceWeight = lerp(float3(1.0, 1.0, 1.0), albedo, tintStrength);
        if (refractedBounce) {
            bounceWeight *= max(0.55, transmission);
        }
    }
    if (isToon) {
        bounceWeight *= (dot(nextDir, n) > 0.55) ? 1.0 : 0.45;
    }
    payload.throughput *= bounceWeight;
    payload.rng = rng;
    payload.next_dir = normalize(nextDir);
    payload.next_origin = hitPos + payload.next_dir * 0.002f;
    PayloadSetBounceState(payload, depth + 1u);
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
    float2 uv0 = float2(TriDataBuf[tb + 12u], TriDataBuf[tb + 13u]);
    float2 uv1 = float2(TriDataBuf[tb + 14u], TriDataBuf[tb + 15u]);
    float2 uv2 = float2(TriDataBuf[tb + 16u], TriDataBuf[tb + 17u]);
    float baryZ = 1.0f - attr.barycentrics.x - attr.barycentrics.y;
    float2 uv = uv0 * baryZ + uv1 * attr.barycentrics.x + uv2 * attr.barycentrics.y;
    float3x4 o2w = ObjectToWorld3x4();
    float3 we1 = float3(dot(o2w[0].xyz, e1), dot(o2w[1].xyz, e1), dot(o2w[2].xyz, e1));
    float3 we2 = float3(dot(o2w[0].xyz, e2), dot(o2w[1].xyz, e2), dot(o2w[2].xyz, e2));
    float3 geomN = normalize(cross(we1, we2));
    bool enteringSurface = dot(WorldRayDirection(), geomN) < 0.0f;
    float3 n = enteringSurface ? geomN : -geomN;
    float3 hitPos = WorldRayOrigin() + WorldRayDirection() * RayTCurrent();
    uint normalTexture = MatNormalTextureIndex(matIdx);
    if (normalTexture != kInvalidTextureIndex) {
        float2 duv1 = uv1 - uv0;
        float2 duv2 = uv2 - uv0;
        float det = duv1.x * duv2.y - duv1.y * duv2.x;
        if (abs(det) > 1.0e-8f) {
            float invDet = 1.0f / det;
            float3 tangent = normalize((we1 * duv2.y - we2 * duv1.y) * invDet);
            float3 bitangent = normalize((-we1 * duv2.x + we2 * duv1.x) * invDet);
            tangent = normalize(tangent - n * dot(n, tangent));
            bitangent = normalize(cross(n, tangent) * (det < 0.0f ? -1.0f : 1.0f));
            float3 encodedNormal = SampleTextureRaw(normalTexture, uv);
            float2 normalXY = encodedNormal.xy * 2.0f - 1.0f;
            normalXY = clamp(normalXY, float2(-1.0f, -1.0f), float2(1.0f, 1.0f));
            float normalZ = sqrt(saturate(1.0f - dot(normalXY, normalXY)));
            float3 mappedNormal = normalize(tangent * normalXY.x + bitangent * normalXY.y + n * normalZ);
            if (dot(mappedNormal, n) > 0.0f) {
                n = normalize(lerp(n, mappedNormal, 0.85f));
            }
        }
    }
    ShadeSurface(payload, hitPos, n, matIdx, WorldRayDirection());
}

[shader("anyhit")]
void AnyHit(inout PathPayload payload, in BuiltInTriangleIntersectionAttributes attr) {
    uint instanceId = InstanceID();
    uint triIdx = PrimitiveIndex();
    if (instanceId != kStaticInstanceId) {
        uint ib = instanceId * kInstStride;
        triIdx += InstBuf[ib + 0u];
    }
    uint tb = triIdx * kPackedTriStride;
    uint matIdx = uint(TriDataBuf[tb + 3u] + 0.5f);
    float3 e1 = float3(TriDataBuf[tb + 4u], TriDataBuf[tb + 5u], TriDataBuf[tb + 6u]);
    bool doubleSided = TriDataBuf[tb + 7u] > 0.5f;
    float3 e2 = float3(TriDataBuf[tb + 8u], TriDataBuf[tb + 9u], TriDataBuf[tb + 10u]);
    float3x4 o2w = ObjectToWorld3x4();
    float3 we1 = float3(dot(o2w[0].xyz, e1), dot(o2w[1].xyz, e1), dot(o2w[2].xyz, e1));
    float3 we2 = float3(dot(o2w[0].xyz, e2), dot(o2w[1].xyz, e2), dot(o2w[2].xyz, e2));
    float a = dot(we1, cross(WorldRayDirection(), we2));
    if ((!doubleSided && a > -1.0e-5f) || (doubleSided && abs(a) < 1.0e-5f)) {
        IgnoreHit();
        return;
    }
    if (MatModel(matIdx) == 5u || MatTransmission(matIdx) > 0.05f || dot(MatEmissive(matIdx), 1.0.xxx) > 0.0f) {
        IgnoreHit();
        return;
    }
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
