#include "uploader.h"

#include <openssl/hmac.h>
#include <openssl/sha.h>

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

#include "logger.h"
#include "utils.h"

namespace fs = std::filesystem;

Uploader::Uploader() { loadConfig(); }

void Uploader::loadConfig() {
  // Load MinIO host from environment variable, default to 127.0.0.1
  const char* host_env = std::getenv("MINIO_HOST");
  minio_host_ = host_env ? std::string(host_env) : "127.0.0.1";

  // Load MinIO port from environment variable, default to 9000
  const char* port_env = std::getenv("MINIO_PORT");
  minio_port_ = port_env ? std::atoi(port_env) : 9000;

  // Load MinIO access key and secret key
  const char* access_key_env = std::getenv("MINIO_ACCESS_KEY");
  minio_access_key_ =
      access_key_env ? std::string(access_key_env) : "minioadmin";

  const char* secret_key_env = std::getenv("MINIO_SECRET_KEY");
  minio_secret_key_ =
      secret_key_env ? std::string(secret_key_env) : "minioadmin";

  // Load bucket name
  const char* bucket_env = std::getenv("MINIO_BUCKET");
  bucket_name_ = bucket_env ? std::string(bucket_env) : "reprobuild";

  Logger::debug("MinIO configuration: " + minio_host_ + ":" +
                std::to_string(minio_port_) + ", bucket: " + bucket_name_);
}

int Uploader::uploadCustomDependencies(
    const std::vector<DependencyPackage>& dependencies) {
  int uploaded_count = 0;
  int skipped_count = 0;

  Logger::info("Starting upload of custom dependencies to MinIO");

  for (const auto& dep : dependencies) {
    if (dep.getOrigin() != DependencyOrigin::CUSTOM) {
      continue;
    }

    const std::string original_path = dep.getOriginalPath();
    const std::string hash = dep.getHashValue();

    if (!fs::exists(original_path)) {
      Logger::warn("Custom dependency file does not exist: " + original_path);
      continue;
    }

    // Check if file already exists on MinIO
    if (fileExistsOnMinio(hash)) {
      Logger::debug("File already exists on MinIO (hash: " + hash +
                    "), skipping: " + dep.getPackageName());
      skipped_count++;
      continue;
    }

    // Upload file
    if (uploadFile(original_path, hash)) {
      uploaded_count++;
    } else {
      Logger::error("Failed to upload: " + dep.getPackageName());
    }
  }

  Logger::info("Upload complete: " + std::to_string(uploaded_count) +
               " uploaded, " + std::to_string(skipped_count) + " skipped");

  return uploaded_count;
}

bool Uploader::uploadFile(const std::string& file_path,
                          const std::string& hash) {
  try {
    // Compress the file
    std::string compressed_file = compressFile(file_path);
    if (compressed_file.empty()) {
      Logger::error("Failed to compress file: " + file_path);
      return false;
    }

    // Upload to MinIO with hash as object name
    std::string object_name = hash + ".zip";
    bool success = uploadToMinioWithCurl(compressed_file, object_name);

    // Clean up compressed file
    fs::remove(compressed_file);

    return success;
  } catch (const std::exception& e) {
    Logger::error("Error uploading file: " + std::string(e.what()));
    return false;
  }
}

std::string Uploader::compressFile(const std::string& file_path) {
  try {
    // Create temporary output path
    std::string output_path = file_path + ".zip";

    // Use zip to compress the file
    std::string cmd = "zip -r -j " + output_path + " \"" + file_path + "\"";
    int result = std::system(cmd.c_str());

    if (result != 0) {
      Logger::error("zip command failed with code: " + std::to_string(result));
      return "";
    }

    if (!fs::exists(output_path)) {
      Logger::error("Compressed file was not created: " + output_path);
      return "";
    }

    return output_path;
  } catch (const std::exception& e) {
    Logger::error("Error compressing file: " + std::string(e.what()));
    return "";
  }
}

bool Uploader::fileExistsOnMinio(const std::string& hash) {
  std::string object_name = hash + ".zip";

  // Build resource path and URL
  std::string resource = "/" + bucket_name_ + "/" + object_name;
  std::string url =
      "http://" + minio_host_ + ":" + std::to_string(minio_port_) + resource;

  // Generate date string (RFC 1123)
  std::string date = getCurrentDateRFC1123();

  // Build string to sign (AWS Signature Version 2 for HEAD request)
  // Format: HTTP-VERB\n\nCONTENT-TYPE\nDATE\nCanonicalizedResource
  std::string string_to_sign = "HEAD\n\n\n" + date + "\n" + resource;

  // Calculate HMAC-SHA1 signature
  std::string signature = hmacSha1(minio_secret_key_, string_to_sign);

  // Build Authorization header
  std::string auth_header =
      "Authorization: AWS " + minio_access_key_ + ":" + signature;

  // Build curl command with proper headers
  std::ostringstream cmd;
  cmd << "curl -s -o /dev/null -w \"%{http_code}\" --head \"" << url << "\"";
  cmd << " -H \"Host: " << minio_host_ << ":" << minio_port_ << "\"";
  cmd << " -H \"Date: " << date << "\"";
  cmd << " -H \"" << auth_header << "\"";

  Logger::debug("Checking existence: " + url);

  // Execute curl command
  FILE* pipe = popen(cmd.str().c_str(), "r");
  if (!pipe) {
    Logger::warn("Failed to execute curl command for existence check");
    return false;
  }

  char buffer[128];
  std::string result;
  while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
    result += buffer;
  }
  pclose(pipe);

  // HTTP 200 means file exists
  return result.find("200") != std::string::npos;
}

