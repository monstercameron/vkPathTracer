// GPU path tracer compute shader (HLSL / SM 6.0).
// One invocation per pixel; accumulates into a persistent RGBA32F ByteAddressBuffer.
// Root constants carry all per-dispatch state (camera, scene counts, sample index).
// Layout mirrors PathTracerSceneSnapshot on the CPU side and pathtrace.comp (GLSL).

#ifndef PT_D3D12_STATIC_TRAVERSAL_MODE
#define PT_D3D12_STATIC_TRAVERSAL_MODE 0
#endif

#ifndef PT_D3D12_PACKED_TRIANGLES
#define PT_D3D12_PACKED_TRIANGLES 1
#endif

cbuffer PCBuf {
    float  camera_pos_x;  float camera_pos_y;  float camera_pos_z;  float fov_tan_half;
    float  cam_fwd_x;     float cam_fwd_y;     float cam_fwd_z;     float aspect;
    float  cam_right_x;   float cam_right_y;   float cam_right_z;   uint  num_sdfs;
    float  cam_up_x;      float cam_up_y;      float cam_up_z;      uint  sample_index;
    uint   num_insts;     uint   num_mats;      uint   num_lights;   uint  width;
    uint   height;        uint   base_seed;     float  env_r;        float env_g;
    float  env_b;         float  max_depth_f;   uint   rays_per_pixel; float  exposure;
    float  aperture_radius; float focus_distance; uint iris_blade_count; float iris_rotation_radians;
    float  iris_roundness; float anamorphic_squeeze; uint tone_map; uint output_transform;
    float  gamma; uint clamp_output; float white_balance_r; float white_balance_g;
    float  white_balance_b; uint denoiser_enabled; float denoiser_strength; float denoiser_color_sigma;
    uint temporal_enabled; uint temporal_history_valid; float temporal_feedback; float temporal_depth_sigma;
    float temporal_normal_power; float temporal_color_margin; uint static_bvh_node_count; uint dynamic_bvh_node_count;
    float prev_camera_pos_x; float prev_camera_pos_y; float prev_camera_pos_z; float prev_fov_tan_half;
    float prev_cam_fwd_x; float prev_cam_fwd_y; float prev_cam_fwd_z; float prev_aspect;
    float prev_cam_right_x; float prev_cam_right_y; float prev_cam_right_z; float _pad2;
    float prev_cam_up_x; float prev_cam_up_y; float prev_cam_up_z; float _pad3;
};

Buffer<float>    VertBuf  : register(t0);
Buffer<uint>     IndexBuf : register(t1);
Buffer<float>    MatBuf   : register(t2);
Buffer<uint>     InstBuf  : register(t3);
Buffer<float>    LightBuf : register(t4);
Buffer<float>    BvhBuf    : register(t5);
Buffer<uint>     TriMatBuf : register(t6);
Buffer<float>    DynamicBvhBuf : register(t7);
Buffer<float>    LocalBvhBuf : register(t8);
Buffer<float>    SdfBuf : register(t9);
Buffer<float>    TriDataBuf : register(t10);
Buffer<float>    EnvBuf : register(t11);
Buffer<uint>     EnvMetaBuf : register(t12);
Buffer<uint>     TexelBuf : register(t13);
Buffer<uint>     TexMetaBuf : register(t14);
RWByteAddressBuffer FilmBuf : register(u0);
RWBuffer<uint>   LdrBuf   : register(u1); // tonemapped RGBA8 output (R|G<<8|B<<16|0xFF<<24)
RWByteAddressBuffer DenoiseBuf : register(u2); // denoised HDR mean, RGBA32F
RWByteAddressBuffer GuideBuf : register(u3); // albedo/depth + normal/hit, two RGBA32F records per pixel
RWByteAddressBuffer TemporalBuf : register(u4); // temporally accumulated HDR + history length, RGBA32F
RWByteAddressBuffer TemporalHistoryBuf : register(u5); // previous temporal output, RGBA32F
RWByteAddressBuffer PrevGuideBuf : register(u6); // previous guide buffer, two RGBA32F records per pixel

static const uint kInstStride = 24u;
static const uint kPackedTriStride = 18u;
static const uint kInstFlagDynamicTransform = 1u;
static const uint kSdfStride = 16u;
static const uint kSdfShapeSphere = 0u;
static const uint kSdfShapeBox = 1u;
static const uint kSdfShapeRoundedBox = 2u;
static const uint kInvalidTextureIndex = 0xFFFFFFFFu;

// ---- Forward declarations (fxc requires them) -------------------------------
float Halton2(uint idx);
float Halton3(uint idx);
struct Hit { bool ok; float t; float3 pos; float3 n; uint mat; float2 uv; };
uint  Pcg(uint v);
float RandF(inout uint rng);
bool  IntersectTri(float3 ro, float3 rd, uint i0, uint i1, uint i2, bool double_sided, inout float best_t);
bool  IntersectTriAny(float3 ro, float3 rd, uint i0, uint i1, uint i2, bool double_sided, float max_t);
bool  IntersectPackedTri(float3 ro, float3 rd, uint tri, inout float best_t,
                         out uint mat_index, out float3 normal, out float2 uv);
bool  IntersectPackedTriAny(float3 ro, float3 rd, uint tri, float max_t);
bool  MatDoubleSided(uint idx);
bool  IntersectSdfPrimitive(uint sdf_index, float3 ro, float3 rd, inout Hit h);
bool  OccludedSdfPrimitive(uint sdf_index, float3 ro, float3 rd, float max_t);
bool  IsMaterialTransmissive(uint mat_index);
float3 PreviewTransmission(float3 ro, float3 rd, float3 fallback_env);
Hit   IntersectScene(float3 ro, float3 rd);
bool  OccludedScene(float3 ro, float3 rd, float max_t);
float3 SampleHemisphere(float3 n, inout uint rng);
float3 SamplePhongLobe(float3 refl, float exponent, float3 normal, inout uint rng);
void  ApplyCameraLens(inout float3 origin, inout float3 dir, inout uint rng);
float3 SampleSceneEnvironment(float3 rd, float3 fallback_env);
float3 Trace(float3 ro, float3 rd, inout uint rng, float3 env);

float4 LoadFilm(uint pixel) {
    return asfloat(FilmBuf.Load4(pixel * 16u));
}

void StoreFilm(uint pixel, float4 value) {
    FilmBuf.Store4(pixel * 16u, asuint(value));
}

float4 LoadDenoised(uint pixel) {
    return asfloat(DenoiseBuf.Load4(pixel * 16u));
}

void StoreDenoised(uint pixel, float4 value) {
    DenoiseBuf.Store4(pixel * 16u, asuint(value));
}

void StoreGuide(uint pixel, float4 albedo_depth, float4 normal_hit) {
    uint base = pixel * 32u;
    GuideBuf.Store4(base, asuint(albedo_depth));
    GuideBuf.Store4(base + 16u, asuint(normal_hit));
}

float4 LoadGuideAlbedoDepth(uint pixel) {
    return asfloat(GuideBuf.Load4(pixel * 32u));
}

float4 LoadGuideNormalHit(uint pixel) {
    return asfloat(GuideBuf.Load4(pixel * 32u + 16u));
}

float4 LoadTemporal(uint pixel) {
    return asfloat(TemporalBuf.Load4(pixel * 16u));
}

void StoreTemporal(uint pixel, float4 value) {
    TemporalBuf.Store4(pixel * 16u, asuint(value));
}

float4 LoadTemporalHistory(uint pixel) {
    return asfloat(TemporalHistoryBuf.Load4(pixel * 16u));
}

float4 LoadPrevGuideAlbedoDepth(uint pixel) {
    return asfloat(PrevGuideBuf.Load4(pixel * 32u));
}

float4 LoadPrevGuideNormalHit(uint pixel) {
    return asfloat(PrevGuideBuf.Load4(pixel * 32u + 16u));
}

float3 FilmMean(uint pixel) {
    float4 acc = LoadFilm(pixel);
    return acc.xyz / max(1.0f, acc.w);
}

float3 CurrentHdrSource(uint pixel) {
    return (temporal_enabled != 0u) ? LoadTemporal(pixel).xyz : FilmMean(pixel);
}

float Luma(float3 c) {
    return dot(c, float3(0.2126f, 0.7152f, 0.0722f));
}

[numthreads(8, 8, 1)]
void main(uint3 gid : SV_DispatchThreadID) {
    if (gid.x >= width || gid.y >= height) return;

    uint pixel = gid.y * width + gid.x;
    float3 cam_pos  = float3(camera_pos_x, camera_pos_y, camera_pos_z);
    float3 cam_fwd  = float3(cam_fwd_x,    cam_fwd_y,    cam_fwd_z);
    float3 cam_right= float3(cam_right_x,  cam_right_y,  cam_right_z);
    float3 cam_up   = float3(cam_up_x,     cam_up_y,     cam_up_z);
    float3 env_col  = float3(env_r, env_g, env_b);

    uint rpp = max(1u, rays_per_pixel);
    float3 total_color = float3(0.0f, 0.0f, 0.0f);

    for (uint ri = 0u; ri < rpp; ++ri) {
        uint eff_sample = sample_index * rpp + ri;
        uint rng = Pcg(pixel ^ Pcg(eff_sample * 1664525u ^ base_seed) ^ (ri * 2654435761u));
        float jx = Halton2(eff_sample);
        float jy = Halton3(eff_sample);
        float fx = (float(gid.x) + jx) / float(width);
        float fy = (float(gid.y) + jy) / float(height);
        float nx = (2.0f * fx - 1.0f) * aspect * fov_tan_half;
        float ny = (1.0f - 2.0f * fy) * fov_tan_half;
        float3 dir = normalize(cam_fwd + cam_right * nx + cam_up * ny);
        float3 origin = cam_pos;
        ApplyCameraLens(origin, dir, rng);
        total_color += Trace(origin, dir, rng, env_col);
    }

    if (sample_index == 0u) {
        StoreFilm(pixel, float4(total_color.x, total_color.y, total_color.z, float(rpp)));
    } else {
        float4 prev = LoadFilm(pixel);
        StoreFilm(pixel, float4(prev.x + total_color.x, prev.y + total_color.y,
                                prev.z + total_color.z, prev.w + float(rpp)));
    }
}

