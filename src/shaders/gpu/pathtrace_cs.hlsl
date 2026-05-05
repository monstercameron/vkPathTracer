// GPU path tracer compute shader (HLSL / SM 6.0).
// One invocation per pixel; accumulates into a persistent RGBA32F ByteAddressBuffer.
// Root constants carry all per-dispatch state (camera, scene counts, sample index).
// Layout mirrors RTSceneData on the CPU side and pathtrace.comp (GLSL).

cbuffer PCBuf {
    float  camera_pos_x;  float camera_pos_y;  float camera_pos_z;  float fov_tan_half;
    float  cam_fwd_x;     float cam_fwd_y;     float cam_fwd_z;     float aspect;
    float  cam_right_x;   float cam_right_y;   float cam_right_z;   float pad0;
    float  cam_up_x;      float cam_up_y;      float cam_up_z;      uint  sample_index;
    uint   num_insts;     uint   num_mats;      uint   num_lights;   uint  width;
    uint   height;        uint   base_seed;     float  env_r;        float env_g;
    float  env_b;         float  max_depth_f;   uint   rays_per_pixel; float  exposure;
};

Buffer<float>    VertBuf  : register(t0);
Buffer<uint>     IndexBuf : register(t1);
Buffer<float>    MatBuf   : register(t2);
Buffer<uint>     InstBuf  : register(t3);
Buffer<float>    LightBuf : register(t4);
Buffer<float>    BvhBuf    : register(t5);
Buffer<uint>     TriMatBuf : register(t6);
RWByteAddressBuffer FilmBuf : register(u0);
RWBuffer<uint>   LdrBuf   : register(u1); // tonemapped RGBA8 output (R|G<<8|B<<16|0xFF<<24)

// ---- Forward declarations (fxc requires them) -------------------------------
float Halton2(uint idx);
float Halton3(uint idx);
struct Hit { bool ok; float t; float3 pos; float3 n; uint mat; };
uint  Pcg(uint v);
float RandF(inout uint rng);
bool  IntersectTri(float3 ro, float3 rd, uint i0, uint i1, uint i2, bool double_sided, inout float best_t);
bool  IntersectTriAny(float3 ro, float3 rd, uint i0, uint i1, uint i2, bool double_sided, float max_t);
bool  MatDoubleSided(uint idx);
Hit   IntersectScene(float3 ro, float3 rd);
bool  OccludedScene(float3 ro, float3 rd, float max_t);
float3 SampleHemisphere(float3 n, inout uint rng);
float3 SamplePhongLobe(float3 refl, float exponent, float3 normal, inout uint rng);
float3 Trace(float3 ro, float3 rd, inout uint rng, float3 env);

float4 LoadFilm(uint pixel) {
    return asfloat(FilmBuf.Load4(pixel * 16u));
}

void StoreFilm(uint pixel, float4 value) {
    FilmBuf.Store4(pixel * 16u, asuint(value));
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
        total_color += Trace(cam_pos, dir, rng, env_col);
    }

    float4 prev = LoadFilm(pixel);
    StoreFilm(pixel, float4(prev.x + total_color.x, prev.y + total_color.y,
                            prev.z + total_color.z, prev.w + float(rpp)));
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
    float  cnt = max(1.0f, acc.w);
    float3 c   = acc.xyz / cnt;

    // Reinhard tonemapping with per-frame exposure
    c *= max(0.0f, exposure);
    c  = c / (1.0f + c);

    // Gamma 2.2 encode
    c = pow(saturate(c), 1.0f / 2.2f);

    uint r = min(255u, (uint)(c.x * 255.0f + 0.5f));
    uint g = min(255u, (uint)(c.y * 255.0f + 0.5f));
    uint b = min(255u, (uint)(c.z * 255.0f + 0.5f));
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

    float3 inv_rd = float3(1.0f / rd.x, 1.0f / rd.y, 1.0f / rd.z);

    uint stack[32];
    uint sp = 0u;
    stack[sp++] = 0u; // root node index

    [loop]
    while (sp > 0u) {
        uint ni = stack[--sp];
        uint nb = ni * 8u;

        float3 bmin = float3(BvhBuf[nb+0u], BvhBuf[nb+1u], BvhBuf[nb+2u]);
        float3 bmax = float3(BvhBuf[nb+3u], BvhBuf[nb+4u], BvhBuf[nb+5u]);

        // Slab AABB intersection test
        float3 t0 = (bmin - ro) * inv_rd;
        float3 t1 = (bmax - ro) * inv_rd;
        float tenter = max(max(min(t0.x, t1.x), min(t0.y, t1.y)), min(t0.z, t1.z));
        float texit  = min(min(max(t0.x, t1.x), max(t0.y, t1.y)), max(t0.z, t1.z));
        if (texit < 1e-4f || tenter > texit || tenter > h.t) continue;

        uint lf = asuint(BvhBuf[nb+6u]);
        uint rc = asuint(BvhBuf[nb+7u]);
        bool is_leaf = (lf & 0x80000000u) != 0u;

        if (is_leaf) {
            uint first_tri = lf & 0x7FFFFFFFu;
            uint ntri = rc;
            for (uint ti = 0u; ti < ntri; ++ti) {
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
                }
            }
        } else {
            // Push right child first so left is processed first (DFS)
            stack[sp++] = rc;
            stack[sp++] = lf;
        }
    }
    return h;
}

