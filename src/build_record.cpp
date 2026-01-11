#include "build_record.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>

BuildRecord::BuildRecord() : project_name_("") {}

BuildRecord::BuildRecord(const std::string& project_name)
    : project_name_(project_name) {}

void BuildRecord::addDependency(const DependencyPackage& package) {
  if (package.isValid()) {
    dependencies_[package.getPackageName()] = package;
  }
}

void BuildRecord::removeDependency(const std::string& package_name) {
  dependencies_.erase(package_name);
}

void BuildRecord::addArtifact(const BuildArtifact& artifact) {
  artifacts_.push_back(artifact);
}

void BuildRecord::clearArtifacts() { artifacts_.clear(); }

const std::vector<BuildArtifact>& BuildRecord::getArtifacts() const {
  return artifacts_;
}

bool BuildRecord::hasDependency(const std::string& package_name) const {
  return dependencies_.find(package_name) != dependencies_.end();
}

DependencyPackage BuildRecord::getDependency(
    const std::string& package_name) const {
  auto it = dependencies_.find(package_name);
  if (it != dependencies_.end()) {
    return it->second;
  }
  return DependencyPackage();
}

std::vector<DependencyPackage> BuildRecord::getAllDependencies() const {
  std::vector<DependencyPackage> result;
  result.reserve(dependencies_.size());

  for (const auto& pair : dependencies_) {
    result.push_back(pair.second);
  }

  std::sort(result.begin(), result.end());
  return result;
}

size_t BuildRecord::getDependencyCount() const { return dependencies_.size(); }

bool BuildRecord::matches(const BuildRecord& other) const {
  if (project_name_ != other.project_name_) {
    return false;
  }

  if (dependencies_.size() != other.dependencies_.size()) {
    return false;
  }

  for (const auto& pair : dependencies_) {
    auto it = other.dependencies_.find(pair.first);
    if (it == other.dependencies_.end() || !(pair.second == it->second)) {
      return false;
    }
  }

  return true;
}

std::string BuildRecord::toString() const {
  std::ostringstream oss;
  oss << "BuildRecord{";
  oss << "project: \"" << project_name_ << "\", ";
  oss << "dependencies: [";

  auto deps = getAllDependencies();
  for (size_t i = 0; i < deps.size(); ++i) {
    if (i > 0) oss << ", ";
    oss << deps[i].getPackageName() << "@" << deps[i].getVersion();
  }

  oss << "]}";
  return oss.str();
}

void BuildRecord::saveToFile(const std::string& filepath) const {
  YAML::Node root;

  // Add metadata section
  YAML::Node metadata_node;
  metadata_node["architecture"] = architecture_;
  metadata_node["distribution"] = distribution_;
  metadata_node["build_cmd"] = build_cmd_;
  metadata_node["build_path"] = build_path_;
  metadata_node["build_timestamp"] = build_timestamp_;
  metadata_node["hostname"] = hostname_;
  metadata_node["locale"] = locale_;
  metadata_node["umask"] = umask_;
  metadata_node["random_seed"] = random_seed_;
  root["metadata"] = metadata_node;

  YAML::Node deps_node;
  auto deps = getAllDependencies();
  for (const auto& dep : deps) {
    YAML::Node dep_node;
    dep_node["name"] = dep.getPackageName();
    dep_node["path"] = dep.getOriginalPath();
    dep_node["version"] = dep.getVersion();
    dep_node["hash"] = dep.getHashValue();
    deps_node.push_back(dep_node);
  }
  root["dependencies"] = deps_node;

  // Add artifacts section
  YAML::Node artifacts_node;
  for (const auto& artifact : artifacts_) {
    YAML::Node artifact_node;
    artifact_node["path"] = artifact.path;
    artifact_node["hash"] = artifact.hash;
    artifact_node["type"] = artifact.type;
    artifacts_node.push_back(artifact_node);
  }
  root["artifacts"] = artifacts_node;

  // Add git commit ids section
  YAML::Node git_commits_node;
  for (const auto& [repo, commit] : git_commit_ids_) {
    YAML::Node commit_node;
    commit_node["repo"] = repo;
    commit_node["commit_id"] = commit;
    git_commits_node.push_back(commit_node);
  }
  if (!git_commit_ids_.empty()) root["git_commit_ids"] = git_commits_node;

  std::ofstream file(filepath);
  if (!file.is_open()) {
    throw std::runtime_error("Cannot open file for writing: " + filepath);
  }

  file << "# Build Record for " << project_name_ << std::endl;
  file << root << std::endl;
  file.close();
}

BuildRecord BuildRecord::loadFromFile(const std::string& filepath) {
  std::ifstream file(filepath);
  if (!file.is_open()) {
    throw std::runtime_error("Cannot open file for reading: " + filepath);
  }

  YAML::Node root = YAML::Load(file);
  file.close();

  BuildRecord record;

  if (root["project"]) {
    record.project_name_ = root["project"].as<std::string>();
  }

  if (root["dependencies"] && root["dependencies"].IsSequence()) {
    for (const auto& dep_node : root["dependencies"]) {
      if (dep_node["name"] && dep_node["path"] && dep_node["version"] &&
          dep_node["hash"]) {
        std::string name = dep_node["name"].as<std::string>();
        std::string path = dep_node["path"].as<std::string>();
        std::string version = dep_node["version"].as<std::string>();
        std::string hash = dep_node["hash"].as<std::string>();

        DependencyPackage dep(name, path, version, hash);
        record.addDependency(dep);
      }
    }
  }

  // Load artifacts section
  if (root["artifacts"] && root["artifacts"].IsSequence()) {
    for (const auto& artifact_node : root["artifacts"]) {
      if (artifact_node["path"] && artifact_node["hash"] &&
          artifact_node["type"]) {
        std::string path = artifact_node["path"].as<std::string>();
        std::string hash = artifact_node["hash"].as<std::string>();
        std::string type = artifact_node["type"].as<std::string>();

        BuildArtifact artifact(path, hash, type);
        record.addArtifact(artifact);
      }
    }
  }

  // Load metadata section
  if (root["metadata"]) {
    const auto& metadata = root["metadata"];
    if (metadata["architecture"]) {
      record.architecture_ = metadata["architecture"].as<std::string>();
    }
    if (metadata["distribution"]) {
      record.distribution_ = metadata["distribution"].as<std::string>();
    }
    if (metadata["build_path"]) {
      record.build_path_ = metadata["build_path"].as<std::string>();
    }
    if (metadata["build_timestamp"]) {
      record.build_timestamp_ = metadata["build_timestamp"].as<std::string>();
    }
    if (metadata["hostname"]) {
      record.hostname_ = metadata["hostname"].as<std::string>();
    }
    if (metadata["locale"]) {
      record.locale_ = metadata["locale"].as<std::string>();
    }
    if (metadata["umask"]) {
      record.umask_ = metadata["umask"].as<std::string>();
    }
  }

  return record;
}