// ============================================================================
// GPU tonemap pass — second entry point, dispatched after path trace.
// Reads the RGBA32F accumulation buffer (R_sum, G_sum, B_sum, count),
// applies per-sample averaging, Reinhard tonemapping with exposure, sRGB
// gamma 2.2, and writes packed RGBA8 (R | G<<8 | B<<16 | 0xFF<<24) to LdrBuf.
// ============================================================================
[numthreads(8, 8, 1)]
void tonemap_main(uint3 gid : SV_DispatchThreadID) {
    if (gid.x >= width || gid.y >= height) return;
    uint pixel = gid.y * width + gid.x;

    float4 acc = LoadFilm(pixel);
    float3 c = acc.xyz / max(1.0f, acc.w);
    if (temporal_enabled != 0u) {
        c = LoadTemporal(pixel).xyz;
    }
    if (denoiser_enabled != 0u) {
        c = LoadDenoised(pixel).xyz;
    }

    c *= max(0.0f, exposure) * float3(white_balance_r, white_balance_g, white_balance_b);

    if (tone_map == 1u) {
        c = c / (1.0f + c);
    } else if (tone_map == 2u) {
        const float A = 0.15f;
        const float B = 0.50f;
        const float C = 0.10f;
        const float D = 0.20f;
        const float E = 0.02f;
        const float F = 0.30f;
        const float W = 11.2f;
        float3 filmic = ((c * (A * c + C * B) + D * E) / (c * (A * c + B) + D * F)) - E / F;
        float white = ((W * (A * W + C * B) + D * E) / (W * (A * W + B) + D * F)) - E / F;
        c = filmic / max(1.0e-4f, white);
    } else if (tone_map == 3u) {
        c = (c * (2.51f * c + 0.03f)) / (c * (2.43f * c + 0.59f) + 0.14f);
    }

    if (output_transform == 0u) {
        c = pow(max(c, 0.0f), 1.0f / max(0.01f, gamma));
    }
    if (clamp_output != 0u) {
        c = saturate(c);
    }

    float3 packed = saturate(c);
    uint r = min(255u, (uint)(packed.x * 255.0f + 0.5f));
    uint g = min(255u, (uint)(packed.y * 255.0f + 0.5f));
    uint b = min(255u, (uint)(packed.z * 255.0f + 0.5f));
    LdrBuf[pixel] = r | (g << 8u) | (b << 16u) | (0xFFu << 24u);
}

// ============================================================================
// PCG RNG
// ============================================================================
uint Pcg(uint v) {
    uint state = v * 747796405u + 2891336453u;
    uint word  = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}
float RandF(inout uint rng) {
    rng = Pcg(rng);
    return float(rng) / 4294967296.0;
}

