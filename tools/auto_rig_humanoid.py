#!/usr/bin/env python3
"""auto_rig_humanoid.py

Procedural humanoid rigger for static GLB meshes (e.g. Meshy AI characters).

Reads a GLB whose mesh is a single static (non-skinned) humanoid character,
computes a 14-joint humanoid skeleton matching the schema used by
``assets/models/low_poly_hero/character.gltf``, computes per-vertex linear
blend skinning weights (4 bones per vertex, weighted by 1/d^2 to bone
center), then writes a new GLB with:

  * an additional ``skins`` array (one skin, 14 joints).
  * additional skeleton joint nodes (parented as a tree under ``body``).
  * the existing mesh's parent node tagged with ``skin: 0``.
  * ``JOINTS_0`` (uint16 VEC4) + ``WEIGHTS_0`` (float32 VEC4) accessors
    appended to every primitive of the chosen mesh.

The script preserves all original geometry, materials, textures, scene
hierarchy, etc.

Usage:
    python tools/auto_rig_humanoid.py \
        --input game/models/Meshy_AI_Black_Ops_Operator_in_0507164325_texture.glb \
        --output game/models/black_ops_operator_rigged.glb

Joint layout (matches low_poly_hero):
    body          (root)              (0, 0.5*H, 0)
    shoulder.r    parent=body         ( 0.18*W, 0.78*H, 0)
    shoulder.l    parent=body         (-0.18*W, 0.78*H, 0)
    arm.r         parent=shoulder.r   ( 0.30*W, 0.65*H, 0)
    arm.l         parent=shoulder.l   (-0.30*W, 0.65*H, 0)
    hand.r        parent=arm.r        ( 0.42*W, 0.50*H, 0)
    hand.l        parent=arm.l        (-0.42*W, 0.50*H, 0)
    thigh.r       parent=body         ( 0.10*W, 0.45*H, 0)
    thigh.l       parent=body         (-0.10*W, 0.45*H, 0)
    leg.r         parent=thigh.r      ( 0.10*W, 0.22*H, 0)
    leg.l         parent=thigh.l      (-0.10*W, 0.22*H, 0)
    foot.r        parent=leg.r        ( 0.10*W, 0.0,  0.05*D)
    foot.l        parent=leg.l        (-0.10*W, 0.0,  0.05*D)
    head          parent=body         (0, 0.92*H, 0)

W/H/D = mesh axis-aligned bounding box dimensions (after applying any
node transforms that affect the chosen mesh).

The order of joints matches the order presented to the engine by
``character.gltf`` (which the engine alphabetises into a stable joint
roster).  Engine-side validation only requires "exactly 14 joints, single
root", so the order is not load-bearing.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import struct
import sys
from typing import List, Optional, Tuple

try:
    import pygltflib  # type: ignore
    HAVE_PYGLTFLIB = True
except ImportError:
    HAVE_PYGLTFLIB = False


# ---------------------------------------------------------------------------
# joint layout
# ---------------------------------------------------------------------------

# Each entry: (name, parent_name_or_None, local_offset_factors_xyz)
# local_offset is in WORLD-bind position space (we'll convert to local
# translations relative to parent during emit).
JOINT_LAYOUT: List[Tuple[str, Optional[str], Tuple[float, float, float]]] = [
    ("body",        None,         (0.00,  0.50,  0.00)),
    ("shoulder.r",  "body",       ( 0.18, 0.78,  0.00)),
    ("shoulder.l",  "body",       (-0.18, 0.78,  0.00)),
    ("arm.r",       "shoulder.r", ( 0.30, 0.65,  0.00)),
    ("arm.l",       "shoulder.l", (-0.30, 0.65,  0.00)),
    ("hand.r",      "arm.r",      ( 0.42, 0.50,  0.00)),
    ("hand.l",      "arm.l",      (-0.42, 0.50,  0.00)),
    ("thigh.r",     "body",       ( 0.10, 0.45,  0.00)),
    ("thigh.l",     "body",       (-0.10, 0.45,  0.00)),
    ("leg.r",       "thigh.r",    ( 0.10, 0.22,  0.00)),
    ("leg.l",       "thigh.l",    (-0.10, 0.22,  0.00)),
    ("foot.r",      "leg.r",      ( 0.10, 0.00,  0.05)),  # +Z forward
    ("foot.l",      "leg.l",      (-0.10, 0.00,  0.05)),
    ("head",        "body",       ( 0.00, 0.92,  0.00)),
]


# ---------------------------------------------------------------------------
# math helpers (4x4 column-major)
# ---------------------------------------------------------------------------

def mat4_translation(tx: float, ty: float, tz: float) -> List[float]:
    """Column-major 4x4 translation matrix."""
    return [
        1.0, 0.0, 0.0, 0.0,
        0.0, 1.0, 0.0, 0.0,
        0.0, 0.0, 1.0, 0.0,
        tx,  ty,  tz,  1.0,
    ]


def mat4_inverse_translation(tx: float, ty: float, tz: float) -> List[float]:
    """Inverse of a pure translation (column-major 4x4)."""
    return [
        1.0, 0.0, 0.0, 0.0,
        0.0, 1.0, 0.0, 0.0,
        0.0, 0.0, 1.0, 0.0,
        -tx, -ty, -tz, 1.0,
    ]


# ---------------------------------------------------------------------------
# GLB reader
# ---------------------------------------------------------------------------

GLB_MAGIC = 0x46546C67  # 'glTF'
GLB_VERSION = 2
CHUNK_JSON = 0x4E4F534A
CHUNK_BIN = 0x004E4942


def load_glb(path: str) -> Tuple[dict, bytes]:
    """Return (json_dict, bin_blob). bin_blob is empty if the GLB has no BIN chunk."""
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 12:
        raise ValueError("file too small to be a GLB")
    magic, version, total_length = struct.unpack_from("<III", data, 0)
    if magic != GLB_MAGIC:
        raise ValueError(f"not a GLB (magic={magic:08x})")
    if version != GLB_VERSION:
        raise ValueError(f"unsupported GLB version: {version}")
    offset = 12
    json_dict: Optional[dict] = None
    bin_blob: bytes = b""
    while offset < total_length:
        chunk_length, chunk_type = struct.unpack_from("<II", data, offset)
        offset += 8
        chunk_data = data[offset:offset + chunk_length]
        offset += chunk_length
        if chunk_type == CHUNK_JSON:
            json_dict = json.loads(chunk_data.decode("utf-8").rstrip("\x00 "))
        elif chunk_type == CHUNK_BIN:
            bin_blob = bytes(chunk_data)
    if json_dict is None:
        raise ValueError("GLB missing JSON chunk")
    return json_dict, bin_blob


def save_glb(path: str, json_dict: dict, bin_blob: bytes) -> None:
    """Write a GLB with one JSON chunk and one BIN chunk."""
    json_text = json.dumps(json_dict, separators=(",", ":")).encode("utf-8")
    json_pad = (4 - (len(json_text) % 4)) % 4
    json_text = json_text + (b" " * json_pad)
    bin_pad = (4 - (len(bin_blob) % 4)) % 4
    bin_padded = bin_blob + (b"\x00" * bin_pad)
    total = 12 + 8 + len(json_text) + 8 + len(bin_padded)
    with open(path, "wb") as f:
        f.write(struct.pack("<III", GLB_MAGIC, GLB_VERSION, total))
        f.write(struct.pack("<II", len(json_text), CHUNK_JSON))
        f.write(json_text)
        f.write(struct.pack("<II", len(bin_padded), CHUNK_BIN))
        f.write(bin_padded)


def save_gltf_pair(json_path: str, json_dict: dict, bin_blob: bytes) -> None:
    """Write a JSON .gltf + companion .bin pair.

    The engine's SceneAssetGltfLoader currently only consumes JSON glTF
    with external .bin buffers (see comment at LoadGltf top), so this
    output form is required for runtime loads.
    """
    bin_path = os.path.splitext(json_path)[0] + ".bin"
    bin_uri = os.path.basename(bin_path)
    out_dict = json.loads(json.dumps(json_dict))  # deep copy
    if out_dict.get("buffers"):
        out_dict["buffers"][0]["uri"] = bin_uri
        out_dict["buffers"][0]["byteLength"] = len(bin_blob)
    else:
        out_dict["buffers"] = [{"uri": bin_uri, "byteLength": len(bin_blob)}]
    with open(json_path, "w", encoding="utf-8") as f:
        json.dump(out_dict, f, separators=(",", ":"))
    with open(bin_path, "wb") as f:
        f.write(bin_blob)


# ---------------------------------------------------------------------------
# accessor helpers
# ---------------------------------------------------------------------------

COMPONENT_SIZE = {
    5120: 1, 5121: 1, 5122: 2, 5123: 2, 5125: 4, 5126: 4,
}
TYPE_COMPONENTS = {
    "SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4,
    "MAT2": 4, "MAT3": 9, "MAT4": 16,
}


def accessor_byte_stride(accessor: dict) -> int:
    return COMPONENT_SIZE[accessor["componentType"]] * TYPE_COMPONENTS[accessor["type"]]


def read_accessor_floats_vec3(json_dict: dict, bin_blob: bytes,
                              accessor_index: int) -> List[Tuple[float, float, float]]:
    """Decode a float VEC3 accessor (e.g. POSITION)."""
    accessor = json_dict["accessors"][accessor_index]
    if accessor["type"] != "VEC3" or accessor["componentType"] != 5126:
        raise ValueError(f"accessor {accessor_index} is not VEC3 float")
    bv = json_dict["bufferViews"][accessor["bufferView"]]
    buffer_index = bv.get("buffer", 0)
    if buffer_index != 0:
        raise ValueError("multi-buffer GLB not supported")
    base = bv.get("byteOffset", 0) + accessor.get("byteOffset", 0)
    stride = bv.get("byteStride", 12)
    count = accessor["count"]
    out: List[Tuple[float, float, float]] = []
    for i in range(count):
        x, y, z = struct.unpack_from("<fff", bin_blob, base + i * stride)
        out.append((x, y, z))
    return out


# ---------------------------------------------------------------------------
# scene traversal: pick a candidate mesh + its world transform
# ---------------------------------------------------------------------------

def compose_node_translation(node: dict) -> Tuple[float, float, float]:
    """Approximate world translation if the node has only a translation."""
    if "translation" in node:
        t = node["translation"]
        return (t[0], t[1], t[2])
    if "matrix" in node:
        # column-major
        m = node["matrix"]
        return (m[12], m[13], m[14])
    return (0.0, 0.0, 0.0)


def find_mesh_node(json_dict: dict) -> Tuple[int, int]:
    """Find the first node referencing a mesh; return (node_index, mesh_index).

    Walks scenes[scene] roots first, then falls back to a flat scan.
    """
    nodes = json_dict.get("nodes", [])
    candidates: List[Tuple[int, int]] = []
    for i, n in enumerate(nodes):
        if "mesh" in n:
            candidates.append((i, n["mesh"]))
    if not candidates:
        raise ValueError("GLB has no node referencing a mesh")
    # If multiple, pick the one with most vertices (likely the body).
    if len(candidates) == 1:
        return candidates[0]
    best = candidates[0]
    best_verts = -1
    for node_idx, mesh_idx in candidates:
        v = sum(_primitive_vertex_count(json_dict, p)
                for p in json_dict["meshes"][mesh_idx].get("primitives", []))
        if v > best_verts:
            best_verts = v
            best = (node_idx, mesh_idx)
    return best


def _primitive_vertex_count(json_dict: dict, primitive: dict) -> int:
    pos_acc = primitive.get("attributes", {}).get("POSITION")
    if pos_acc is None:
        return 0
    return json_dict["accessors"][pos_acc].get("count", 0)


def gather_world_positions(json_dict: dict, bin_blob: bytes,
                           mesh_index: int, world_offset: Tuple[float, float, float]
                          ) -> Tuple[List[List[Tuple[float, float, float]]],
                                     Tuple[float, float, float],
                                     Tuple[float, float, float]]:
    """Return (per-primitive position lists in MESH-LOCAL space,
       bbox_min, bbox_max in MESH-LOCAL space).

    We work in mesh-local space: the joint positions are computed from the
    bounding box of the mesh as authored, then expressed in mesh-local
    coordinates so the joint nodes (children of the mesh's parent node)
    end up correctly positioned regardless of the parent's translation.
    """
    primitives = json_dict["meshes"][mesh_index].get("primitives", [])
    per_prim: List[List[Tuple[float, float, float]]] = []
    bbox_min = [math.inf, math.inf, math.inf]
    bbox_max = [-math.inf, -math.inf, -math.inf]
    for prim in primitives:
        pos_acc = prim["attributes"]["POSITION"]
        positions = read_accessor_floats_vec3(json_dict, bin_blob, pos_acc)
        per_prim.append(positions)
        for x, y, z in positions:
            if x < bbox_min[0]: bbox_min[0] = x
            if y < bbox_min[1]: bbox_min[1] = y
            if z < bbox_min[2]: bbox_min[2] = z
            if x > bbox_max[0]: bbox_max[0] = x
            if y > bbox_max[1]: bbox_max[1] = y
            if z > bbox_max[2]: bbox_max[2] = z
    return per_prim, tuple(bbox_min), tuple(bbox_max)


# ---------------------------------------------------------------------------
# skeleton + skinning
# ---------------------------------------------------------------------------

def compute_joint_world_positions(bbox_min: Tuple[float, float, float],
                                  bbox_max: Tuple[float, float, float]
                                 ) -> List[Tuple[float, float, float]]:
    """Convert JOINT_LAYOUT factors into mesh-local 3D positions.

    The factors in the spec are absolute (e.g. 0.5*H), but they assume the
    mesh's local-space origin is at (cx, 0, cz) with the mesh standing on
    the floor (y=0) and centered on x/z. We bias positions back into the
    mesh's actual bbox so a Meshy character authored at, say, y in
    [-1.0, +1.0] is handled correctly.
    """
    width  = bbox_max[0] - bbox_min[0]
    height = bbox_max[1] - bbox_min[1]
    depth  = bbox_max[2] - bbox_min[2]
    cx = 0.5 * (bbox_min[0] + bbox_max[0])
    cz = 0.5 * (bbox_min[2] + bbox_max[2])
    base_y = bbox_min[1]
    out: List[Tuple[float, float, float]] = []
    for _name, _parent, (fx, fy, fz) in JOINT_LAYOUT:
        x = cx + fx * width
        y = base_y + fy * height
        z = cz + fz * depth
        out.append((x, y, z))
    return out


def joint_centers(world_positions: List[Tuple[float, float, float]]
                 ) -> List[Tuple[float, float, float]]:
    """Bone center = midpoint between joint and parent (for root, joint itself)."""
    name_to_idx = {name: i for i, (name, _, _) in enumerate(JOINT_LAYOUT)}
    out: List[Tuple[float, float, float]] = []
    for i, (_name, parent_name, _) in enumerate(JOINT_LAYOUT):
        if parent_name is None:
            out.append(world_positions[i])
        else:
            p = world_positions[name_to_idx[parent_name]]
            j = world_positions[i]
            out.append(((p[0] + j[0]) * 0.5, (p[1] + j[1]) * 0.5, (p[2] + j[2]) * 0.5))
    return out


def compute_skin_weights(positions: List[Tuple[float, float, float]],
                         centers: List[Tuple[float, float, float]]
                        ) -> Tuple[List[Tuple[int, int, int, int]],
                                   List[Tuple[float, float, float, float]]]:
    """For each vertex, return its 4 closest bones + normalized 1/d^2 weights."""
    EPS = 1e-6
    joints_out: List[Tuple[int, int, int, int]] = []
    weights_out: List[Tuple[float, float, float, float]] = []
    for px, py, pz in positions:
        # distance^2 to each bone center
        scored: List[Tuple[float, int]] = []
        for j_idx, (cx, cy, cz) in enumerate(centers):
            dx = px - cx; dy = py - cy; dz = pz - cz
            scored.append((dx * dx + dy * dy + dz * dz, j_idx))
        scored.sort(key=lambda t: t[0])
        top4 = scored[:4]
        raw_weights = [1.0 / (d2 + EPS) for d2, _ in top4]
        s = sum(raw_weights)
        if s <= 0.0:
            raw_weights = [1.0, 0.0, 0.0, 0.0]
            s = 1.0
        norm = [w / s for w in raw_weights]
        joints_out.append((top4[0][1], top4[1][1], top4[2][1], top4[3][1]))
        weights_out.append((norm[0], norm[1], norm[2], norm[3]))
    return joints_out, weights_out


# ---------------------------------------------------------------------------
# GLB augmentation
# ---------------------------------------------------------------------------

def append_buffer_view(json_dict: dict, current_bin: bytearray,
                       data: bytes, target: Optional[int] = None,
                       byte_stride: Optional[int] = None) -> int:
    """Append data to the BIN blob and create a bufferView. Return its index."""
    # Pad to 4 bytes before this view, GLB-spec friendly.
    pad = (4 - (len(current_bin) % 4)) % 4
    current_bin.extend(b"\x00" * pad)
    offset = len(current_bin)
    current_bin.extend(data)
    bv = {
        "buffer": 0,
        "byteOffset": offset,
        "byteLength": len(data),
    }
    if target is not None:
        bv["target"] = target
    if byte_stride is not None:
        bv["byteStride"] = byte_stride
    json_dict.setdefault("bufferViews", []).append(bv)
    return len(json_dict["bufferViews"]) - 1


def append_accessor(json_dict: dict, buffer_view: int, component_type: int,
                    count: int, type_: str,
                    min_: Optional[List[float]] = None,
                    max_: Optional[List[float]] = None) -> int:
    accessor = {
        "bufferView": buffer_view,
        "componentType": component_type,
        "count": count,
        "type": type_,
    }
    if min_ is not None:
        accessor["min"] = min_
    if max_ is not None:
        accessor["max"] = max_
    json_dict.setdefault("accessors", []).append(accessor)
    return len(json_dict["accessors"]) - 1


def add_skeleton_to_glb(json_dict: dict, bin_blob: bytes,
                        mesh_node_index: int, mesh_index: int,
                        per_prim_positions: List[List[Tuple[float, float, float]]],
                        bbox_min: Tuple[float, float, float],
                        bbox_max: Tuple[float, float, float]) -> Tuple[dict, bytes]:
    """Augment json_dict + bin_blob with skin, joints, JOINTS_0/WEIGHTS_0."""
    new_bin = bytearray(bin_blob)
    nodes = json_dict.setdefault("nodes", [])
    name_to_idx = {name: i for i, (name, _, _) in enumerate(JOINT_LAYOUT)}

    # 1) Compute joint world positions in mesh-local space.
    joint_world = compute_joint_world_positions(bbox_min, bbox_max)

    # 2) Each new joint node lives in the parent joint's local frame
    #    (translation = world_joint - world_parent_joint, except root which
    #    sits in mesh-local space directly under the mesh node's parent).
    joint_node_indices: List[int] = []
    for i, (name, parent_name, _) in enumerate(JOINT_LAYOUT):
        if parent_name is None:
            wx, wy, wz = joint_world[i]
            translation = [wx, wy, wz]
        else:
            p = joint_world[name_to_idx[parent_name]]
            j = joint_world[i]
            translation = [j[0] - p[0], j[1] - p[1], j[2] - p[2]]
        node = {
            "name": name,
            "translation": translation,
            "rotation": [0.0, 0.0, 0.0, 1.0],
            "scale": [1.0, 1.0, 1.0],
        }
        nodes.append(node)
        joint_node_indices.append(len(nodes) - 1)

    # Wire children.
    children_by_parent: dict[int, List[int]] = {}
    for i, (_name, parent_name, _) in enumerate(JOINT_LAYOUT):
        if parent_name is None:
            continue
        p_idx = joint_node_indices[name_to_idx[parent_name]]
        children_by_parent.setdefault(p_idx, []).append(joint_node_indices[i])
    for parent_node_idx, child_list in children_by_parent.items():
        existing_children = nodes[parent_node_idx].get("children", [])
        nodes[parent_node_idx]["children"] = existing_children + child_list

    # 3) Inverse-bind matrices (column-major, mesh-local space).
    ibm_floats: List[float] = []
    for wx, wy, wz in joint_world:
        ibm_floats.extend(mat4_inverse_translation(wx, wy, wz))
    ibm_bytes = struct.pack("<%df" % len(ibm_floats), *ibm_floats)
    ibm_bv = append_buffer_view(json_dict, new_bin, ibm_bytes)
    ibm_acc = append_accessor(json_dict, ibm_bv, 5126,
                              len(JOINT_LAYOUT), "MAT4")

    # 4) Skin object.
    skin = {
        "name": "auto_humanoid",
        "joints": joint_node_indices,
        "inverseBindMatrices": ibm_acc,
        "skeleton": joint_node_indices[name_to_idx["body"]],
    }
    skins = json_dict.setdefault("skins", [])
    skin_index = len(skins)
    skins.append(skin)

    # 5) Tag mesh node + add joint nodes as children of the mesh node's
    #    parent. Strategy: parent the joint hierarchy (root only) under the
    #    same parent as the mesh node so the bind-pose joint translations
    #    in mesh-local coordinates produce the correct world positions.
    nodes[mesh_node_index]["skin"] = skin_index
    # Find mesh node parent (or scene root).
    mesh_parent_idx: Optional[int] = None
    for i, n in enumerate(nodes):
        if mesh_node_index in n.get("children", []):
            mesh_parent_idx = i
            break
    root_joint_node = joint_node_indices[name_to_idx["body"]]
    if mesh_parent_idx is not None:
        existing = nodes[mesh_parent_idx].get("children", [])
        if root_joint_node not in existing:
            nodes[mesh_parent_idx]["children"] = existing + [root_joint_node]
    else:
        # Mesh node lives at scene root; add the root joint to that scene.
        scenes = json_dict.setdefault("scenes", [{"nodes": [mesh_node_index]}])
        scene_obj = scenes[json_dict.get("scene", 0)]
        scene_nodes = scene_obj.setdefault("nodes", [])
        if root_joint_node not in scene_nodes:
            scene_nodes.append(root_joint_node)

    # 6) Per-primitive JOINTS_0 + WEIGHTS_0.
    primitives = json_dict["meshes"][mesh_index].get("primitives", [])
    centers = joint_centers(joint_world)
    for prim, positions in zip(primitives, per_prim_positions):
        joints, weights = compute_skin_weights(positions, centers)

        # JOINTS_0: uint16 VEC4
        j_bytes = b"".join(struct.pack("<HHHH", *t) for t in joints)
        j_bv = append_buffer_view(json_dict, new_bin, j_bytes,
                                  target=34962, byte_stride=8)
        j_acc = append_accessor(json_dict, j_bv, 5123, len(joints), "VEC4")

        # WEIGHTS_0: float32 VEC4
        w_floats: List[float] = [c for t in weights for c in t]
        w_bytes = struct.pack("<%df" % len(w_floats), *w_floats)
        w_bv = append_buffer_view(json_dict, new_bin, w_bytes,
                                  target=34962, byte_stride=16)
        w_acc = append_accessor(json_dict, w_bv, 5126, len(weights), "VEC4")

        attrs = prim.setdefault("attributes", {})
        attrs["JOINTS_0"] = j_acc
        attrs["WEIGHTS_0"] = w_acc

    # 7) Update the buffer's byteLength.
    if json_dict.get("buffers"):
        json_dict["buffers"][0]["byteLength"] = len(new_bin)
    else:
        json_dict["buffers"] = [{"byteLength": len(new_bin)}]
    # Strip any URI field — embedded GLB never has uri on buffer 0.
    json_dict["buffers"][0].pop("uri", None)

    return json_dict, bytes(new_bin)


# ---------------------------------------------------------------------------
# self-check (post-write verification)
# ---------------------------------------------------------------------------

def self_check(path: str) -> bool:
    if path.lower().endswith(".gltf"):
        with open(path, "r", encoding="utf-8") as f:
            json_dict = json.load(f)
    else:
        json_dict, _ = load_glb(path)
    skins = json_dict.get("skins", [])
    if len(skins) < 1:
        print(f"[self-check] FAIL: no skins array")
        return False
    if len(skins[0].get("joints", [])) != 14:
        print(f"[self-check] FAIL: skin0 joints != 14 (got {len(skins[0].get('joints', []))})")
        return False
    # Check at least one primitive on the mesh has JOINTS_0 + WEIGHTS_0
    found = False
    for mesh in json_dict.get("meshes", []):
        for prim in mesh.get("primitives", []):
            attrs = prim.get("attributes", {})
            if "JOINTS_0" in attrs and "WEIGHTS_0" in attrs:
                found = True
                break
        if found:
            break
    if not found:
        print(f"[self-check] FAIL: no primitive carries JOINTS_0/WEIGHTS_0")
        return False
    # Confirm at least one node has skin field
    if not any("skin" in n for n in json_dict.get("nodes", [])):
        print(f"[self-check] FAIL: no node references the skin")
        return False
    print(f"[self-check] ok: 14 joints, JOINTS_0/WEIGHTS_0 present, mesh node has skin")
    return True


# ---------------------------------------------------------------------------
# entry point
# ---------------------------------------------------------------------------

def main() -> int:
    parser = argparse.ArgumentParser(description="Procedurally rig a static humanoid GLB.")
    parser.add_argument("--input", required=True, help="Source GLB.")
    parser.add_argument("--output", required=True, help="Destination GLB.")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    if not os.path.isfile(args.input):
        print(f"input not found: {args.input}", file=sys.stderr)
        return 2

    print(f"[auto_rig] reading {args.input}")
    json_dict, bin_blob = load_glb(args.input)

    nodes = json_dict.get("nodes", [])
    if args.verbose:
        print(f"[auto_rig] nodes={len(nodes)}, meshes={len(json_dict.get('meshes', []))}, "
              f"existing skins={len(json_dict.get('skins', []))}")
        print(f"[auto_rig] existing buffer0 length={len(bin_blob)}")

    if json_dict.get("skins"):
        print(f"[auto_rig] WARNING: input already has {len(json_dict['skins'])} skin(s); "
              f"new skin will be added (existing untouched)")

    mesh_node_index, mesh_index = find_mesh_node(json_dict)
    print(f"[auto_rig] target mesh node={mesh_node_index} mesh={mesh_index} "
          f"name={nodes[mesh_node_index].get('name','?')}")

    per_prim_positions, bbox_min, bbox_max = gather_world_positions(
        json_dict, bin_blob, mesh_index, (0.0, 0.0, 0.0))

    width  = bbox_max[0] - bbox_min[0]
    height = bbox_max[1] - bbox_min[1]
    depth  = bbox_max[2] - bbox_min[2]
    print(f"[auto_rig] bbox min={bbox_min} max={bbox_max}")
    print(f"[auto_rig] W={width:.4f} H={height:.4f} D={depth:.4f}")

    if width <= 0.0 or height <= 0.0 or depth <= 0.0:
        print("[auto_rig] FATAL: degenerate bounding box; cannot rig", file=sys.stderr)
        return 3

    new_json, new_bin = add_skeleton_to_glb(
        json_dict, bin_blob, mesh_node_index, mesh_index,
        per_prim_positions, bbox_min, bbox_max)

    print(f"[auto_rig] writing {args.output} (bin={len(new_bin)} bytes)")
    if args.output.lower().endswith(".gltf"):
        save_gltf_pair(args.output, new_json, new_bin)
    else:
        save_glb(args.output, new_json, new_bin)

    ok = self_check(args.output)
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