bool Uploader::uploadToMinioWithCurl(const std::string& local_file,
                                     const std::string& object_name) {
  // Check if file exists
  if (!fs::exists(local_file)) {
    Logger::error("File does not exist: " + local_file);
    return false;
  }

  // Get file size
  uintmax_t file_size = fs::file_size(local_file);

  // Build resource path and URL
  std::string resource = "/" + bucket_name_ + "/" + object_name;
  std::string url =
      "http://" + minio_host_ + ":" + std::to_string(minio_port_) + resource;

  // Generate date string (RFC 1123)
  std::string date = getCurrentDateRFC1123();

  // Content type
  std::string content_type = "application/octet-stream";

  // Build string to sign (AWS Signature Version 2)
  // Format: HTTP-VERB\n\nCONTENT-TYPE\nDATE\nCanonicalizedResource
  std::string string_to_sign =
      "PUT\n\n" + content_type + "\n" + date + "\n" + resource;

  // Calculate HMAC-SHA1 signature
  std::string signature = hmacSha1(minio_secret_key_, string_to_sign);

  // Build Authorization header
  std::string auth_header =
      "Authorization: AWS " + minio_access_key_ + ":" + signature;

  // Build curl command with proper headers
  std::ostringstream cmd;
  cmd << "curl -s -X PUT -T \"" << local_file << "\" \"" << url << "\"";
  cmd << " -H \"Host: " << minio_host_ << ":" << minio_port_ << "\"";
  cmd << " -H \"Date: " << date << "\"";
  cmd << " -H \"Content-Type: " << content_type << "\"";
  cmd << " -H \"" << auth_header << "\"";
  cmd << " -w \"\\nHTTP_CODE:%{http_code}\"";

  Logger::debug("Upload URL: " + url);
  Logger::debug("String to sign: " + string_to_sign);
  Logger::debug("Signature: " + signature);

  // Execute curl command
  FILE* pipe = popen(cmd.str().c_str(), "r");
  if (!pipe) {
    Logger::error("Failed to execute curl command");
    return false;
  }

  // Read response
  char buffer[256];
  std::string response;
  while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
    response += buffer;
  }
  int exit_code = pclose(pipe);

  // Extract HTTP status code
  size_t code_pos = response.find("HTTP_CODE:");
  int http_code = 0;
  if (code_pos != std::string::npos) {
    http_code = std::stoi(response.substr(code_pos + 10));
  }

  // Check if upload was successful
  if (http_code == 200 || http_code == 201) {
    Logger::debug("Upload successful (HTTP " + std::to_string(http_code) + ")");
    return true;
  } else {
    Logger::error("Upload failed (HTTP " + std::to_string(http_code) + ")");
    if (!response.empty()) {
      Logger::error("Response: " + response);
    }
    return false;
  }
}

std::string Uploader::getCurrentDateRFC1123() {
  time_t now = time(nullptr);
  struct tm tm_info;
  gmtime_r(&now, &tm_info);

  char buffer[128];
  strftime(buffer, sizeof(buffer), "%a, %d %b %Y %H:%M:%S GMT", &tm_info);
  return std::string(buffer);
}

std::string Uploader::base64Encode(const unsigned char* input, int length) {
  static const char encoding_table[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

  std::string encoded;
  int i = 0;
  unsigned char array_3[3];
  unsigned char array_4[4];

  while (length--) {
    array_3[i++] = *(input++);
    if (i == 3) {
      array_4[0] = (array_3[0] & 0xfc) >> 2;
      array_4[1] = ((array_3[0] & 0x03) << 4) + ((array_3[1] & 0xf0) >> 4);
      array_4[2] = ((array_3[1] & 0x0f) << 2) + ((array_3[2] & 0xc0) >> 6);
      array_4[3] = array_3[2] & 0x3f;

      for (i = 0; i < 4; i++) {
        encoded += encoding_table[array_4[i]];
      }
      i = 0;
    }
  }

  if (i) {
    for (int j = i; j < 3; j++) {
      array_3[j] = '\0';
    }

    array_4[0] = (array_3[0] & 0xfc) >> 2;
    array_4[1] = ((array_3[0] & 0x03) << 4) + ((array_3[1] & 0xf0) >> 4);
    array_4[2] = ((array_3[1] & 0x0f) << 2) + ((array_3[2] & 0xc0) >> 6);

    for (int j = 0; j < i + 1; j++) {
      encoded += encoding_table[array_4[j]];
    }

    while (i++ < 3) {
      encoded += '=';
    }
  }

  return encoded;
}

std::string Uploader::hmacSha1(const std::string& key,
                               const std::string& data) {
  unsigned char* digest;
  unsigned int digest_len;

  digest = HMAC(EVP_sha1(), key.c_str(), key.length(),
                reinterpret_cast<const unsigned char*>(data.c_str()),
                data.length(), nullptr, &digest_len);

  return base64Encode(digest, digest_len);
}