void ApplyCameraLens(inout float3 origin, inout float3 dir, inout uint rng) {
    if (aperture_radius <= 0.0f || focus_distance <= 1.0e-4f) return;
    float lens_radius_sample = sqrt(RandF(rng));
    float lens_phi = 6.28318530718f * RandF(rng);
    float aperture_boundary = 1.0f;
    float roundness = saturate(iris_roundness);
    if (iris_blade_count >= 3u && roundness < 0.999f) {
        float blades = float(min(iris_blade_count, 64u));
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

// ============================================================================
// Low-discrepancy Halton sequence
// ============================================================================
float Halton2(uint idx) {
    uint bits = idx;
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10f;
}
float Halton3(uint idx) {
    float result = 0.0f;
    float f = 1.0f / 3.0f;
    uint  i = idx;
    [loop]
    for (uint step = 0u; step < 21u; ++step) {
        if (i == 0u) break;
        result += f * float(i % 3u);
        i /= 3u;
        f *= (1.0f / 3.0f);
    }
    return result;
}

// ============================================================================
// Möller-Trumbore intersection
// ============================================================================
bool IntersectTri(float3 ro, float3 rd, uint i0, uint i1, uint i2, bool double_sided, inout float best_t) {
    float3 v0 = float3(VertBuf[i0*3u],    VertBuf[i0*3u+1u],  VertBuf[i0*3u+2u]);
    float3 v1 = float3(VertBuf[i1*3u],    VertBuf[i1*3u+1u],  VertBuf[i1*3u+2u]);
    float3 v2 = float3(VertBuf[i2*3u],    VertBuf[i2*3u+1u],  VertBuf[i2*3u+2u]);
    float3 e1 = v1 - v0;
    float3 e2 = v2 - v0;
    float3 h  = cross(rd, e2);
    float a   = dot(e1, h);
    // Cornell scene faces are wound inward, so this culls exterior backsides
    // while keeping the interior visible from the orbiting camera.
    if (double_sided) {
        if (abs(a) < 1e-5) return false;
    } else if (a < 1e-5) {
        return false;
    }
    float f = 1.0 / a;
    float3 s = ro - v0;
    float u = f * dot(s, h);
    if (u < 0.0 || u > 1.0) return false;
    float3 q = cross(s, e1);
    float v  = f * dot(rd, q);
    if (v < 0.0 || u + v > 1.0) return false;
    float t = f * dot(e2, q);
    if (t > 1e-4 && t < best_t) { best_t = t; return true; }
    return false;
}

bool IntersectTriAny(float3 ro, float3 rd, uint i0, uint i1, uint i2, bool double_sided, float max_t) {
    float3 v0 = float3(VertBuf[i0*3u],    VertBuf[i0*3u+1u],  VertBuf[i0*3u+2u]);
    float3 v1 = float3(VertBuf[i1*3u],    VertBuf[i1*3u+1u],  VertBuf[i1*3u+2u]);
    float3 v2 = float3(VertBuf[i2*3u],    VertBuf[i2*3u+1u],  VertBuf[i2*3u+2u]);
    float3 e1 = v1 - v0;
    float3 e2 = v2 - v0;
    float3 h  = cross(rd, e2);
    float a   = dot(e1, h);
    if (double_sided) {
        if (abs(a) < 1e-5) return false;
    } else if (a < 1e-5) {
        return false;
    }
    float f = 1.0 / a;
    float3 s = ro - v0;
    float u = f * dot(s, h);
    if (u < 0.0 || u > 1.0) return false;
    float3 q = cross(s, e1);
    float v  = f * dot(rd, q);
    if (v < 0.0 || u + v > 1.0) return false;
    float t = f * dot(e2, q);
    return t > 1e-4 && t < max_t;
}

void LoadPackedTriGeometry(uint b, out float3 v0, out float3 e1, out float3 e2,
                           out bool double_sided) {
    v0 = float3(TriDataBuf[b + 0u], TriDataBuf[b + 1u], TriDataBuf[b + 2u]);
    e1 = float3(TriDataBuf[b + 4u], TriDataBuf[b + 5u], TriDataBuf[b + 6u]);
    double_sided = TriDataBuf[b + 7u] > 0.5f;
    e2 = float3(TriDataBuf[b + 8u], TriDataBuf[b + 9u], TriDataBuf[b + 10u]);
}

bool IntersectPackedTri(float3 ro, float3 rd, uint tri, inout float best_t,
                        out uint mat_index, out float3 normal, out float2 uv) {
    uint b = tri * kPackedTriStride;
    float3 v0;
    float3 e1;
    float3 e2;
    bool double_sided;
    LoadPackedTriGeometry(b, v0, e1, e2, double_sided);

    float3 h = cross(rd, e2);
    float a = dot(e1, h);
    if (double_sided) {
        if (abs(a) < 1e-5f) return false;
    } else if (a < 1e-5f) {
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
    if (t <= 1e-4f || t >= best_t) return false;

    best_t = t;
    mat_index = uint(TriDataBuf[b + 3u] + 0.5f);
    normal = normalize(cross(e1, e2));
    if (double_sided && dot(normal, rd) > 0.0f) {
        normal = -normal;
    }
    float2 uv0 = float2(TriDataBuf[b + 12u], TriDataBuf[b + 13u]);
    float2 uv1 = float2(TriDataBuf[b + 14u], TriDataBuf[b + 15u]);
    float2 uv2 = float2(TriDataBuf[b + 16u], TriDataBuf[b + 17u]);
    float baryZ = 1.0f - u - v;
    uv = uv0 * baryZ + uv1 * u + uv2 * v;
    return true;
}

bool IntersectPackedTriAny(float3 ro, float3 rd, uint tri, float max_t) {
    uint b = tri * kPackedTriStride;
    float3 v0;
    float3 e1;
    float3 e2;
    bool double_sided;
    LoadPackedTriGeometry(b, v0, e1, e2, double_sided);

    float3 h = cross(rd, e2);
    float a = dot(e1, h);
    if (double_sided) {
        if (abs(a) < 1e-5f) return false;
    } else if (a < 1e-5f) {
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
    return t > 1e-4f && t < max_t;
}

float3 SafeInstanceScale(uint ib) {
    float3 s = float3(asfloat(InstBuf[ib + 12u]), asfloat(InstBuf[ib + 13u]), asfloat(InstBuf[ib + 14u]));
    s.x = (abs(s.x) < 1e-6f) ? 1.0f : s.x;
    s.y = (abs(s.y) < 1e-6f) ? 1.0f : s.y;
    s.z = (abs(s.z) < 1e-6f) ? 1.0f : s.z;
    return s;
}

float4 InstanceRotation(uint ib) {
    float4 q = float4(asfloat(InstBuf[ib + 8u]), asfloat(InstBuf[ib + 9u]),
                      asfloat(InstBuf[ib + 10u]), asfloat(InstBuf[ib + 11u]));
    float len2 = max(1e-12f, dot(q, q));
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

float3 InstanceLocalToWorldPoint(uint ib, float3 p) {
    float3 t = float3(asfloat(InstBuf[ib + 4u]), asfloat(InstBuf[ib + 5u]), asfloat(InstBuf[ib + 6u]));
    return RotateQuat(p * SafeInstanceScale(ib), InstanceRotation(ib)) + t;
}

float3 InstanceLocalNormalToWorld(uint ib, float3 n) {
    return normalize(RotateQuat(n / SafeInstanceScale(ib), InstanceRotation(ib)));
}

bool IntersectLocalInstanceBounds(uint ib, float3 ro_local, float3 rd_local, float max_t) {
    float3 bmin = float3(asfloat(InstBuf[ib + 16u]), asfloat(InstBuf[ib + 17u]), asfloat(InstBuf[ib + 18u]));
    float3 bmax = float3(asfloat(InstBuf[ib + 20u]), asfloat(InstBuf[ib + 21u]), asfloat(InstBuf[ib + 22u]));
    float3 inv_rd = float3(
        abs(rd_local.x) > 1e-8f ? 1.0f / rd_local.x : 1.0e30f,
        abs(rd_local.y) > 1e-8f ? 1.0f / rd_local.y : 1.0e30f,
        abs(rd_local.z) > 1e-8f ? 1.0f / rd_local.z : 1.0e30f);
    float3 t0 = (bmin - ro_local) * inv_rd;
    float3 t1 = (bmax - ro_local) * inv_rd;
    float tenter = max(max(min(t0.x, t1.x), min(t0.y, t1.y)), min(t0.z, t1.z));
    float texit  = min(min(max(t0.x, t1.x), max(t0.y, t1.y)), max(t0.z, t1.z));
    return texit >= 1e-4f && tenter <= texit && tenter <= max_t;
}

bool IntersectDynamicTri(uint ib, float3 ro_world, float3 rd_world,
                         float3 ro_local, float3 rd_local,
                         uint i0, uint i1, uint i2, bool double_sided,
                         inout float best_t, out float3 out_pos, out float3 out_n) {
    float3 v0 = float3(VertBuf[i0*3u],    VertBuf[i0*3u+1u],  VertBuf[i0*3u+2u]);
    float3 v1 = float3(VertBuf[i1*3u],    VertBuf[i1*3u+1u],  VertBuf[i1*3u+2u]);
    float3 v2 = float3(VertBuf[i2*3u],    VertBuf[i2*3u+1u],  VertBuf[i2*3u+2u]);
    float3 e1 = v1 - v0;
    float3 e2 = v2 - v0;
    float3 h  = cross(rd_local, e2);
    float a   = dot(e1, h);
    if (double_sided) {
        if (abs(a) < 1e-5) return false;
    } else if (a < 1e-5) {
        return false;
    }
    float f = 1.0 / a;
    float3 s = ro_local - v0;
    float u = f * dot(s, h);
    if (u < 0.0 || u > 1.0) return false;
    float3 q = cross(s, e1);
    float v = f * dot(rd_local, q);
    if (v < 0.0 || u + v > 1.0) return false;
    float t = f * dot(e2, q);
    if (t <= 1e-4f || t >= best_t) return false;

    float3 local_pos = ro_local + rd_local * t;
    float3 world_pos = InstanceLocalToWorldPoint(ib, local_pos);
    float world_t = dot(world_pos - ro_world, rd_world);
    if (world_t <= 1e-4f || world_t >= best_t) return false;

    float3 world_n = InstanceLocalNormalToWorld(ib, normalize(cross(e1, e2)));
    if (double_sided && dot(world_n, rd_world) > 0.0f) {
        world_n = -world_n;
    }
    best_t = world_t;
    out_pos = world_pos;
    out_n = world_n;
    return true;
}

bool IntersectDynamicPackedTri(uint ib, float3 ro_world, float3 rd_world,
                               float3 ro_local, float3 rd_local,
                               uint tri, bool double_sided,
                               inout float best_t, out float3 out_pos, out float3 out_n, out float2 out_uv) {
    uint b = tri * kPackedTriStride;
    float3 v0;
    float3 e1;
    float3 e2;
    bool packed_double_sided;
    LoadPackedTriGeometry(b, v0, e1, e2, packed_double_sided);

    float3 h = cross(rd_local, e2);
    float a = dot(e1, h);
    if (double_sided) {
        if (abs(a) < 1e-5f) return false;
    } else if (a < 1e-5f) {
        return false;
    }
    float f = 1.0f / a;
    float3 s = ro_local - v0;
    float u = f * dot(s, h);
    if (u < 0.0f || u > 1.0f) return false;
    float3 q = cross(s, e1);
    float v = f * dot(rd_local, q);
    if (v < 0.0f || u + v > 1.0f) return false;
    float t = f * dot(e2, q);
    if (t <= 1e-4f || t >= best_t) return false;

    float3 local_pos = ro_local + rd_local * t;
    float3 world_pos = InstanceLocalToWorldPoint(ib, local_pos);
    float world_t = dot(world_pos - ro_world, rd_world);
    if (world_t <= 1e-4f || world_t >= best_t) return false;

    float3 world_n = InstanceLocalNormalToWorld(ib, normalize(cross(e1, e2)));
    if (double_sided && dot(world_n, rd_world) > 0.0f) {
        world_n = -world_n;
    }
    best_t = world_t;
    out_pos = world_pos;
    out_n = world_n;
    float2 uv0 = float2(TriDataBuf[b + 12u], TriDataBuf[b + 13u]);
    float2 uv1 = float2(TriDataBuf[b + 14u], TriDataBuf[b + 15u]);
    float2 uv2 = float2(TriDataBuf[b + 16u], TriDataBuf[b + 17u]);
    float baryZ = 1.0f - u - v;
    out_uv = uv0 * baryZ + uv1 * u + uv2 * v;
    return true;
}

bool IntersectDynamicPackedTriAny(uint ib, float3 ro_world, float3 rd_world,
                                  float3 ro_local, float3 rd_local,
                                  uint tri, bool double_sided,
                                  inout float best_t) {
    uint b = tri * kPackedTriStride;
    float3 v0;
    float3 e1;
    float3 e2;
    bool packed_double_sided;
    LoadPackedTriGeometry(b, v0, e1, e2, packed_double_sided);

    float3 h = cross(rd_local, e2);
    float a = dot(e1, h);
    if (double_sided) {
        if (abs(a) < 1e-5f) return false;
    } else if (a < 1e-5f) {
        return false;
    }
    float f = 1.0f / a;
    float3 s = ro_local - v0;
    float u = f * dot(s, h);
    if (u < 0.0f || u > 1.0f) return false;
    float3 q = cross(s, e1);
    float v = f * dot(rd_local, q);
    if (v < 0.0f || u + v > 1.0f) return false;
    float t = f * dot(e2, q);
    if (t <= 1e-4f || t >= best_t) return false;

    float3 local_pos = ro_local + rd_local * t;
    float3 world_pos = InstanceLocalToWorldPoint(ib, local_pos);
    float world_t = dot(world_pos - ro_world, rd_world);
    if (world_t <= 1e-4f || world_t >= best_t) return false;

    best_t = world_t;
    return true;
}

bool IntersectWorldBounds(float3 bmin, float3 bmax, float3 ro, float3 inv_rd, float max_t) {
    float3 t0 = (bmin - ro) * inv_rd;
    float3 t1 = (bmax - ro) * inv_rd;
    float tenter = max(max(min(t0.x, t1.x), min(t0.y, t1.y)), min(t0.z, t1.z));
    float texit  = min(min(max(t0.x, t1.x), max(t0.y, t1.y)), max(t0.z, t1.z));
    return texit >= 1e-4f && tenter <= texit && tenter <= max_t;
}

bool IntersectWorldBoundsT(float3 bmin, float3 bmax, float3 ro, float3 inv_rd, float max_t, out float enter_t) {
    float3 t0 = (bmin - ro) * inv_rd;
    float3 t1 = (bmax - ro) * inv_rd;
    float tenter = max(max(min(t0.x, t1.x), min(t0.y, t1.y)), min(t0.z, t1.z));
    float texit  = min(min(max(t0.x, t1.x), max(t0.y, t1.y)), max(t0.z, t1.z));
    enter_t = tenter;
    return texit >= 1e-4f && tenter <= texit && tenter <= max_t;
}

void IntersectDynamicInstance(uint instance_index, float3 ro, float3 rd, inout Hit h) {
    uint ib = instance_index * kInstStride;
    uint flags = InstBuf[ib + 3u];
    if ((flags & kInstFlagDynamicTransform) == 0u) return;
    uint ftri = InstBuf[ib + 0u];
    uint ntri = InstBuf[ib + 1u];
    uint mat_index = InstBuf[ib + 2u];
    uint local_bvh_first = InstBuf[ib + 7u];
    uint local_bvh_count = InstBuf[ib + 15u];
    bool double_sided = MatDoubleSided(mat_index);
    float3 ro_local = InstanceWorldToLocalPoint(ib, ro);
    float3 rd_local = InstanceWorldToLocalVector(ib, rd);
    if (!IntersectLocalInstanceBounds(ib, ro_local, rd_local, 1e30f)) return;
    if (local_bvh_count > 0u) {
        float3 inv_local_rd = float3(
            abs(rd_local.x) > 1e-8f ? 1.0f / rd_local.x : 1.0e30f,
            abs(rd_local.y) > 1e-8f ? 1.0f / rd_local.y : 1.0e30f,
            abs(rd_local.z) > 1e-8f ? 1.0f / rd_local.z : 1.0e30f);
        uint stack[64];
        uint sp = 0u;
        stack[sp++] = local_bvh_first;
        uint local_bvh_end = local_bvh_first + local_bvh_count;
        [loop]
        while (sp > 0u) {
            uint ni = stack[--sp];
            if (ni < local_bvh_first || ni >= local_bvh_end) continue;
            uint nb = ni * 8u;
            float3 bmin = float3(LocalBvhBuf[nb+0u], LocalBvhBuf[nb+1u], LocalBvhBuf[nb+2u]);
            float3 bmax = float3(LocalBvhBuf[nb+3u], LocalBvhBuf[nb+4u], LocalBvhBuf[nb+5u]);
            if (!IntersectWorldBounds(bmin, bmax, ro_local, inv_local_rd, 1e30f)) continue;
            uint lf = asuint(LocalBvhBuf[nb+6u]);
            uint rc = asuint(LocalBvhBuf[nb+7u]);
            bool is_leaf = (lf & 0x80000000u) != 0u;
            if (is_leaf) {
                uint first_tri = lf & 0x7FFFFFFFu;
                for (uint ti = 0u; ti < rc; ++ti) {
                    float3 pos;
                    float3 normal;
                    float2 uv = float2(0.0f, 0.0f);
#if PT_D3D12_PACKED_TRIANGLES
                    if (IntersectDynamicPackedTri(ib, ro, rd, ro_local, rd_local,
                                                  first_tri + ti, double_sided, h.t, pos, normal, uv)) {
#else
                    uint tb = (first_tri + ti) * 3u;
                    if (IntersectDynamicTri(ib, ro, rd, ro_local, rd_local,
                                            IndexBuf[tb], IndexBuf[tb + 1u], IndexBuf[tb + 2u],
                                            double_sided, h.t, pos, normal)) {
#endif
                        h.ok = true;
                        h.pos = pos;
                        h.n = normal;
                        h.mat = mat_index;
                        h.uv = uv;
                    }
                }
            } else if (sp + 2u <= 64u) {
                stack[sp++] = rc;
                stack[sp++] = lf;
            }
        }
        return;
    }
    for (uint ti = 0u; ti < ntri; ++ti) {
        float3 pos;
        float3 normal;
        float2 uv = float2(0.0f, 0.0f);
#if PT_D3D12_PACKED_TRIANGLES
        if (IntersectDynamicPackedTri(ib, ro, rd, ro_local, rd_local,
                                      ftri + ti, double_sided, h.t, pos, normal, uv)) {
#else
        uint tb = (ftri + ti) * 3u;
        if (IntersectDynamicTri(ib, ro, rd, ro_local, rd_local,
                                IndexBuf[tb], IndexBuf[tb + 1u], IndexBuf[tb + 2u],
                                double_sided, h.t, pos, normal)) {
#endif
            h.ok = true;
            h.pos = pos;
            h.n = normal;
            h.mat = mat_index;
            h.uv = uv;
        }
    }
}

bool OccludedDynamicInstance(uint instance_index, float3 ro, float3 rd, float max_t) {
    uint ib = instance_index * kInstStride;
    uint flags = InstBuf[ib + 3u];
    if ((flags & kInstFlagDynamicTransform) == 0u) return false;
    uint ftri = InstBuf[ib + 0u];
    uint ntri = InstBuf[ib + 1u];
    uint mat_index = InstBuf[ib + 2u];
    uint local_bvh_first = InstBuf[ib + 7u];
    uint local_bvh_count = InstBuf[ib + 15u];
    bool double_sided = MatDoubleSided(mat_index);
    float3 ro_local = InstanceWorldToLocalPoint(ib, ro);
    float3 rd_local = InstanceWorldToLocalVector(ib, rd);
    if (!IntersectLocalInstanceBounds(ib, ro_local, rd_local, 1e30f)) return false;
    if (local_bvh_count > 0u) {
        float3 inv_local_rd = float3(
            abs(rd_local.x) > 1e-8f ? 1.0f / rd_local.x : 1.0e30f,
            abs(rd_local.y) > 1e-8f ? 1.0f / rd_local.y : 1.0e30f,
            abs(rd_local.z) > 1e-8f ? 1.0f / rd_local.z : 1.0e30f);
        uint stack[64];
        uint sp = 0u;
        stack[sp++] = local_bvh_first;
        uint local_bvh_end = local_bvh_first + local_bvh_count;
        float best = max_t;
        [loop]
        while (sp > 0u) {
            uint ni = stack[--sp];
            if (ni < local_bvh_first || ni >= local_bvh_end) continue;
            uint nb = ni * 8u;
            float3 bmin = float3(LocalBvhBuf[nb+0u], LocalBvhBuf[nb+1u], LocalBvhBuf[nb+2u]);
            float3 bmax = float3(LocalBvhBuf[nb+3u], LocalBvhBuf[nb+4u], LocalBvhBuf[nb+5u]);
            if (!IntersectWorldBounds(bmin, bmax, ro_local, inv_local_rd, 1e30f)) continue;
            uint lf = asuint(LocalBvhBuf[nb+6u]);
            uint rc = asuint(LocalBvhBuf[nb+7u]);
            bool is_leaf = (lf & 0x80000000u) != 0u;
            if (is_leaf) {
                uint first_tri = lf & 0x7FFFFFFFu;
                for (uint ti = 0u; ti < rc; ++ti) {
#if PT_D3D12_PACKED_TRIANGLES
                    if (IntersectDynamicPackedTriAny(ib, ro, rd, ro_local, rd_local,
                                                     first_tri + ti, double_sided, best)) {
#else
                    float3 pos;
                    float3 normal;
                    uint tb = (first_tri + ti) * 3u;
                    if (IntersectDynamicTri(ib, ro, rd, ro_local, rd_local,
                                            IndexBuf[tb], IndexBuf[tb + 1u], IndexBuf[tb + 2u],
                                            double_sided, best, pos, normal)) {
#endif
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
    float best = max_t;
    for (uint ti = 0u; ti < ntri; ++ti) {
#if PT_D3D12_PACKED_TRIANGLES
        if (IntersectDynamicPackedTriAny(ib, ro, rd, ro_local, rd_local,
                                         ftri + ti, double_sided, best)) {
#else
        float3 pos;
        float3 normal;
        uint tb = (ftri + ti) * 3u;
        if (IntersectDynamicTri(ib, ro, rd, ro_local, rd_local,
                                IndexBuf[tb], IndexBuf[tb + 1u], IndexBuf[tb + 2u],
                                double_sided, best, pos, normal)) {
#endif
            return true;
        }
    }
    return false;
}

bool IntersectSdfSphere(uint sdf_index, float3 ro, float3 rd, inout Hit h) {
    uint base = sdf_index * kSdfStride;
    uint shape = uint(SdfBuf[base + 0u] + 0.5f);
    if (shape != kSdfShapeSphere) return false;

    float3 center = float3(SdfBuf[base + 4u], SdfBuf[base + 5u], SdfBuf[base + 6u]);
    float3 scale = abs(float3(SdfBuf[base + 8u], SdfBuf[base + 9u], SdfBuf[base + 10u]));
    float scale_max = max(scale.x, max(scale.y, scale.z));
    float radius = max(0.001f, SdfBuf[base + 2u] * max(scale_max, 0.001f));

    float3 oc = ro - center;
    float half_b = dot(oc, rd);
    float c = dot(oc, oc) - radius * radius;
    float discriminant = half_b * half_b - c;
    if (discriminant < 0.0f) return false;

    float root = sqrt(discriminant);
    float t = -half_b - root;
    if (t <= 1e-4f) {
        t = -half_b + root;
    }
    if (t <= 1e-4f || t >= h.t) return false;

    h.ok = true;
    h.t = t;
    h.pos = ro + rd * t;
    h.n = normalize(h.pos - center);
    h.mat = uint(SdfBuf[base + 1u] + 0.5f);
    return true;
}

float3 SdfBoxNormal(float3 local, float3 half_extents) {
    float3 face_dist = abs(half_extents - abs(local));
    if (face_dist.x <= face_dist.y && face_dist.x <= face_dist.z) {
        return float3(local.x >= 0.0f ? 1.0f : -1.0f, 0.0f, 0.0f);
    }
    if (face_dist.y <= face_dist.z) {
        return float3(0.0f, local.y >= 0.0f ? 1.0f : -1.0f, 0.0f);
    }
    return float3(0.0f, 0.0f, local.z >= 0.0f ? 1.0f : -1.0f);
}

bool IntersectSdfBox(uint sdf_index, float3 ro, float3 rd, inout Hit h) {
    uint base = sdf_index * kSdfStride;
    uint shape = uint(SdfBuf[base + 0u] + 0.5f);
    if (shape != kSdfShapeBox && shape != kSdfShapeRoundedBox) return false;

    float3 center = float3(SdfBuf[base + 4u], SdfBuf[base + 5u], SdfBuf[base + 6u]);
    float3 half_extents = max(abs(float3(SdfBuf[base + 8u], SdfBuf[base + 9u], SdfBuf[base + 10u])),
                              float3(0.001f, 0.001f, 0.001f));
    float3 bmin = center - half_extents;
    float3 bmax = center + half_extents;
    float3 inv_rd = 1.0f / rd;
    float3 t0 = (bmin - ro) * inv_rd;
    float3 t1 = (bmax - ro) * inv_rd;
    float3 tsm = min(t0, t1);
    float3 tbg = max(t0, t1);
    float t_near = max(tsm.x, max(tsm.y, tsm.z));
    float t_far = min(tbg.x, min(tbg.y, tbg.z));
    if (t_far <= 1.0e-4f || t_near > t_far) return false;

    float t = (t_near > 1.0e-4f) ? t_near : t_far;
    if (t <= 1.0e-4f || t >= h.t) return false;

    h.ok = true;
    h.t = t;
    h.pos = ro + rd * t;
    h.n = SdfBoxNormal(h.pos - center, half_extents);
    h.mat = uint(SdfBuf[base + 1u] + 0.5f);
    return true;
}

bool IntersectSdfPrimitive(uint sdf_index, float3 ro, float3 rd, inout Hit h) {
    uint base = sdf_index * kSdfStride;
    uint shape = uint(SdfBuf[base + 0u] + 0.5f);
    if (shape == kSdfShapeSphere) return IntersectSdfSphere(sdf_index, ro, rd, h);
    if (shape == kSdfShapeBox || shape == kSdfShapeRoundedBox) return IntersectSdfBox(sdf_index, ro, rd, h);
    return false;
}

bool OccludedSdfSphere(uint sdf_index, float3 ro, float3 rd, float max_t) {
    uint base = sdf_index * kSdfStride;
    uint shape = uint(SdfBuf[base + 0u] + 0.5f);
    if (shape != kSdfShapeSphere) return false;

    float3 center = float3(SdfBuf[base + 4u], SdfBuf[base + 5u], SdfBuf[base + 6u]);
    float3 scale = abs(float3(SdfBuf[base + 8u], SdfBuf[base + 9u], SdfBuf[base + 10u]));
    float scale_max = max(scale.x, max(scale.y, scale.z));
    float radius = max(0.001f, SdfBuf[base + 2u] * max(scale_max, 0.001f));

    float3 oc = ro - center;
    float half_b = dot(oc, rd);
    float c = dot(oc, oc) - radius * radius;
    float discriminant = half_b * half_b - c;
    if (discriminant < 0.0f) return false;

    float root = sqrt(discriminant);
    float t = -half_b - root;
    if (t <= 1e-4f) {
        t = -half_b + root;
    }
    return t > 1e-4f && t < max_t;
}

bool OccludedSdfBox(uint sdf_index, float3 ro, float3 rd, float max_t) {
    uint base = sdf_index * kSdfStride;
    uint shape = uint(SdfBuf[base + 0u] + 0.5f);
    if (shape != kSdfShapeBox && shape != kSdfShapeRoundedBox) return false;

    float3 center = float3(SdfBuf[base + 4u], SdfBuf[base + 5u], SdfBuf[base + 6u]);
    float3 half_extents = max(abs(float3(SdfBuf[base + 8u], SdfBuf[base + 9u], SdfBuf[base + 10u])),
                              float3(0.001f, 0.001f, 0.001f));
    float3 bmin = center - half_extents;
    float3 bmax = center + half_extents;
    float3 inv_rd = 1.0f / rd;
    float3 t0 = (bmin - ro) * inv_rd;
    float3 t1 = (bmax - ro) * inv_rd;
    float3 tsm = min(t0, t1);
    float3 tbg = max(t0, t1);
    float t_near = max(tsm.x, max(tsm.y, tsm.z));
    float t_far = min(tbg.x, min(tbg.y, tbg.z));
    if (t_far <= 1.0e-4f || t_near > t_far) return false;
    float t = (t_near > 1.0e-4f) ? t_near : t_far;
    return t > 1.0e-4f && t < max_t;
}

bool OccludedSdfPrimitive(uint sdf_index, float3 ro, float3 rd, float max_t) {
    uint base = sdf_index * kSdfStride;
    uint shape = uint(SdfBuf[base + 0u] + 0.5f);
    uint mat_index = uint(SdfBuf[base + 1u] + 0.5f);
    if (IsMaterialTransmissive(mat_index)) {
        return false;
    }
    if (shape == kSdfShapeSphere) return OccludedSdfSphere(sdf_index, ro, rd, max_t);
    if (shape == kSdfShapeBox || shape == kSdfShapeRoundedBox) return OccludedSdfBox(sdf_index, ro, rd, max_t);
    return false;
}

// ============================================================================
// Scene hit
// ============================================================================
Hit IntersectScene(float3 ro, float3 rd) {
    Hit h;
    h.ok  = false;
    h.t   = 1e30f;
    h.pos = float3(0.0f, 0.0f, 0.0f);
    h.n   = float3(0.0f, 1.0f, 0.0f);
    h.mat = 0u;
    h.uv  = float2(0.0f, 0.0f);

    float3 inv_rd = float3(1.0f / rd.x, 1.0f / rd.y, 1.0f / rd.z);

    uint stack[128];
    uint sp = 0u;
    stack[sp++] = 0u; // root node index

    [loop]
    while (sp > 0u) {
        uint ni = stack[--sp];
        uint nb = ni * 8u;

        float3 bmin = float3(BvhBuf[nb+0u], BvhBuf[nb+1u], BvhBuf[nb+2u]);
        float3 bmax = float3(BvhBuf[nb+3u], BvhBuf[nb+4u], BvhBuf[nb+5u]);

#if PT_D3D12_STATIC_TRAVERSAL_MODE == 1 || PT_D3D12_STATIC_TRAVERSAL_MODE == 2
        if (!IntersectWorldBounds(bmin, bmax, ro, inv_rd, h.t)) continue;
#else
        // Slab AABB intersection test
        float3 t0 = (bmin - ro) * inv_rd;
        float3 t1 = (bmax - ro) * inv_rd;
        float tenter = max(max(min(t0.x, t1.x), min(t0.y, t1.y)), min(t0.z, t1.z));
        float texit  = min(min(max(t0.x, t1.x), max(t0.y, t1.y)), max(t0.z, t1.z));
        if (texit < 1e-4f || tenter > texit || tenter > h.t) continue;
#endif

        uint lf = asuint(BvhBuf[nb+6u]);
        uint rc = asuint(BvhBuf[nb+7u]);
        bool is_leaf = (lf & 0x80000000u) != 0u;

        if (is_leaf) {
            uint first_tri = lf & 0x7FFFFFFFu;
            uint ntri = rc;
            for (uint ti = 0u; ti < ntri; ++ti) {
#if PT_D3D12_PACKED_TRIANGLES
                uint mat_index;
                float3 normal;
                float2 uv;
                float t_best = h.t;
                if (IntersectPackedTri(ro, rd, first_tri + ti, t_best, mat_index, normal, uv)) {
                    h.ok = true;
                    h.t = t_best;
                    h.pos = ro + rd * h.t;
                    h.n = normal;
                    h.mat = mat_index;
                    h.uv = uv;
                }
#else
                uint tb = (first_tri + ti) * 3u;
                uint i0 = IndexBuf[tb];
                uint i1 = IndexBuf[tb + 1u];
                uint i2 = IndexBuf[tb + 2u];
                uint mat_index = TriMatBuf[first_tri + ti];
                bool double_sided = MatDoubleSided(mat_index);
                float t_best = h.t;
                if (IntersectTri(ro, rd, i0, i1, i2, double_sided, t_best)) {
                    h.ok  = true;
                    h.t   = t_best;
                    h.pos = ro + rd * h.t;
                    float3 v0 = float3(VertBuf[i0*3u], VertBuf[i0*3u+1u], VertBuf[i0*3u+2u]);
                    float3 v1 = float3(VertBuf[i1*3u], VertBuf[i1*3u+1u], VertBuf[i1*3u+2u]);
                    float3 v2 = float3(VertBuf[i2*3u], VertBuf[i2*3u+1u], VertBuf[i2*3u+2u]);
                    h.n   = normalize(cross(v1 - v0, v2 - v0));
                    if (double_sided && dot(h.n, rd) > 0.0) {
                        h.n = -h.n;
                    }
                    h.mat = mat_index;
                    h.uv = float2(0.0f, 0.0f);
                }
#endif
            }
        } else {
#if PT_D3D12_STATIC_TRAVERSAL_MODE == 2
            uint lb = lf * 8u;
            uint rb = rc * 8u;
            float left_t = 1e30f;
            float right_t = 1e30f;
            bool left_hit = IntersectWorldBoundsT(
                float3(BvhBuf[lb+0u], BvhBuf[lb+1u], BvhBuf[lb+2u]),
                float3(BvhBuf[lb+3u], BvhBuf[lb+4u], BvhBuf[lb+5u]),
                ro, inv_rd, h.t, left_t);
            bool right_hit = IntersectWorldBoundsT(
                float3(BvhBuf[rb+0u], BvhBuf[rb+1u], BvhBuf[rb+2u]),
                float3(BvhBuf[rb+3u], BvhBuf[rb+4u], BvhBuf[rb+5u]),
                ro, inv_rd, h.t, right_t);
            if (left_hit && right_hit) {
                if (left_t <= right_t) {
                    if (sp + 2u <= 128u) {
                        stack[sp++] = rc;
                        stack[sp++] = lf;
                    }
                } else {
                    if (sp + 2u <= 128u) {
                        stack[sp++] = lf;
                        stack[sp++] = rc;
                    }
                }
            } else if (left_hit) {
                if (sp < 128u) stack[sp++] = lf;
            } else if (right_hit) {
                if (sp < 128u) stack[sp++] = rc;
            }
#else
            // Push right child first so left is processed first (DFS)
            if (sp + 2u <= 128u) {
                stack[sp++] = rc;
                stack[sp++] = lf;
            }
#endif
        }
    }

    sp = 0u;
    stack[sp++] = 0u;
    [loop]
    while (sp > 0u) {
        uint ni = stack[--sp];
        uint nb = ni * 8u;
        float3 bmin = float3(DynamicBvhBuf[nb+0u], DynamicBvhBuf[nb+1u], DynamicBvhBuf[nb+2u]);
        float3 bmax = float3(DynamicBvhBuf[nb+3u], DynamicBvhBuf[nb+4u], DynamicBvhBuf[nb+5u]);
        if (!IntersectWorldBounds(bmin, bmax, ro, inv_rd, h.t)) continue;
        uint lf = asuint(DynamicBvhBuf[nb+6u]);
        uint rc = asuint(DynamicBvhBuf[nb+7u]);
        bool is_leaf = (lf & 0x80000000u) != 0u;
        if (is_leaf) {
            if (rc == 0u) continue;
            uint instance_index = lf & 0x7FFFFFFFu;
            IntersectDynamicInstance(instance_index, ro, rd, h);
        } else {
            if (sp + 2u <= 128u) {
                stack[sp++] = rc;
                stack[sp++] = lf;
            }
        }
    }

    [loop]
    for (uint si = 0u; si < num_sdfs; ++si) {
        IntersectSdfPrimitive(si, ro, rd, h);
    }
    return h;
}

bool OccludedScene(float3 ro, float3 rd, float max_t) {
    if (max_t <= 1e-4f) return false;

    float3 inv_rd = float3(1.0f / rd.x, 1.0f / rd.y, 1.0f / rd.z);
    uint stack[128];
    uint sp = 0u;
    stack[sp++] = 0u;

    [loop]
    while (sp > 0u) {
        uint ni = stack[--sp];
        uint nb = ni * 8u;

        float3 bmin = float3(BvhBuf[nb+0u], BvhBuf[nb+1u], BvhBuf[nb+2u]);
        float3 bmax = float3(BvhBuf[nb+3u], BvhBuf[nb+4u], BvhBuf[nb+5u]);
#if PT_D3D12_STATIC_TRAVERSAL_MODE == 1 || PT_D3D12_STATIC_TRAVERSAL_MODE == 2
        if (!IntersectWorldBounds(bmin, bmax, ro, inv_rd, max_t)) continue;
#else
        float3 t0 = (bmin - ro) * inv_rd;
        float3 t1 = (bmax - ro) * inv_rd;
        float tenter = max(max(min(t0.x, t1.x), min(t0.y, t1.y)), min(t0.z, t1.z));
        float texit  = min(min(max(t0.x, t1.x), max(t0.y, t1.y)), max(t0.z, t1.z));
        if (texit < 1e-4f || tenter > texit || tenter > max_t) continue;
#endif

        uint lf = asuint(BvhBuf[nb+6u]);
        uint rc = asuint(BvhBuf[nb+7u]);
        bool is_leaf = (lf & 0x80000000u) != 0u;
        if (is_leaf) {
            uint first_tri = lf & 0x7FFFFFFFu;
            uint ntri = rc;
            for (uint ti = 0u; ti < ntri; ++ti) {
#if PT_D3D12_PACKED_TRIANGLES
                if (IntersectPackedTriAny(ro, rd, first_tri + ti, max_t)) {
                    return true;
                }
#else
                uint tb = (first_tri + ti) * 3u;
                uint mat_index = TriMatBuf[first_tri + ti];
                if (IntersectTriAny(ro, rd, IndexBuf[tb], IndexBuf[tb + 1u], IndexBuf[tb + 2u],
                                    MatDoubleSided(mat_index), max_t)) {
                    return true;
                }
#endif
            }
        } else {
#if PT_D3D12_STATIC_TRAVERSAL_MODE == 2
            uint lb = lf * 8u;
            uint rb = rc * 8u;
            float left_t = 1e30f;
            float right_t = 1e30f;
            bool left_hit = IntersectWorldBoundsT(
                float3(BvhBuf[lb+0u], BvhBuf[lb+1u], BvhBuf[lb+2u]),
                float3(BvhBuf[lb+3u], BvhBuf[lb+4u], BvhBuf[lb+5u]),
                ro, inv_rd, max_t, left_t);
            bool right_hit = IntersectWorldBoundsT(
                float3(BvhBuf[rb+0u], BvhBuf[rb+1u], BvhBuf[rb+2u]),
                float3(BvhBuf[rb+3u], BvhBuf[rb+4u], BvhBuf[rb+5u]),
                ro, inv_rd, max_t, right_t);
            if (left_hit && right_hit) {
                if (left_t <= right_t) {
                    if (sp + 2u <= 128u) {
                        stack[sp++] = rc;
                        stack[sp++] = lf;
                    }
                } else {
                    if (sp + 2u <= 128u) {
                        stack[sp++] = lf;
                        stack[sp++] = rc;
                    }
                }
            } else if (left_hit) {
                if (sp < 128u) stack[sp++] = lf;
            } else if (right_hit) {
                if (sp < 128u) stack[sp++] = rc;
            }
#else
            if (sp + 2u <= 128u) {
                stack[sp++] = rc;
                stack[sp++] = lf;
            }
#endif
        }
    }

    sp = 0u;
    stack[sp++] = 0u;
    [loop]
    while (sp > 0u) {
        uint ni = stack[--sp];
        uint nb = ni * 8u;
        float3 bmin = float3(DynamicBvhBuf[nb+0u], DynamicBvhBuf[nb+1u], DynamicBvhBuf[nb+2u]);
        float3 bmax = float3(DynamicBvhBuf[nb+3u], DynamicBvhBuf[nb+4u], DynamicBvhBuf[nb+5u]);
        if (!IntersectWorldBounds(bmin, bmax, ro, inv_rd, max_t)) continue;
        uint lf = asuint(DynamicBvhBuf[nb+6u]);
        uint rc = asuint(DynamicBvhBuf[nb+7u]);
        bool is_leaf = (lf & 0x80000000u) != 0u;
        if (is_leaf) {
            if (rc == 0u) continue;
            uint instance_index = lf & 0x7FFFFFFFu;
            if (OccludedDynamicInstance(instance_index, ro, rd, max_t)) {
                return true;
            }
        } else {
            if (sp + 2u <= 128u) {
                stack[sp++] = rc;
                stack[sp++] = lf;
            }
        }
    }
    [loop]
    for (uint si = 0u; si < num_sdfs; ++si) {
        if (OccludedSdfPrimitive(si, ro, rd, max_t)) {
            return true;
        }
    }
    return false;
}

// ============================================================================
// Tangent frame   (matches ScalarCpuPathTracer)
// ============================================================================
void BuildFrame(float3 n, out float3 tang, out float3 btan) {
    float3 ref = (abs(n.z) < 0.999) ? float3(0,0,1) : float3(0,1,0);
    tang = normalize(cross(ref, n));
    btan = cross(n, tang);
}

// Cosine-weighted hemisphere sample
float3 SampleHemisphere(float3 n, inout uint rng) {
    float u1 = RandF(rng);
    float u2 = RandF(rng);
    float r  = sqrt(max(0.0, 1.0 - u1));
    float phi = 6.28318530718 * u2;
    float3 tang, btan;
    BuildFrame(n, tang, btan);
    return normalize(tang * (r * cos(phi)) + btan * (r * sin(phi)) + n * sqrt(max(0.0, u1)));
}

// Phong lobe sample around reflection direction
float3 SamplePhongLobe(float3 refl, float exponent, float3 normal, inout uint rng) {
    float u1 = RandF(rng);
    float u2 = RandF(rng);
    float cosT = pow(max(0.0, u1), 1.0 / (exponent + 1.0));
    float sinT = sqrt(max(0.0, 1.0 - cosT * cosT));
    float phi  = 6.28318530718 * u2;
    float3 tang, btan;
    BuildFrame(refl, tang, btan);
    float3 dir = normalize(tang * (sinT * cos(phi)) + btan * (sinT * sin(phi)) + refl * cosT);
    if (dot(dir, normal) <= 0.0) {
        dir = dir - 2.0 * dot(dir, normal) * normal;
        if (dot(dir, normal) <= 0.0) return SampleHemisphere(normal, rng);
    }
    return dir;
}

// ============================================================================
// Material helpers
// ============================================================================
static const uint kMatStride = 16u;
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
bool   IsMaterialTransmissive(uint mat_index) {
    if (num_mats == 0u) return false;
    uint idx = min(mat_index, num_mats - 1u);
    return MatModel(idx) == 5u || MatTransmission(idx) > 0.05;
}

uint WrapTexelCoord(int value, uint size) {
    int wrapped = value % int(size);
    if (wrapped < 0) wrapped += int(size);
    return uint(wrapped);
}

float3 DecodeRgba8(uint packed) {
    float3 srgb = float3(float(packed & 255u),
                         float((packed >> 8u) & 255u),
                         float((packed >> 16u) & 255u)) * (1.0 / 255.0);
    float3 lo = srgb / 12.92;
    float3 hi = pow(max((srgb + 0.055) / 1.055, 0.0), 2.4);
    return lerp(lo, hi, step(0.04045, srgb));
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

float3 MatSurfaceAlbedo(uint idx, float3 p, float3 n, float3 rd, float2 uv) {
    float3 color = MatAlbedo(idx);
    uint baseTexture = MatBaseColorTextureIndex(idx);
    if (baseTexture != kInvalidTextureIndex) {
        color *= SampleBaseColorTexture(baseTexture, uv);
    }
    uint effect = MatEffect(idx);
    float h = Hash01(p, float(effect));
    if (effect == 1u) {
        float rim = Pow2Fast(saturate(1.0 - abs(dot(n, -rd))));
        color = color * (0.65 + 0.25 * h) + float3(0.25, 0.22, 0.28) * rim * (0.4 + MatSheen(idx));
    } else if (effect == 2u) {
        float stripes = 0.5 + 0.5 * sin(p.x * 7.0 + p.z * 5.0 + h * 6.0);
        color *= 0.45 + 0.55 * stripes;
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
        float streak = 0.5 + 0.5 * sin(p.y * 18.0 + h * 5.0);
        color *= 0.55 + 0.45 * streak;
    } else if (effect == 12u) {
        float retro = Pow6Fast(saturate(dot(n, -rd)));
        color += float3(0.55, 0.65, 0.95) * retro;
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
        color = ProceduralWoodAlbedo(color, p, MatRoughness(idx), MatClearcoat(idx), effect);
    }
    return clamp(color, 0.0, 1.5);
}

bool MatIsEmissive(uint idx) {
    float3 e = MatEmissive(idx);
    return e.x > 0.0 || e.y > 0.0 || e.z > 0.0;
}

float3 PreviewTransmission(float3 ro, float3 rd, float3 fallback_env) {
    float3 origin = ro;
    [loop]
    for (uint skip = 0u; skip < 8u; ++skip) {
        Hit preview_hit = IntersectScene(origin, rd);
        if (!preview_hit.ok) {
            return SampleSceneEnvironment(rd, fallback_env);
        }

        uint preview_mat = (num_mats > 0u) ? min(preview_hit.mat, num_mats - 1u) : 0u;
        if (IsMaterialTransmissive(preview_mat)) {
            origin = preview_hit.pos + rd * 0.015f;
            continue;
        }

        float3 preview_n = preview_hit.n;
        if (dot(preview_n, -rd) < 0.0f) {
            preview_n = -preview_n;
        }
        float3 surface = MatSurfaceAlbedo(preview_mat, preview_hit.pos, preview_n, rd, preview_hit.uv);
        float3 emissive = MatEmissive(preview_mat);
        float ndotl = 0.45f;
        if (num_lights > 0u) {
            float3 lpos = float3(LightBuf[0u], LightBuf[1u], LightBuf[2u]);
            float3 ldir = normalize(lpos - preview_hit.pos);
            ndotl = max(ndotl, saturate(dot(preview_n, ldir)));
        }
        return surface * (0.28f + 0.72f * ndotl) + emissive;
    }
    return SampleSceneEnvironment(rd, fallback_env);
}

float3 LoadEnvTexel(uint x, uint y, uint width) {
    uint base = (y * width + x) * 3u;
    return float3(EnvBuf[base], EnvBuf[base + 1u], EnvBuf[base + 2u]);
}

float3 SampleSceneEnvironment(float3 rd, float3 fallback_env) {
    uint width = EnvMetaBuf[0];
    uint height = EnvMetaBuf[1];
    uint enabled = EnvMetaBuf[2];
    if (enabled == 0u || width == 0u || height == 0u) {
        return fallback_env;
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

// ============================================================================
// Path trace
// ============================================================================
float3 Trace(float3 ro, float3 rd, inout uint rng, float3 env) {
    float3 rad = 0.0;
    float3 thr = 1.0;
    uint max_depth = uint(max_depth_f);

    for (uint depth = 0u; depth < max_depth; ++depth) {
        Hit hit = IntersectScene(ro, rd);
        if (!hit.ok) {
            rad += thr * SampleSceneEnvironment(rd, env);
            break;
        }

        float3 geom_n = hit.n;
        bool entering_surface = dot(rd, geom_n) < 0.0;
        float3 n = entering_surface ? geom_n : -geom_n;

        uint mi = (num_mats > 0u) ? min(hit.mat, num_mats - 1u) : 0u;
        float3 albedo   = MatSurfaceAlbedo(mi, hit.pos, n, rd, hit.uv);
        float3 emissive = MatEmissive(mi);

        if (depth == 0u || num_lights == 0u) {
            rad += thr * emissive;
        }

        float roughness = MatRoughness(mi);
        uint  model = MatModel(mi);
        bool  is_mirror  = (model == 2u) || (roughness <= 0.001);
        bool  is_metallic = (model == 4u) || (MatMetallic(mi) > 0.65);
        bool  is_transmissive = (model == 5u) || (MatTransmission(mi) > 0.05);
        bool  is_diffuse = (roughness >= 0.999) && !is_mirror && !is_metallic && !is_transmissive;
        bool  is_sheen = (model == 6u);
        bool  is_clearcoat = (model == 7u) || (MatClearcoat(mi) > 0.05);
        bool  is_toon = (model == 8u);

        // NEE: sample one point light. The editor preview also adds a compact
        // specular/direct term so mirror, metal, glass, and clearcoat families
        // visibly respond under point lights even with a black environment.
        if (num_lights > 0u) {
            uint  li   = uint(RandF(rng) * float(num_lights));
            li = min(li, num_lights - 1u);
            uint  lb   = li * 16u;
            float3 lpos = float3(LightBuf[lb], LightBuf[lb+1u], LightBuf[lb+2u]);
            float3 lcol = float3(LightBuf[lb+3u], LightBuf[lb+4u], LightBuf[lb+5u]);
            float  lint = LightBuf[lb+6u];
            float  lrad = max(0.0f, LightBuf[lb+7u]);
            float3 spotDir = normalize(float3(LightBuf[lb+8u], LightBuf[lb+9u], LightBuf[lb+10u]));
            float  spotInner = LightBuf[lb+11u];
            float  spotOuter = LightBuf[lb+12u];

            // Radius-based area approximation for softer shadows.
            // radius=0 preserves point-light behavior.
            if (lrad > 1e-5f) {
                float uz  = 1.0f - 2.0f * RandF(rng);
                float upr = sqrt(max(0.0f, 1.0f - uz * uz));
                float phi = 6.28318530718f * RandF(rng);
                float3 ofs = float3(upr * cos(phi), uz, upr * sin(phi)) * lrad;
                lpos += ofs;
            }

            float3 to_l  = lpos - hit.pos;
            float  dist2 = dot(to_l, to_l);
            float  dist  = sqrt(dist2);
            float3 ldir  = to_l / dist;
            float  cos_t = dot(n, ldir);
            float  spotFactor = 1.0f;
            if (spotInner > -0.999f) {
                float cone = dot(-ldir, spotDir);
                if (cone <= spotOuter) {
                    spotFactor = 0.0f;
                } else if (cone < spotInner && spotInner > spotOuter) {
                    float t = saturate((cone - spotOuter) / (spotInner - spotOuter));
                    spotFactor = t * t * (3.0f - 2.0f * t);
                }
            }

            if (cos_t > 0.0 && dist > 1e-4 && spotFactor > 0.0f) {
                bool occ = OccludedScene(hit.pos + n * 0.002, ldir, dist - 0.004);
                if (is_transmissive) {
                    occ = false;
                }
                if (!occ) {
                    float3 irrad  = lcol * ((lint * spotFactor) / (dist2 + 1e-4));
                    float3 direct = float3(0.0, 0.0, 0.0);
                    if (!is_mirror && !is_transmissive) {
                        direct += albedo * (1.0 / 3.14159265) * irrad * cos_t * float(num_lights);
                    }
                    if (is_mirror || is_metallic || is_clearcoat || is_transmissive || roughness < 0.65) {
                        float3 view_dir = normalize(-rd);
                        float3 half_dir = normalize(ldir + view_dir);
                        float effectiveRoughness = max(0.025, roughness * (is_metallic ? 0.65 : 1.0));
                        float a2 = effectiveRoughness * effectiveRoughness;
                        float specPower = clamp(2.0 / max(0.0005, a2 * a2) - 2.0, 4.0, 96.0);
                        float spec = pow(saturate(dot(n, half_dir)), specPower);
                        float cosView = saturate(dot(n, view_dir));
                        float ior = MatIor(mi);
                        float f0 = (1.0 - ior) / (1.0 + ior);
                        f0 *= f0;
                        float fresnel = f0 + (1.0 - f0) * Pow5Fast(1.0 - cosView);
                        float specStrength = saturate((1.0 - roughness) * 0.8 +
                                                      MatMetallic(mi) * 0.45 +
                                                      MatClearcoat(mi) * 0.35 +
                                                      (is_mirror ? 0.75 : 0.0) +
                                                      (is_transmissive ? fresnel : 0.0));
                        float3 specTint = lerp(float3(1.0, 1.0, 1.0), albedo, is_metallic ? 0.85 : 0.12);
                        direct += specTint * irrad * spec * cos_t * float(num_lights) * max(0.15, specStrength);
                    }
                    rad += thr * direct;
                }
            }
        }

        // BSDF bounce
        float3 out_dir;
        bool refracted_bounce = false;
        if (is_mirror) {
            out_dir = rd - 2.0 * dot(n, rd) * n;
        } else if (is_transmissive) {
            float cosTheta = saturate(dot(n, -rd));
            float ior = MatIor(mi);
            float r0 = (1.0 - ior) / (1.0 + ior);
            r0 *= r0;
            float fresnel = r0 + (1.0 - r0) * Pow5Fast(1.0 - cosTheta);
            if (RandF(rng) < min(0.98, fresnel + MatClearcoat(mi) * 0.015)) {
                out_dir = rd - 2.0 * dot(n, rd) * n;
            } else {
                float eta = entering_surface ? (1.0 / ior) : ior;
                out_dir = refract(rd, n, eta);
                if (dot(out_dir, out_dir) <= 1.0e-8) {
                    out_dir = rd - 2.0 * dot(n, rd) * n;
                } else {
                    out_dir = normalize(out_dir);
                    refracted_bounce = true;
                }
            }
        } else if (is_diffuse || is_toon) {
            out_dir = SampleHemisphere(n, rng);
        } else {
            float effectiveRoughness = max(0.025, roughness * (is_metallic ? 0.75 : 1.0) * (is_clearcoat ? 0.65 : 1.0));
            float a2   = effectiveRoughness * effectiveRoughness;
            float expt = max(0.0, 2.0 / (a2 * a2) - 2.0);
            float3 refl = rd - 2.0 * dot(n, rd) * n;
            out_dir = SamplePhongLobe(refl, expt, n, rng);
            if (is_sheen && RandF(rng) < MatSheen(mi) * 0.35) {
                out_dir = SampleHemisphere(n, rng);
            }
        }
        if (!is_transmissive && dot(out_dir, n) <= 0.0) break;
        float3 bounce_weight = albedo;
        if (is_metallic || is_mirror) {
            bounce_weight = albedo * (0.65 + 0.35 * MatMetallic(mi));
        } else if (is_transmissive) {
            float tint_strength = saturate(MatAlpha(mi)) * MatTransmission(mi);
            bounce_weight = lerp(float3(1.0, 1.0, 1.0), albedo, tint_strength);
            if (refracted_bounce) {
                bounce_weight *= max(0.55, MatTransmission(mi));
            }
        }
        if (is_toon) {
            bounce_weight *= (dot(out_dir, n) > 0.55) ? 1.0 : 0.45;
        }
        thr *= bounce_weight;

        float mx = max(thr.x, max(thr.y, thr.z));
        if (mx < 0.001) break;

        // Russian roulette after depth 3
        if (depth >= 3u) {
            float rr = clamp(mx, 0.1, 0.99);
            if (RandF(rng) > rr) break;
            thr /= rr;
        }

        ro = hit.pos + out_dir * 0.002;
        rd = out_dir;
    }
    return rad;
}

// ============================================================================
// GPU guide generation + spatial denoise
// ============================================================================
[numthreads(8, 8, 1)]
void guide_main(uint3 gid : SV_DispatchThreadID) {
    if (gid.x >= width || gid.y >= height) return;

    uint pixel = gid.y * width + gid.x;
    float3 cam_pos  = float3(camera_pos_x, camera_pos_y, camera_pos_z);
    float3 cam_fwd  = float3(cam_fwd_x,    cam_fwd_y,    cam_fwd_z);
    float3 cam_right= float3(cam_right_x,  cam_right_y,  cam_right_z);
    float3 cam_up   = float3(cam_up_x,     cam_up_y,     cam_up_z);

    float fx = (float(gid.x) + 0.5f) / float(width);
    float fy = (float(gid.y) + 0.5f) / float(height);
    float nx = (2.0f * fx - 1.0f) * aspect * fov_tan_half;
    float ny = (1.0f - 2.0f * fy) * fov_tan_half;
    float3 dir = normalize(cam_fwd + cam_right * nx + cam_up * ny);

    Hit hit = IntersectScene(cam_pos, dir);
    if (!hit.ok) {
        StoreGuide(pixel, float4(0.0f, 0.0f, 0.0f, 1.0e20f),
                   float4(0.0f, 0.0f, 0.0f, 0.0f));
        return;
    }

    float3 n = hit.n;
    if (dot(n, -dir) < 0.0f) n = -n;
    uint mi = (num_mats > 0u) ? min(hit.mat, num_mats - 1u) : 0u;
    float3 albedo = MatSurfaceAlbedo(mi, hit.pos, n, dir, hit.uv);
    float roughness = MatRoughness(mi);
    StoreGuide(pixel, float4(saturate(albedo), hit.t), float4(n, 1.0f + roughness));
}

[numthreads(8, 8, 1)]
void denoise_main(uint3 gid : SV_DispatchThreadID) {
    if (gid.x >= width || gid.y >= height) return;

    uint pixel = gid.y * width + gid.x;
    float4 centerAcc = LoadFilm(pixel);
    float centerCount = max(1.0f, centerAcc.w);
    float3 centerColor = CurrentHdrSource(pixel);
    float4 centerAlbedoDepth = LoadGuideAlbedoDepth(pixel);
    float4 centerNormalHit = LoadGuideNormalHit(pixel);
    bool centerHit = centerNormalHit.w > 0.5f;
    float3 centerNormal = centerHit ? normalize(centerNormalHit.xyz) : float3(0.0f, 0.0f, 1.0f);
    float centerDepth = centerAlbedoDepth.w;
    float centerLuma = Luma(centerColor);

    float3 sum = centerColor;
    float weightSum = 1.0f;
    float colorSigma = max(0.025f, denoiser_color_sigma) * (1.0f + centerLuma * 0.35f);
    float depthSigma = max(0.01f, centerDepth * 0.015f);
    float albedoSigma = 0.35f;

    [unroll]
    for (int oy = -2; oy <= 2; ++oy) {
        int y = int(gid.y) + oy;
        if (y < 0 || y >= int(height)) continue;

        [unroll]
        for (int ox = -2; ox <= 2; ++ox) {
            int x = int(gid.x) + ox;
            if (x < 0 || x >= int(width) || (ox == 0 && oy == 0)) continue;

            uint neighbor = uint(y) * width + uint(x);
            float3 nColor = CurrentHdrSource(neighbor);
            float4 nAlbedoDepth = LoadGuideAlbedoDepth(neighbor);
            float4 nNormalHit = LoadGuideNormalHit(neighbor);
            bool nHit = nNormalHit.w > 0.5f;

            float dist2 = float(ox * ox + oy * oy);
            float weight = exp(-dist2 * 0.28f);

            if (centerHit && nHit) {
                float3 nNormal = normalize(nNormalHit.xyz);
                float normalWeight = pow(saturate(dot(centerNormal, nNormal)), 24.0f);
                float depthWeight = exp(-abs(nAlbedoDepth.w - centerDepth) / depthSigma);
                float3 albedoDiff = nAlbedoDepth.xyz - centerAlbedoDepth.xyz;
                float albedoWeight = exp(-dot(albedoDiff, albedoDiff) / max(1.0e-4f, albedoSigma * albedoSigma));
                weight *= normalWeight * depthWeight * albedoWeight;
            } else if (centerHit != nHit) {
                weight *= 0.02f;
            }

            float colorWeight = exp(-abs(Luma(nColor) - centerLuma) / colorSigma);
            weight *= colorWeight;
            sum += nColor * weight;
            weightSum += weight;
        }
    }

    float3 filtered = sum / max(1.0e-5f, weightSum);
    float adaptiveStrength = saturate(denoiser_strength) *
        (0.25f + 0.75f * saturate((64.0f - min(centerCount, 64.0f)) / 64.0f));
    StoreDenoised(pixel, float4(lerp(centerColor, filtered, adaptiveStrength), 1.0f));
}

float3 CameraRayDir(float2 pixelCenter) {
    float fx = pixelCenter.x / float(width);
    float fy = pixelCenter.y / float(height);
    float nx = (2.0f * fx - 1.0f) * aspect * fov_tan_half;
    float ny = (1.0f - 2.0f * fy) * fov_tan_half;
    return normalize(float3(cam_fwd_x, cam_fwd_y, cam_fwd_z) +
                     float3(cam_right_x, cam_right_y, cam_right_z) * nx +
                     float3(cam_up_x, cam_up_y, cam_up_z) * ny);
}

bool ProjectToPreviousPixel(float3 worldPos, out uint prevPixel, out float prevRayDistance) {
    float3 prevPos = float3(prev_camera_pos_x, prev_camera_pos_y, prev_camera_pos_z);
    float3 prevFwd = normalize(float3(prev_cam_fwd_x, prev_cam_fwd_y, prev_cam_fwd_z));
    float3 prevRight = normalize(float3(prev_cam_right_x, prev_cam_right_y, prev_cam_right_z));
    float3 prevUp = normalize(float3(prev_cam_up_x, prev_cam_up_y, prev_cam_up_z));
    float3 rel = worldPos - prevPos;
    prevRayDistance = length(rel);
    float forwardDistance = dot(rel, prevFwd);
    if (forwardDistance <= 1.0e-4f || prevRayDistance <= 1.0e-4f) {
        prevPixel = 0u;
        return false;
    }

    float projectedX = dot(rel, prevRight) / forwardDistance;
    float projectedY = dot(rel, prevUp) / forwardDistance;
    float denomX = max(1.0e-4f, prev_aspect * prev_fov_tan_half);
    float denomY = max(1.0e-4f, prev_fov_tan_half);
    float2 uv = float2(0.5f * (projectedX / denomX + 1.0f),
                       0.5f * (1.0f - projectedY / denomY));
    if (uv.x < 0.0f || uv.x >= 1.0f || uv.y < 0.0f || uv.y >= 1.0f) {
        prevPixel = 0u;
        return false;
    }

    uint px = min(width - 1u, uint(uv.x * float(width)));
    uint py = min(height - 1u, uint(uv.y * float(height)));
    prevPixel = py * width + px;
    return true;
}

float3 ClampHistoryToCurrentNeighborhood(uint2 coord, float3 historyColor, float3 centerColor) {
    float3 minColor = centerColor;
    float3 maxColor = centerColor;

    [unroll]
    for (int oy = -1; oy <= 1; ++oy) {
        int y = int(coord.y) + oy;
        if (y < 0 || y >= int(height)) continue;

        [unroll]
        for (int ox = -1; ox <= 1; ++ox) {
            int x = int(coord.x) + ox;
            if (x < 0 || x >= int(width)) continue;
            float3 c = FilmMean(uint(y) * width + uint(x));
            minColor = min(minColor, c);
            maxColor = max(maxColor, c);
        }
    }

    float margin = max(0.0f, temporal_color_margin) * (1.0f + Luma(centerColor));
    return clamp(historyColor, minColor - margin, maxColor + margin);
}

[numthreads(8, 8, 1)]
void temporal_main(uint3 gid : SV_DispatchThreadID) {
    if (gid.x >= width || gid.y >= height) return;

    uint pixel = gid.y * width + gid.x;
    float3 currentColor = FilmMean(pixel);
    float4 centerAlbedoDepth = LoadGuideAlbedoDepth(pixel);
    float4 centerNormalHit = LoadGuideNormalHit(pixel);
    bool centerHit = centerNormalHit.w > 0.5f && centerAlbedoDepth.w < 1.0e19f;

    if (temporal_enabled == 0u || temporal_history_valid == 0u || !centerHit) {
        StoreTemporal(pixel, float4(currentColor, 1.0f));
        return;
    }

    float3 rayDir = CameraRayDir(float2(float(gid.x) + 0.5f, float(gid.y) + 0.5f));
    float3 cameraPos = float3(camera_pos_x, camera_pos_y, camera_pos_z);
    float3 worldPos = cameraPos + rayDir * centerAlbedoDepth.w;

    uint prevPixel = 0u;
    float prevRayDistance = 0.0f;
    if (!ProjectToPreviousPixel(worldPos, prevPixel, prevRayDistance)) {
        StoreTemporal(pixel, float4(currentColor, 1.0f));
        return;
    }

    float4 prevAlbedoDepth = LoadPrevGuideAlbedoDepth(prevPixel);
    float4 prevNormalHit = LoadPrevGuideNormalHit(prevPixel);
    float4 prevHistory = LoadTemporalHistory(prevPixel);
    bool prevHit = prevNormalHit.w > 0.5f && prevAlbedoDepth.w < 1.0e19f && prevHistory.w > 0.5f;
    if (!prevHit) {
        StoreTemporal(pixel, float4(currentColor, 1.0f));
        return;
    }

    float3 centerNormal = normalize(centerNormalHit.xyz);
    float3 prevNormal = normalize(prevNormalHit.xyz);
    float normalWeight = pow(saturate(dot(centerNormal, prevNormal)), max(1.0f, temporal_normal_power));

    float depthTolerance = max(temporal_depth_sigma, prevRayDistance * 0.03f);
    float depthDelta = abs(prevAlbedoDepth.w - prevRayDistance);
    float depthWeight = exp(-depthDelta / max(1.0e-4f, depthTolerance));

    float3 albedoDiff = centerAlbedoDepth.xyz - prevAlbedoDepth.xyz;
    float albedoWeight = exp(-dot(albedoDiff, albedoDiff) / 0.16f);
    float confidence = normalWeight * depthWeight * albedoWeight;
    if (confidence < 0.04f) {
        StoreTemporal(pixel, float4(currentColor, 1.0f));
        return;
    }

    float3 historyColor = ClampHistoryToCurrentNeighborhood(gid.xy, prevHistory.xyz, currentColor);
    float historyLength = clamp(prevHistory.w, 1.0f, 64.0f);
    float feedback = min(0.96f, saturate(temporal_feedback) * confidence * saturate(historyLength / 4.0f));
    float3 temporalColor = lerp(currentColor, historyColor, feedback);
    StoreTemporal(pixel, float4(temporalColor, min(64.0f, historyLength + 1.0f)));
}
