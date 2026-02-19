#ifndef UPLOADER_H
#define UPLOADER_H

#include <string>
#include <vector>

#include "dependency_package.h"

class Uploader {
 public:
  Uploader();

  // Upload custom dependencies to MinIO server
  // Each file is compressed (gzip) and uploaded with its hash as filename
  // Returns number of files successfully uploaded
  int uploadCustomDependencies(
      const std::vector<DependencyPackage>& dependencies);

  // Upload a single file to MinIO server
  // file_path: path to the file to upload
  // hash: hash value to use as the filename on MinIO
  // Returns true if successful
  bool uploadFile(const std::string& file_path, const std::string& hash);

 private:
  std::string minio_host_;
  int minio_port_;
  std::string minio_access_key_;
  std::string minio_secret_key_;
  std::string bucket_name_;

  // Load configuration from environment variables
  void loadConfig();

  // Compress a file using gzip
  // Returns path to compressed file, or empty string on failure
  std::string compressFile(const std::string& file_path);

  // Check if a file already exists on MinIO (by hash)
  bool fileExistsOnMinio(const std::string& hash);

  // Execute curl command to upload file to MinIO
  bool uploadToMinioWithCurl(const std::string& local_file,
                             const std::string& object_name);

  // Generate RFC 1123 formatted date string
  std::string getCurrentDateRFC1123();

  // Calculate HMAC-SHA1 signature
  std::string hmacSha1(const std::string& key, const std::string& data);

  // Base64 encode
  std::string base64Encode(const unsigned char* input, int length);
};

#endif  // UPLOADER_H
