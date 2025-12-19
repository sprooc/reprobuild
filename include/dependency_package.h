#ifndef DEPENDENCY_PACKAGE_H
#define DEPENDENCY_PACKAGE_H

#include <iostream>
#include <string>

class DependencyPackage {
 public:
  DependencyPackage();
  DependencyPackage(const std::string& package_name,
                    const std::string& original_path,
                    const std::string& version, const std::string& hash_value);

  // Getter methods
  const std::string& getPackageName() const { return package_name_; }
  const std::string& getOriginalPath() const { return original_path_; }
  const std::string& getVersion() const { return version_; }
  const std::string& getHashValue() const { return hash_value_; }

  // Setter methods
  void setPackageName(const std::string& package_name) {
    package_name_ = package_name;
  }
  void setOriginalPath(const std::string& original_path) {
    original_path_ = original_path;
  }
  void setVersion(const std::string& version) { version_ = version; }
  void setHashValue(const std::string& hash_value) { hash_value_ = hash_value; }

  // Utility methods
  bool isValid() const;
  std::string generateUniqueId() const;
  bool matches(const DependencyPackage& other) const;
  bool verifyIntegrity(const std::string& computed_hash) const;
  std::string toString() const;

  static DependencyPackage fromRawFile(const std::string& raw_file_path);

 private:
  std::string package_name_;   ///< Name of the dependency package
  std::string original_path_;  ///< Original source path or URL
  std::string version_;        ///< Version string of the package
  std::string hash_value_;  ///< Cryptographic hash for integrity verification
};

std::ostream& operator<<(std::ostream& os, const DependencyPackage& package);

#endif  // DEPENDENCY_PACKAGE_H