bool OccludedScene(float3 ro, float3 rd, float max_t) {
    if (max_t <= 1e-4f) return false;

    float3 inv_rd = float3(1.0f / rd.x, 1.0f / rd.y, 1.0f / rd.z);
    uint stack[32];
    uint sp = 0u;
    stack[sp++] = 0u;

    [loop]
    while (sp > 0u) {
        uint ni = stack[--sp];
        uint nb = ni * 8u;

        float3 bmin = float3(BvhBuf[nb+0u], BvhBuf[nb+1u], BvhBuf[nb+2u]);
        float3 bmax = float3(BvhBuf[nb+3u], BvhBuf[nb+4u], BvhBuf[nb+5u]);
        float3 t0 = (bmin - ro) * inv_rd;
        float3 t1 = (bmax - ro) * inv_rd;
        float tenter = max(max(min(t0.x, t1.x), min(t0.y, t1.y)), min(t0.z, t1.z));
        float texit  = min(min(max(t0.x, t1.x), max(t0.y, t1.y)), max(t0.z, t1.z));
        if (texit < 1e-4f || tenter > texit || tenter > max_t) continue;

        uint lf = asuint(BvhBuf[nb+6u]);
        uint rc = asuint(BvhBuf[nb+7u]);
        bool is_leaf = (lf & 0x80000000u) != 0u;
        if (is_leaf) {
            uint first_tri = lf & 0x7FFFFFFFu;
            uint ntri = rc;
            for (uint ti = 0u; ti < ntri; ++ti) {
                uint tb = (first_tri + ti) * 3u;
                uint mat_index = TriMatBuf[first_tri + ti];
                if (IntersectTriAny(ro, rd, IndexBuf[tb], IndexBuf[tb + 1u], IndexBuf[tb + 2u],
                                    MatDoubleSided(mat_index), max_t)) {
                    return true;
                }
            }
        } else {
            stack[sp++] = rc;
            stack[sp++] = lf;
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

float Hash01(float3 p, float seed) {
    return frac(sin(dot(p, float3(12.9898, 78.233, 37.719)) + seed * 19.19) * 43758.5453);
}

float3 MatSurfaceAlbedo(uint idx, float3 p, float3 n, float3 rd) {
    float3 color = MatAlbedo(idx);
    uint effect = MatEffect(idx);
    float h = Hash01(p, float(effect));
    if (effect == 1u) {
        float rim = pow(saturate(1.0 - abs(dot(n, -rd))), 2.0);
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
        float retro = pow(saturate(dot(n, -rd)), 6.0);
        color += float3(0.55, 0.65, 0.95) * retro;
    } else if (effect == 13u) {
        color *= 0.65 + 0.35 * abs(sin(p.x * 10.0) * cos(p.z * 10.0));
    } else if (effect == 14u) {
        float rim = pow(saturate(1.0 - abs(dot(n, -rd))), 0.8);
        color = float3(0.15, 0.75, 1.0) * (0.2 + 0.8 * rim);
    }
    return clamp(color, 0.0, 1.5);
}

bool MatIsEmissive(uint idx) {
    float3 e = MatEmissive(idx);
    return e.x > 0.0 || e.y > 0.0 || e.z > 0.0;
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
            rad += thr * env;
            break;
        }

        float3 n = hit.n;
        if (dot(n, -rd) < 0.0) n = -n;

        uint mi = (num_mats > 0u) ? min(hit.mat, num_mats - 1u) : 0u;
        float3 albedo   = MatSurfaceAlbedo(mi, hit.pos, n, rd);
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

        // NEE: sample one point light (skip for perfect mirrors)
        if (num_lights > 0u && !is_mirror && !is_transmissive) {
            uint  li   = uint(RandF(rng) * float(num_lights));
            li = min(li, num_lights - 1u);
            uint  lb   = li * 8u;
            float3 lpos = float3(LightBuf[lb], LightBuf[lb+1u], LightBuf[lb+2u]);
            float3 lcol = float3(LightBuf[lb+3u], LightBuf[lb+4u], LightBuf[lb+5u]);
            float  lint = LightBuf[lb+6u];
            float  lrad = max(0.0f, LightBuf[lb+7u]);

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

            if (cos_t > 0.0 && dist > 1e-4) {
                bool occ = OccludedScene(hit.pos + n * 0.002, ldir, dist - 0.004);
                if (!occ) {
                    float3 irrad  = lcol * (lint / (dist2 + 1e-4));
                    float3 direct = albedo * (1.0 / 3.14159265) * irrad * cos_t * float(num_lights);
                    rad += thr * direct;
                }
            }
        }

        // BSDF bounce
        float3 out_dir;
        if (is_mirror) {
            out_dir = rd - 2.0 * dot(n, rd) * n;
        } else if (is_transmissive) {
            float cosTheta = saturate(dot(n, -rd));
            float ior = MatIor(mi);
            float r0 = (1.0 - ior) / (1.0 + ior);
            r0 *= r0;
            float fresnel = r0 + (1.0 - r0) * pow(1.0 - cosTheta, 5.0);
            if (RandF(rng) < min(0.98, fresnel + MatClearcoat(mi) * 0.15)) {
                out_dir = rd - 2.0 * dot(n, rd) * n;
            } else {
                out_dir = refract(rd, n, 1.0 / ior);
                if (dot(out_dir, out_dir) <= 1.0e-8) {
                    out_dir = rd - 2.0 * dot(n, rd) * n;
                } else {
                    out_dir = normalize(out_dir);
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
            bounce_weight = albedo * (0.25 + 0.55 * MatAlpha(mi)) + float3(0.2, 0.2, 0.2);
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

        ro = hit.pos + n * 0.002;
        rd = out_dir;
    }
    return rad;
}
