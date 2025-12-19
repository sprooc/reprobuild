#ifndef BUILD_RECORD_H
#define BUILD_RECORD_H

#include "dependency_package.h"
#include <vector>
#include <string>
#include <map>

class BuildRecord {
public:
    BuildRecord();
    BuildRecord(const std::string& project_name);

    void addDependency(const DependencyPackage& package);
    void removeDependency(const std::string& package_name);
    
    bool hasDependency(const std::string& package_name) const;
    DependencyPackage getDependency(const std::string& package_name) const;
    std::vector<DependencyPackage> getAllDependencies() const;
    
    size_t getDependencyCount() const;
    
    std::string toString() const;
    bool matches(const BuildRecord& other) const;
    void saveToFile(const std::string& filepath) const;
    static BuildRecord loadFromFile(const std::string& filepath);

    const std::string& getProjectName() const { return project_name_; }
    void setProjectName(const std::string& name) { project_name_ = name; }

private:
    std::string project_name_;
    std::map<std::string, DependencyPackage> dependencies_;
};

#endif // BUILD_RECORD_H
