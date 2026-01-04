#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

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
  // printf("Intercepted %s: %s\n", #name, pathname);

#define INTERCEPT_WITH_ARGV(name, pathname, argv, call_real)         \
  load_real_exec_functions();                                        \
  INTERCEPT_LOG(name, pathname)                                      \
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

#define INTERCEPT_VARARGS(name, pathname, arg, real_func_call)    \
  load_real_exec_functions();                                     \
  INTERCEPT_LOG(name, pathname)                                   \
  va_list args;                                                   \
  va_start(args, arg);                                            \
  char** argv = varargs_to_argv(arg, args);                       \
  va_end(args);                                                   \
  if (!argv) return -1;                                           \
  if (should_add_compiler_flags(pathname)) {                      \
    char** modified_argv =                                        \
        add_arguments(argv, getenv("REPROBUILD_COMPILER_FLAGS")); \
    if (modified_argv) {                                          \
      int result = real_func_call;                                \
      free_argv(modified_argv);                                   \
      free_argv(argv);                                            \
      return result;                                              \
    }                                                             \
  }                                                               \
  int result = real_func_call;                                    \
  free_argv(argv);                                                \
  return result;

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

// Load the real exec functions
static void load_real_exec_functions() {
  LOAD_REAL_FUNC(execve, int, const char*, char* const[], char* const[])
  LOAD_REAL_FUNC(execv, int, const char*, char* const[])
  LOAD_REAL_FUNC(execvp, int, const char*, char* const[])
  LOAD_REAL_FUNC(execvpe, int, const char*, char* const[], char* const[])
  LOAD_REAL_FUNC(execl, int, const char*, const char*, ...)
  LOAD_REAL_FUNC(execlp, int, const char*, const char*, ...)
  LOAD_REAL_FUNC(execle, int, const char*, const char*, ...)
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
