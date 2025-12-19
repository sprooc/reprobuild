#include "build_record.h"
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

bool BuildRecord::hasDependency(const std::string& package_name) const {
    return dependencies_.find(package_name) != dependencies_.end();
}

DependencyPackage BuildRecord::getDependency(const std::string& package_name) const {
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
    std::ofstream file(filepath);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open file for writing: " + filepath);
    }
    
    file << "# Build Record for " << project_name_ << std::endl;
    file << "project: " << project_name_ << std::endl;
    file << "dependencies:" << std::endl;
    
    auto deps = getAllDependencies();
    for (const auto& dep : deps) {
        file << "  - name: " << dep.getPackageName() << std::endl;
        file << "    path: " << dep.getOriginalPath() << std::endl;
        file << "    version: " << dep.getVersion() << std::endl;
        file << "    hash: " << dep.getHashValue() << std::endl;
    }
    
    file.close();
}

BuildRecord BuildRecord::loadFromFile(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open file for reading: " + filepath);
    }
    
    BuildRecord record;
    std::string line;
    std::string current_name, current_path, current_version, current_hash;
    
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        
        if (line.substr(0, 8) == "project:") {
            record.project_name_ = line.substr(9);
        } else if (line.find("  - name:") == 0) {
            if (!current_name.empty()) {
                DependencyPackage dep(current_name, current_path, current_version, current_hash);
                record.addDependency(dep);
            }
            current_name = line.substr(10);
            current_path = current_version = current_hash = "";
        } else if (line.find("    path:") == 0) {
            current_path = line.substr(10);
        } else if (line.find("    version:") == 0) {
            current_version = line.substr(13);
        } else if (line.find("    hash:") == 0) {
            current_hash = line.substr(10);
        }
    }
    
    if (!current_name.empty()) {
        DependencyPackage dep(current_name, current_path, current_version, current_hash);
        record.addDependency(dep);
    }
    
    file.close();
    return record;
}
