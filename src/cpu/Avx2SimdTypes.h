#pragma once

#include <cstdint>
#include <immintrin.h>

// SoA Vec3 backed by 3x __m256 (8-wide float)
struct Vec3M256 {
  __m256 x, y, z;
};

inline Vec3M256 broadcast_vec3_m256(float x, float y, float z) {
  return {_mm256_set1_ps(x), _mm256_set1_ps(y), _mm256_set1_ps(z)};
}

inline __m256 dot_m256(const Vec3M256& a, const Vec3M256& b) {
  __m256 m = _mm256_mul_ps(a.x, b.x);
  m = _mm256_fmadd_ps(a.y, b.y, m);
  m = _mm256_fmadd_ps(a.z, b.z, m);
  return m;
}

inline Vec3M256 mul_m256(const Vec3M256& a, __m256 scalar) {
  return {_mm256_mul_ps(a.x, scalar), _mm256_mul_ps(a.y, scalar), _mm256_mul_ps(a.z, scalar)};
}

inline Vec3M256 add_m256(const Vec3M256& a, const Vec3M256& b) {
  return {_mm256_add_ps(a.x, b.x), _mm256_add_ps(a.y, b.y), _mm256_add_ps(a.z, b.z)};
}

inline Vec3M256 sub_m256(const Vec3M256& a, const Vec3M256& b) {
  return {_mm256_sub_ps(a.x, b.x), _mm256_sub_ps(a.y, b.y), _mm256_sub_ps(a.z, b.z)};
}

inline Vec3M256 cross_m256(const Vec3M256& a, const Vec3M256& b) {
  return {
    _mm256_fmsub_ps(a.y, b.z, _mm256_mul_ps(a.z, b.y)),
    _mm256_fmsub_ps(a.z, b.x, _mm256_mul_ps(a.x, b.z)),
    _mm256_fmsub_ps(a.x, b.y, _mm256_mul_ps(a.y, b.x))
  };
}

inline Vec3M256 mad_m256(__m256 s, const Vec3M256& a, const Vec3M256& b) {
  return {
    _mm256_fmadd_ps(s, a.x, b.x),
    _mm256_fmadd_ps(s, a.y, b.y),
    _mm256_fmadd_ps(s, a.z, b.z)
  };
}

inline __m256 length_sq_m256(const Vec3M256& a) {
  return dot_m256(a, a);
}

inline __m256 max_m256(__m256 a, __m256 b) { return _mm256_max_ps(a, b); }
inline __m256 min_m256(__m256 a, __m256 b) { return _mm256_min_ps(a, b); }

inline __m256 sqrt_m256(__m256 a) { return _mm256_sqrt_ps(a); }

inline __m256 blendv_m256(__m256 a, __m256 b, __m256 mask) {
  return _mm256_blendv_ps(a, b, mask);
}

inline int movemask_m256(__m256 cmp) {
  return _mm256_movemask_ps(cmp);
}

inline __m256 cmp_lt_m256(__m256 a, __m256 b) {
  return _mm256_cmp_ps(a, b, _CMP_LT_OQ);
}

inline __m256 cmp_neq_m256(__m256 a) {
  __m256 zero = _mm256_setzero_ps();
  return _mm256_cmp_ps(a, zero, _CMP_NEQ_OQ);
}

inline __m256 rsqrt_m256(__m256 a) {
  return _mm256_rsqrt_ps(a);
}
