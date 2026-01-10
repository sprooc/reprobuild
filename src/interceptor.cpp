#include <dlfcn.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

// External environ variable
extern char** environ;

// Macros for function pointer declarations
#define DECLARE_REAL_FUNC(name, ret_type, ...) \
  static ret_type (*real_##name)(__VA_ARGS__) = nullptr;

#define LOAD_REAL_FUNC(name, ret_type, ...)                             \
  if (!real_##name) {                                                   \
    real_##name = (ret_type (*)(__VA_ARGS__))dlsym(RTLD_NEXT, #name);   \
    if (!real_##name) {                                                 \
      fprintf(stderr, "Error loading real %s: %s\n", #name, dlerror()); \
      exit(1);                                                          \
    }                                                                   \
  }

#define INTERCEPT_LOG(name, pathname) \
  printf("Intercepted %s: %s\n", #name, pathname);

#define INTERCEPT_WITH_ARGV(name, pathname, argv, call_real)         \
  load_real_exec_functions();                                        \
  INTERCEPT_LOG(name, pathname)                                      \
  if (handle_git_clone(argv)) {                                      \
    return 0;                                                        \
  }                                                                  \
  if (should_add_compiler_flags(pathname)) {                         \
    char* const* orig_argv = (char**)argv;                           \
    argv = add_arguments(argv, getenv("REPROBUILD_COMPILER_FLAGS")); \
    if (argv) {                                                      \
      int result = call_real;                                        \
      free_argv((char**)argv);                                       \
      return result;                                                 \
    }                                                                \
    argv = orig_argv;                                                \
  }                                                                  \
  return call_real;

// Function pointers to the real exec functions
DECLARE_REAL_FUNC(execve, int, const char* pathname, char* const argv[],
                  char* const envp[])
DECLARE_REAL_FUNC(execv, int, const char* pathname, char* const argv[])
DECLARE_REAL_FUNC(execvp, int, const char* file, char* const argv[])
DECLARE_REAL_FUNC(execvpe, int, const char* file, char* const argv[],
                  char* const envp[])
DECLARE_REAL_FUNC(execl, int, const char* pathname, const char* arg, ...)
DECLARE_REAL_FUNC(execlp, int, const char* file, const char* arg, ...)
DECLARE_REAL_FUNC(execle, int, const char* pathname, const char* arg, ...)
DECLARE_REAL_FUNC(posix_spawn, int, pid_t* pid, const char* path,
                  const posix_spawn_file_actions_t* file_actions,
                  const posix_spawnattr_t* attrp, char* const argv[],
                  char* const envp[])

// Load the real exec functions
static void load_real_exec_functions() {
  LOAD_REAL_FUNC(execve, int, const char*, char* const[], char* const[])
  LOAD_REAL_FUNC(execv, int, const char*, char* const[])
  LOAD_REAL_FUNC(execvp, int, const char*, char* const[])
  LOAD_REAL_FUNC(execvpe, int, const char*, char* const[], char* const[])
  LOAD_REAL_FUNC(execl, int, const char*, const char*, ...)
  LOAD_REAL_FUNC(execlp, int, const char*, const char*, ...)
  LOAD_REAL_FUNC(execle, int, const char*, const char*, ...)
  LOAD_REAL_FUNC(posix_spawn, int, pid_t*, const char*,
                 const posix_spawn_file_actions_t*, const posix_spawnattr_t*,
                 char* const[], char* const[])
}

// Global map to store git repository URLs and their commit IDs
static std::map<std::string, std::string> git_repo_commits;

// Global variable to store the last git clone PID for posix_spawn
static pid_t git_clone_pid = 0;

// Function to execute command and capture output
static std::string execute_and_capture(const std::string& command) {
  std::unique_ptr<FILE, int (*)(FILE*)> pipe(popen(command.c_str(), "r"),
                                             pclose);
  if (!pipe) return "";

  char buffer[128];
  std::string result;
  while (fgets(buffer, sizeof(buffer), pipe.get()) != nullptr) {
    result += buffer;
  }

  // Remove trailing newline
  if (!result.empty() && result.back() == '\n') {
    result.pop_back();
  }

  return result;
}

// Check if this is a git clone command and handle it
static bool handle_git_clone(char* const argv[]) {
  if (!argv || !argv[0]) return false;

  // Check if this is git command
  const char* basename = strrchr(argv[0], '/');
  if (!basename)
    basename = argv[0];
  else
    basename++;

  if (strcmp(basename, "git") != 0) return false;

  // Check if this is a clone command
  if (!argv[1] || strcmp(argv[1], "clone") != 0) return false;

  // Find the URL argument (skip flags)
  std::string repo_url;
  std::string target_dir;

  for (int i = 2; argv[i]; i++) {
    if (argv[i][0] != '-') {
      if (repo_url.empty()) {
        repo_url = argv[i];
      } else if (target_dir.empty()) {
        target_dir = argv[i];
      }
    }
  }

  if (repo_url.empty()) return false;

  printf("Intercepted git clone: %s\n", repo_url.c_str());

  // Execute the original git clone command using real posix_spawn to avoid
  // recursion First, load the real posix_spawn function
  LOAD_REAL_FUNC(posix_spawn, int, pid_t*, const char*,
                 const posix_spawn_file_actions_t*, const posix_spawnattr_t*,
                 char* const[], char* const[])

  // Build argv array for git clone
  std::vector<char*> git_argv;
  git_argv.push_back(const_cast<char*>("git"));
  for (int i = 1; argv[i]; i++) {
    git_argv.push_back(argv[i]);
  }
  git_argv.push_back(nullptr);

  pid_t git_pid;
  int clone_result = real_posix_spawn(&git_pid, "/usr/bin/git", nullptr,
                                      nullptr, git_argv.data(), environ);

  // Store the PID globally for posix_spawn to return
  git_clone_pid = git_pid;

  if (clone_result != 0) {
    printf("Git clone posix_spawn failed with error: %d\n", clone_result);
    return true;
  }

  // Wait for git clone to complete
  int status;
  waitpid(git_pid, &status, 0);

  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    printf("Git clone failed with exit code: %d\n", WEXITSTATUS(status));
    return true;
  }

  // Get commit ID from the cloned repository
  std::string work_dir;
  if (!target_dir.empty()) {
    work_dir = target_dir;
  } else {
    // Extract directory name from URL
    size_t last_slash = repo_url.find_last_of('/');
    if (last_slash != std::string::npos) {
      work_dir = repo_url.substr(last_slash + 1);
      // Remove .git suffix if present
      if (work_dir.size() > 4 &&
          work_dir.substr(work_dir.size() - 4) == ".git") {
        work_dir = work_dir.substr(0, work_dir.size() - 4);
      }
    }
  }

  if (!work_dir.empty()) {
    std::string rev_parse_cmd =
        "cd \"" + work_dir + "\" && git rev-parse HEAD 2>/dev/null";
    std::string commit_id = execute_and_capture(rev_parse_cmd);

    if (!commit_id.empty()) {
      git_repo_commits[repo_url] = commit_id;
      printf("Recorded commit for %s: %s\n", repo_url.c_str(),
             commit_id.c_str());

      // Optionally write to a file for persistence
      FILE* commit_file = fopen("/tmp/git_clone_commits.log", "a");
      if (commit_file) {
        fprintf(commit_file, "%s %s\n", repo_url.c_str(), commit_id.c_str());
        fclose(commit_file);
      }
    }
  }

  return true;
}

// Check if we should add compiler flags to this command
static bool should_add_compiler_flags(const char* pathname) {
  if (!pathname) return false;

  // Check for common compilers
  const char* basename = strrchr(pathname, '/');
  if (!basename)
    basename = pathname;
  else
    basename++;  // Skip the '/'

  // List of compiler executables we want to intercept
  const char* compilers[] = {"gcc", "g++", "clang", "clang++",
                             "cc",  "c++", nullptr};

  for (int i = 0; compilers[i]; i++) {
    if (strcmp(basename, compilers[i]) == 0) {
      return true;
    }
  }

  return false;
}

// Add arguments to the original argv
static char** add_arguments(char* const original_argv[], char* extra_flags) {
  if (!original_argv || !extra_flags) return nullptr;

  // Count original arguments
  int argc = 0;
  while (original_argv[argc]) argc++;

  // Parse extra flags (simple space-separated parsing)
  char* flags_copy = strdup((const char*)extra_flags);
  if (!flags_copy) return nullptr;

  int extra_argc = 0;
  char* token = strtok(flags_copy, " ");
  char* extra_args[64];  // Max 64 additional arguments

  while (token && extra_argc < 63) {
    extra_args[extra_argc] = strdup(token);
    extra_argc++;
    token = strtok(nullptr, " ");
  }
  free(flags_copy);

  // Create new argv with additional flags
  char** new_argv = (char**)malloc((argc + extra_argc + 1) * sizeof(char*));

  // Copy original argv[0] (program name)
  new_argv[0] = strdup(original_argv[0]);

  // Add extra flags after program name
  for (int i = 0; i < extra_argc; i++) {
    new_argv[i + 1] = extra_args[i];
  }

  // Copy remaining original arguments
  for (int i = 1; i < argc; i++) {
    new_argv[i + extra_argc] = strdup(original_argv[i]);
    if (!new_argv[i + extra_argc]) {
      // Cleanup on failure
      for (int j = 0; j < i + extra_argc; j++) {
        free(new_argv[j]);
      }
      free(new_argv);
      return nullptr;
    }
  }

  new_argv[argc + extra_argc] = nullptr;
  return new_argv;
}

// Free modified argv
static void free_argv(char** argv) {
  if (!argv) return;

  for (int i = 0; argv[i]; i++) {
    free(argv[i]);
  }
  free(argv);
}

// Intercepted exec functions
extern "C" int execve(const char* pathname, char* const argv[],
                      char* const envp[]) {
  INTERCEPT_WITH_ARGV(execve, pathname, argv, real_execve(pathname, argv, envp))
}

extern "C" int execv(const char* pathname, char* const argv[]) {
  INTERCEPT_WITH_ARGV(execv, pathname, argv, real_execv(pathname, argv))
}

extern "C" int execvp(const char* file, char* const argv[]) {
  INTERCEPT_WITH_ARGV(execvp, file, argv, real_execvp(file, argv))
}

extern "C" int execvpe(const char* file, char* const argv[],
                       char* const envp[]) {
  INTERCEPT_WITH_ARGV(execvpe, file, argv, real_execvpe(file, argv, envp))
}

// Intercepted posix_spawn function
extern "C" int posix_spawn(pid_t* pid, const char* path,
                           const posix_spawn_file_actions_t* file_actions,
                           const posix_spawnattr_t* attrp, char* const argv[],
                           char* const envp[]) {
  LOAD_REAL_FUNC(posix_spawn, int, pid_t*, const char*,
                 const posix_spawn_file_actions_t*, const posix_spawnattr_t*,
                 char* const[], char* const[])

  // Simple output for posix_spawn
  printf("Intercepted posix_spawn: %s\n", path);

  // Check for git clone handling
  if (handle_git_clone(argv)) {
    return real_posix_spawn(pid, "/usr/bin/true", nullptr, nullptr, argv, envp);
  }

  // Check if we should add compiler flags
  if (should_add_compiler_flags(path)) {
    char* const* orig_argv = (char**)argv;
    argv = add_arguments(argv, getenv("REPROBUILD_COMPILER_FLAGS"));
    if (argv) {
      int result = real_posix_spawn(pid, path, file_actions, attrp, argv, envp);
      free_argv((char**)argv);
      return result;
    }
    argv = orig_argv;
  }

  // Call the real posix_spawn function
  return real_posix_spawn(pid, path, file_actions, attrp, argv, envp);
}
