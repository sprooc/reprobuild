#include "dependency_resolver.h"

#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

#include "utils.h"

namespace {

constexpr char kDpkgStatusPath[] = "/var/lib/dpkg/status";
constexpr char kDpkgInfoDir[] = "/var/lib/dpkg/info";

std::string stripArchitectureSuffix(const std::string& package_name) {
  const auto colon_pos = package_name.find(':');
  if (colon_pos == std::string::npos) {
    return package_name;
  }
  return package_name.substr(0, colon_pos);
}

class DependencyResolutionCache {
 public:
  static DependencyResolutionCache& instance() {
    static DependencyResolutionCache cache;
    return cache;
  }

  bool get(PackageMgr pkg_mgr, const std::string& path,
           DependencyPackage& package) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = packages_.find(makeKey(pkg_mgr, path));
    if (it == packages_.end()) {
      return false;
    }
    package = it->second;
    return true;
  }

  DependencyPackage put(PackageMgr pkg_mgr, const std::string& path,
                        const DependencyPackage& package) {
    std::lock_guard<std::mutex> lock(mutex_);
    packages_[makeKey(pkg_mgr, path)] = package;
    return package;
  }

 private:
  static std::string makeKey(PackageMgr pkg_mgr, const std::string& path) {
    return std::to_string(static_cast<int>(pkg_mgr)) + "\n" + path;
  }

  std::mutex mutex_;
  std::unordered_map<std::string, DependencyPackage> packages_;
};

struct DebianPackageOwner {
  std::string query_name;
  std::string display_name;
};

class DpkgDatabase {
 public:
  static const DpkgDatabase& instance() {
    static const DpkgDatabase database;
    return database;
  }

  const DebianPackageOwner* findOwner(const std::string& raw_path,
                                      const std::string& real_path) const {
    if (const DebianPackageOwner* owner = findOwnerByExactPath(raw_path)) {
      return owner;
    }
    return findOwnerByExactPath(real_path);
  }

  std::string findVersion(const DebianPackageOwner& owner) const {
    auto it = package_versions_.find(owner.query_name);
    if (it != package_versions_.end()) {
      return it->second;
    }

    it = package_versions_.find(owner.display_name);
    if (it != package_versions_.end()) {
      return it->second;
    }

    return fallbackDpkgQuery(owner);
  }

 private:
  DpkgDatabase() {
    loadInstalledVersions();
    loadFileOwners();
  }

  const DebianPackageOwner* findOwnerByExactPath(
      const std::string& path) const {
    auto it = path_to_package_.find(path);
    if (it == path_to_package_.end()) {
      return nullptr;
    }
    return &it->second;
  }

  void loadInstalledVersions() {
    std::ifstream status_file(kDpkgStatusPath);
    if (!status_file.is_open()) return;

    std::string line;
    std::string package_name;
    std::string architecture;
    std::string version;

    auto flush_stanza = [&]() {
      if (!package_name.empty() && !version.empty()) {
        package_versions_[package_name] = version;
        if (!architecture.empty()) {
          package_versions_[package_name + ":" + architecture] = version;
        }
      }
      package_name.clear();
      architecture.clear();
      version.clear();
    };

    while (std::getline(status_file, line)) {
      if (line.empty()) {
        flush_stanza();
        continue;
      }

      readField(line, "Package: ", package_name);
      readField(line, "Architecture: ", architecture);
      readField(line, "Version: ", version);
    }
    flush_stanza();
  }

  void loadFileOwners() {
    const std::filesystem::path info_dir(kDpkgInfoDir);
    std::error_code ec;
    if (!std::filesystem::exists(info_dir, ec)) return;

    for (const auto& entry : std::filesystem::directory_iterator(info_dir, ec)) {
      if (ec) {
        break;
      }
      if (!isDpkgFileList(entry)) continue;

      const std::string query_name = entry.path().stem().string();
      DebianPackageOwner owner{query_name, stripArchitectureSuffix(query_name)};
      loadFileList(entry.path(), owner);
    }
  }

  bool isDpkgFileList(const std::filesystem::directory_entry& entry) const {
    std::error_code ec;
    return entry.is_regular_file(ec) && entry.path().extension() == ".list";
  }

  void loadFileList(const std::filesystem::path& list_path,
                    const DebianPackageOwner& owner) {
    std::ifstream list_file(list_path);
    std::string owned_path;
    while (std::getline(list_file, owned_path)) {
      if (!owned_path.empty()) {
        path_to_package_.emplace(owned_path, owner);
      }
    }
  }

  static void readField(const std::string& line, const std::string& prefix,
                        std::string& value) {
    if (Utils::startsWith(line, prefix)) {
      value = line.substr(prefix.size());
    }
  }

  static std::string fallbackDpkgQuery(const DebianPackageOwner& owner) {
    std::string version_command = "dpkg-query -W -f='${Version}\\n' " +
                                  owner.query_name + " 2>/dev/null";
    std::string version = Utils::executeCommand(version_command);
    if (!version.empty()) {
      return version;
    }

    version_command = "dpkg-query -W -f='${Version}\\n' " +
                      owner.display_name + " 2>/dev/null";
    return Utils::executeCommand(version_command);
  }

  std::unordered_map<std::string, DebianPackageOwner> path_to_package_;
  std::unordered_map<std::string, std::string> package_versions_;
};

}  // namespace

namespace DependencyResolver {

std::string canonicalizePath(const std::string& path) {
  std::error_code ec;
  auto canonical = std::filesystem::canonical(path, ec);
  if (!ec) {
    return canonical.string();
  }

  auto weak_canonical = std::filesystem::weakly_canonical(path, ec);
  if (!ec) {
    return weak_canonical.string();
  }

  auto absolute = std::filesystem::absolute(path, ec);
  return ec ? path : absolute.string();
}

bool getCachedPackage(PackageMgr pkg_mgr, const std::string& path,
                      DependencyPackage& package) {
  return DependencyResolutionCache::instance().get(pkg_mgr, path, package);
}

DependencyPackage cachePackage(PackageMgr pkg_mgr, const std::string& path,
                               const DependencyPackage& package) {
  return DependencyResolutionCache::instance().put(pkg_mgr, path, package);
}

bool resolveAptPackage(const std::string& raw_file_path,
                       DependencyPackage& package) {
  const std::string real_path = canonicalizePath(raw_file_path);
  const DebianPackageOwner* owner =
      DpkgDatabase::instance().findOwner(raw_file_path, real_path);
  if (!owner) {
    return false;
  }

  const std::string version = DpkgDatabase::instance().findVersion(*owner);
  if (version.empty()) {
    throw std::runtime_error("Could not get version for package: " +
                             owner->query_name);
  }

  package = createDependencyPackage(owner->display_name, DependencyOrigin::APT,
                                    raw_file_path, real_path, version);
  return true;
}

DependencyPackage createDependencyPackage(
    const std::string& package_name, DependencyOrigin origin,
    const std::string& raw_file_path, const std::string& real_path,
    const std::string& version) {
  std::string hash_value = Utils::calculateFileHash(real_path);
  if (hash_value.empty()) {
    throw std::runtime_error("Could not calculate hash for file: " +
                             raw_file_path);
  }

  return DependencyPackage(package_name, origin, real_path, version,
                           hash_value);
}

DependencyPackage createCustomDependency(const std::string& raw_file_path) {
  const std::string real_path = canonicalizePath(raw_file_path);
  const std::string package_name =
      std::filesystem::path(real_path).filename().string();
  return createDependencyPackage(package_name, DependencyOrigin::CUSTOM,
                                 raw_file_path, real_path, "custom");
}

}  // namespace DependencyResolver
