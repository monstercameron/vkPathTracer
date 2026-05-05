struct CameraUniforms {
  resolution : vec2<u32>,
  frame_index : u32,
  sample_index : u32,
};

@group(0) @binding(0)
var output_image : texture_storage_2d<rgba16float, write>;

@group(0) @binding(1)
var<uniform> camera : CameraUniforms;

fn hash2(pixel : vec2<u32>, seed : u32) -> f32 {
  var x = pixel.x * 1664525u + pixel.y * 1013904223u + seed * 747796405u;
  x = ((x >> 16u) ^ x) * 2246822519u;
  x = ((x >> 13u) ^ x) * 3266489917u;
  x = (x >> 16u) ^ x;
  return f32(x & 0x00ffffffu) / 16777215.0;
}

@compute @workgroup_size(8, 8, 1)
fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
  let dimensions = textureDimensions(output_image);
  if (gid.x >= dimensions.x || gid.y >= dimensions.y) {
    return;
  }

  let uv = vec2<f32>(vec2<u32>(gid.xy)) / vec2<f32>(max(dimensions, vec2<u32>(1u, 1u)));
  let jitter = hash2(gid.xy, camera.frame_index + camera.sample_index);
  let color = vec4<f32>(uv.x, uv.y, 0.18 + 0.08 * jitter, 1.0);
  textureStore(output_image, vec2<i32>(gid.xy), color);
}
