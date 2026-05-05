#pragma once

#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "build_info.generated.h"

namespace vkpt::build {

struct BuildMetadata {
  std::string project_version;
  std::string git_hash;
  std::string build_date;
  std::string compiler_name;
  std::string compiler_version;
  std::string cxx_standard;
  std::string target_os;
  std::string target_arch;
  std::string build_type;
  std::string platform_shells;
  std::vector<std::string> enabled_features;
  std::vector<std::string> disabled_features;
  std::string simd_compile_options;
  std::string backend_compile_options;
  bool sanitizers_enabled = false;
  std::string sanitizer_flavor;
  std::string sanitizer_mode;
  bool strict_determinism = false;
  bool qt_enabled = false;
  bool qt_editor_enabled = false;
  bool qt_rhi_enabled = false;
  std::string qt_version;
};

inline std::string EscapeJson(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 16);
  for (const char ch : text) {
    switch (ch) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out.push_back(ch);
        break;
    }
  }
  return out;
}

inline std::vector<std::string> SplitFeatureCsv(std::string_view csv) {
  std::vector<std::string> out;
  if (csv.empty() || csv == "(none)") {
    return out;
  }

  std::size_t start = 0;
  while (start <= csv.size()) {
    const std::size_t comma = csv.find(',', start);
    const std::size_t end = comma == std::string_view::npos ? csv.size() : comma;
    if (end > start) {
      out.emplace_back(csv.substr(start, end - start));
    }
    if (comma == std::string_view::npos) {
      break;
    }
    start = comma + 1;
  }
  return out;
}

inline bool FeatureListContains(const std::vector<std::string>& features, std::string_view name) {
  for (const auto& feature : features) {
    if (feature == name) {
      return true;
    }
  }
  return false;
}

inline std::string SanitizerMode(bool enabled, std::string_view flavor) {
  if (!enabled) {
    return "disabled";
  }
  if (flavor.empty() || flavor == "disabled") {
    return "enabled";
  }
  return std::string(flavor);
}

inline BuildMetadata GetBuildMetadata() {
  BuildMetadata out;
  out.project_version = std::string(kProjectVersion);
  out.git_hash = std::string(kGitHash);
  out.build_date = std::string(kBuildDate);
  out.compiler_name = std::string(kCompilerName);
  out.compiler_version = std::string(kCompilerVersion);
  out.cxx_standard = std::string(kCxxStandard);
  out.target_os = std::string(kTargetOs);
  out.target_arch = std::string(kTargetArch);
  out.build_type = std::string(kBuildType);
  out.platform_shells = std::string(kPlatformShells);
  out.enabled_features = SplitFeatureCsv(kEnabledFeatureFlags);
  out.disabled_features = SplitFeatureCsv(kDisabledFeatureFlags);
  out.simd_compile_options = std::string(kSimdCompileOptions);
  out.backend_compile_options = std::string(kBackendCompileOptions);
  out.sanitizers_enabled = kSanitizersEnabled;
  out.sanitizer_flavor = std::string(kSanitizerFlavor);
  out.sanitizer_mode = SanitizerMode(kSanitizersEnabled, kSanitizerFlavor);
  out.strict_determinism = kStrictDeterminism;
  out.qt_enabled = kQtEnabled;
  out.qt_editor_enabled = kQtEditorEnabled;
  out.qt_rhi_enabled = kQtRhiEnabled;
  out.qt_version = std::string(kQtVersion);
  return out;
}

inline void AppendStringArrayJson(std::ostringstream& out,
                                  const std::vector<std::string>& values) {
  out << '[';
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i) {
      out << ", ";
    }
    out << '"' << EscapeJson(values[i]) << '"';
  }
  out << ']';
}

inline std::string SerializeBuildMetadata(const BuildMetadata& metadata) {
  std::ostringstream out;
  out << "{\n";
  out << "    \"project_version\": \"" << EscapeJson(metadata.project_version) << "\",\n";
  out << "    \"git_hash\": \"" << EscapeJson(metadata.git_hash) << "\",\n";
  out << "    \"build_date\": \"" << EscapeJson(metadata.build_date) << "\",\n";
  out << "    \"compiler_name\": \"" << EscapeJson(metadata.compiler_name) << "\",\n";
  out << "    \"compiler_version\": \"" << EscapeJson(metadata.compiler_version) << "\",\n";
  out << "    \"cxx_standard\": \"" << EscapeJson(metadata.cxx_standard) << "\",\n";
  out << "    \"target_os\": \"" << EscapeJson(metadata.target_os) << "\",\n";
  out << "    \"target_arch\": \"" << EscapeJson(metadata.target_arch) << "\",\n";
  out << "    \"build_type\": \"" << EscapeJson(metadata.build_type) << "\",\n";
  out << "    \"platform_shells\": \"" << EscapeJson(metadata.platform_shells) << "\",\n";
  out << "    \"enabled_features\": ";
  AppendStringArrayJson(out, metadata.enabled_features);
  out << ",\n";
  out << "    \"disabled_features\": ";
  AppendStringArrayJson(out, metadata.disabled_features);
  out << ",\n";
  out << "    \"sanitizers_enabled\": " << (metadata.sanitizers_enabled ? "true" : "false") << ",\n";
  out << "    \"sanitizer_flavor\": \"" << EscapeJson(metadata.sanitizer_flavor) << "\",\n";
  out << "    \"sanitizer_mode\": \"" << EscapeJson(metadata.sanitizer_mode) << "\",\n";
  out << "    \"strict_determinism\": " << (metadata.strict_determinism ? "true" : "false") << ",\n";
  out << "    \"simd_compile_options\": \"" << EscapeJson(metadata.simd_compile_options) << "\",\n";
  out << "    \"backend_compile_options\": \"" << EscapeJson(metadata.backend_compile_options) << "\",\n";
  out << "    \"qt_enabled\": " << (metadata.qt_enabled ? "true" : "false") << ",\n";
  out << "    \"qt_editor_enabled\": " << (metadata.qt_editor_enabled ? "true" : "false") << ",\n";
  out << "    \"qt_rhi_enabled\": " << (metadata.qt_rhi_enabled ? "true" : "false") << ",\n";
  out << "    \"qt_version\": \"" << EscapeJson(metadata.qt_version) << "\"\n";
  out << "  }";
  return out.str();
}

inline std::string SerializeBuildMetadata() {
  return SerializeBuildMetadata(GetBuildMetadata());
}

}  // namespace vkpt::build
