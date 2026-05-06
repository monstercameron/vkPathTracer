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
  SceneMetadata metadata;
  std::vector<SceneAssetDefinition> assets;
  std::vector<SceneMaterialDefinition> materials;
  std::vector<SceneGeometryDefinition> geometry;
  std::vector<SceneSdfPrimitiveDefinition> sdf_primitives;
  std::vector<SceneEntityDefinition> entities;
  std::vector<SceneTransformEntry> transforms;
  std::vector<SceneCameraDefinition> cameras;
  std::vector<SceneLightDefinition> lights;
  SceneBenchmarkMetadata benchmark;

  SceneSchemaError parse_result = SceneSchemaError::Ok;
  std::string parse_error;

  static vkpt::core::Result<SceneDocument> load_from_text(std::string_view text);
  static vkpt::core::Result<SceneDocument> load_from_file(std::string_view path);

  bool validate(std::vector<std::string>* issues = nullptr) const;
  bool has_section(std::string_view name) const;
  vkpt::core::Result<SceneWorld> to_world() const;
  vkpt::core::Result<void> apply_to_world(SceneWorld& world) const;

  std::string to_json(bool pretty = false) const;
  std::string export_hash_hex() const;
  SceneSnapshot snapshot() const;
  RenderSceneProxy extract_render_scene(vkpt::core::FrameIndex frame = 0) const;
};

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
