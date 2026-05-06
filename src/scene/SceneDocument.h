#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "core/Types.h"
#include "scene/RenderExtraction.h"
#include "scene/SceneDocumentSchema.h"
#include "scene/SceneWorld.h"

namespace vkpt::scene {

class SceneDocument {
 public:
  /// Author-facing metadata and schema version carried through load/export.
  SceneMetadata metadata;
  /// External model or texture references that may expand into document entities.
  std::vector<SceneAssetDefinition> assets;
  std::vector<SceneMaterialDefinition> materials;
  std::vector<SceneGeometryDefinition> geometry;
  std::vector<SceneSdfPrimitiveDefinition> sdf_primitives;
  std::vector<SceneParticleEmitterDefinition> particle_emitters;
  /// Canonical authored entity records. Legacy top-level transform/camera/light
  /// sections are merged with these records when constructing a SceneWorld.
  std::vector<SceneEntityDefinition> entities;
  std::vector<SceneTransformEntry> transforms;
  std::vector<SceneCameraDefinition> cameras;
  std::vector<SceneLightDefinition> lights;
  SceneBenchmarkMetadata benchmark;

  /// Last parse/schema result for callers that keep a partially populated document.
  SceneSchemaError parse_result = SceneSchemaError::Ok;
  std::string parse_error;

  /// Parse a JSON scene document, assign missing stable IDs, and validate references.
  static vkpt::core::Result<SceneDocument> load_from_text(std::string_view text);
  /// Load a JSON scene document from disk and expand importable asset references relative to that file.
  static vkpt::core::Result<SceneDocument> load_from_file(std::string_view path);

  /// Validate ID uniqueness, hierarchy acyclicity, numeric ranges, and cross-section references.
  bool validate(std::vector<std::string>* issues = nullptr) const;
  /// Return whether the named top-level JSON section is part of the supported schema.
  bool has_section(std::string_view name) const;
  /// Build an ECS world from document definitions without mutating this document.
  vkpt::core::Result<SceneWorld> to_world() const;
  /// Clear and repopulate an existing ECS world from this document.
  vkpt::core::Result<void> apply_to_world(SceneWorld& world) const;

  /// Serialize the document using the engine JSON representation.
  std::string to_json(bool pretty = false) const;
  /// Return the snapshot scene hash encoded as lowercase hexadecimal.
  std::string export_hash_hex() const;
  /// Extract a stable, renderer-independent summary of scene content.
  SceneSnapshot snapshot() const;
  /// Convert document data to a render proxy, preferring ECS extraction when possible.
  RenderSceneProxy extract_render_scene(vkpt::core::FrameIndex frame = 0) const;
};

/// Abstract loader boundary used by runtime/editor code that should not depend on JSON details.
class ISceneLoader {
 public:
  virtual ~ISceneLoader() = default;
  virtual vkpt::core::Result<SceneDocument> load_document_from_text(std::string_view text) = 0;
  virtual vkpt::core::Result<SceneDocument> load_document_from_file(std::string_view path) = 0;
};

class JsonSceneLoader final : public ISceneLoader {
 public:
  vkpt::core::Result<SceneDocument> load_document_from_text(std::string_view text) override;
  vkpt::core::Result<SceneDocument> load_document_from_file(std::string_view path) override;
};

}  // namespace vkpt::scene
