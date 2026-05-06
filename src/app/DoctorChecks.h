#pragma once

namespace vkpt::app {

void RunDoctor(bool checkBuild,
               bool checkCpu,
               bool checkBackends,
               bool checkAssets,
               bool checkShaders,
               bool checkJobSystem,
               bool checkSceneSchema,
               bool checkBenchmarkArtifact);

}  // namespace vkpt::app
