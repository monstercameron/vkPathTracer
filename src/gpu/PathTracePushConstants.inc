// Shared schema for the Vulkan path-tracing push constants.
//
// Include this file only after defining:
//   VKPT_PC_FLOAT3(name)
//   VKPT_PC_FLOAT(name)
//   VKPT_PC_UINT(name)
//
// Every entry is one 32-bit word except FLOAT3, which expands to three words.
// Keep this list as the single source of truth for both C++ and GLSL.
VKPT_PC_FLOAT3(camera_pos)
VKPT_PC_FLOAT(fov_tan_half)
VKPT_PC_FLOAT3(cam_forward)
VKPT_PC_FLOAT(aspect)
VKPT_PC_FLOAT3(cam_right)
VKPT_PC_UINT(num_sdfs)
VKPT_PC_FLOAT3(cam_up)
VKPT_PC_UINT(sample_index)
VKPT_PC_UINT(num_insts)
VKPT_PC_UINT(num_mats)
VKPT_PC_UINT(num_lights)
VKPT_PC_UINT(width)
VKPT_PC_UINT(height)
VKPT_PC_UINT(base_seed)
VKPT_PC_FLOAT(env_r)
VKPT_PC_FLOAT(env_g)
VKPT_PC_FLOAT(env_b)
VKPT_PC_FLOAT(max_depth_f)
VKPT_PC_FLOAT(aperture_radius)
VKPT_PC_FLOAT(focus_distance)
VKPT_PC_UINT(iris_blade_count)
VKPT_PC_FLOAT(iris_rotation_radians)
VKPT_PC_FLOAT(iris_roundness)
VKPT_PC_FLOAT(anamorphic_squeeze)
