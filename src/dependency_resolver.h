#ifndef DEPENDENCY_RESOLVER_H
#define DEPENDENCY_RESOLVER_H

#include <string>

#include "dependency_package.h"

namespace DependencyResolver {

std::string canonicalizePath(const std::string& path);

bool getCachedPackage(PackageMgr pkg_mgr, const std::string& path,
                      DependencyPackage& package);
DependencyPackage cachePackage(PackageMgr pkg_mgr, const std::string& path,
                               const DependencyPackage& package);

bool resolveAptPackage(const std::string& raw_file_path,
                       DependencyPackage& package);
DependencyPackage createDependencyPackage(
    const std::string& package_name, DependencyOrigin origin,
    const std::string& raw_file_path, const std::string& real_path,
    const std::string& version);
DependencyPackage createCustomDependency(const std::string& raw_file_path);

}  // namespace DependencyResolver

#endif  // DEPENDENCY_RESOLVER_H
