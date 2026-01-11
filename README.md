## Building

```bash
# Install dependencies
make deps
# Build the project
make
# Run tests
make ctest
```

## Usage

### Basic Usage

```bash
# Track a simple build command
./build/reprobuild make

# Specify custom output file
./build/reprobuild -o custom_record.yaml make

# Set log directory
./build/reprobuild -l ./logs make
```

## Build Record YAML
输出是一个YAML文件，包含构建过程中的所有依赖和构建产物的信息。[示例文件](./build_record.yaml)是构建本项目的过程中生成的，即在此目录下执行`reprobuild make -j8`。

### Metadata Section
`Metadata`部分包含构建环境的关键信息：
```yaml
metadata:
  architecture: x86_64                    # System architecture (uname -m)
  distribution: Ubuntu 25.04     # OS distribution info
  build_path: /home/user/project          # Absolute build directory path
  build_timestamp: 2025-12-26T09:43:00+00:00  # ISO format timestamp
  hostname: buildserver                   # Build machine hostname
  locale: en_US.UTF-8                  # System locale setting
  umask: 0002                          # File permission mask
```

#### 各项说明

- **architecture & distribution**: OS版本和硬件信息直接影响构建产物。必须保证`uname`输出相同才能确保可重现构建。
- **build_timestamp**: 通过[SOURCE_DATE_EPOCH](https://reproducible-builds.org/docs/source-date-epoch/)环境变量固定构建时间。许多工具(如gcc)在构建产物中写入构建时间，不统一时间会导致产物不一致。
- **build_path**: 记录构建路径用于[Build path](https://reproducible-builds.org/docs/build-path/)标准化。带有调试信息(-g)的二进制文件会包含文件绝对路径信息，需要使用`-ffile-prefix-map`选项进行路径映射。在容器中复现环境时，一种做法是设置环境变量`CFLAGS`, `CXXFLAGS`，例如`export CFLAGS="-ffile-prefix-map=/home/user/project=."`。(参考[setCompilerOptions函数](./src/utils.cpp)
- **hostname**: 记录构建机器信息，确保环境一致性。
- **locale**: Locale影响格式化、排序方式，可能导致构建产物不一致，因此需要固定Locale设置。
- **umask**: 文件权限的不同可能导致构建产物不同，使用umask统一默认权限。

### Dependencies Section
列出所有构建过程中使用的依赖项及其版本和校验信息：

```yaml
dependencies:
  - name: libc6-dev
    path: /usr/lib/gcc/x86_64-linux-gnu/14/../../../x86_64-linux-gnu/libm.so
    version: 2.41-6ubuntu1.2
    hash: 55de4a3e12e17c3e2b18eb95ee21b3f46a8893ea6cab68c0a1c3a5caaf32e6cf
  - name: cmake
    path: /usr/bin/cmake
    version: 3.31.6-1ubuntu1
    hash: 63e339c0858912c62402a9e5474e5c5f27037e613129011a4803d1b432322566
```

#### 各项说明
- **path**: 依赖的共享库或可执行文件的绝对路径
- **name**: 依赖所属包的名称。目前是针对Ubuntu系统实现的，可使用`apt`安装相应的包。
- **version**: 依赖包的版本号
- **hash**: 依赖文件的SHA256校验和，用于验证依赖文件。直接使用`sha256sum <path>`命令计算

### Artifacts Section
列出所有构建产物及其校验信息：

```yaml
artifacts:
  - path: build/bin/reprobuild_tests
    hash: fd4fc2a2c5d70296d38ec60acc3df7d4c5f6ea6526302669536d12ff74737a32
    type: executable
  - path: build/reprobuild
    hash: e3172f69aa3aad298a5b6f8f5f80f34ff82b33457ab6d8d3292c986bde43ffbb
    type: executable
```

#### 各项说明
- **path**: 构建产物的路径
- **hash**: 构建产物的SHA256校验和，用于验证产物一致性。实验的目标就是在容器复现构建环境中生成的产物与原始产物的hash值一致。
- **type**: 构建产物的类型（executable/library）

### Git Commit IDs Section
列出构建过程中使用的Git仓库及其对应的提交ID：
```yaml
git_commit_ids:
  - repo: https://github.com/sprooc/reprobuild.git
    commit_id: bc23d9de3e3bb5485f7fb50ea6a81a49f788129e
  - repo: xxx
    commit_id: xxx
```

在重新构建时，拦截`git clone`命令，并使用对应的提交ID检出代码，确保源代码版本一致。

## Notes

### 2026/1/4
1. yaml文件中添加了build_cmd字段，记录了具体的构建命令，方便复现。
2. 添加了random_seed，用于在构建过程中传递给编译器，确保每次构建时使用相同的随机种子。例如`-frandom-seed=0`。
3. 有些项目不会将`CFLAGS`等环境变量传递给编译器，导致-ffile-prefix-map等选项不能生效。现在使用了`LD_PRELOAD`的方式拦截`execve`调用，对所有编译器命令添加这些选项，确保路径映射等选项生效。具体做法：
    - 设置环境变量`LD_PRELOAD`，指向拦截库`libreprobuild_interceptor.so`。
    - 将之前设置到`CFLAGS`的值同样设置到环境变量`REPROBUILD_COMPILER_FLAGS`，包含需要传递给编译器的选项，例如`-ffile-prefix-map=/home/user/project=.`和`-frandom-seed=0`。拦截函数会读取这个环境变量，并将其内容添加到编译器命令行参数中。

### 2026/1/10
添加了git_commit_ids字段，记录构建过程中使用的Git仓库及其对应的提交ID。在重新构建时，拦截`git clone`命令，并使用对应的commit_id将代码滚回对应版本，确保源代码一致。
拦截和回滚功能已在`libreprobuild_interceptor.so`中实现。需要在重建时创建一个文本文件，每一行为`<repo_url> <commit_id>`，空格隔开([参考](./tests/git-test/git_log))。并设置环境变量`REPROBUILD_LOG_GIT_CLONESG`指向该文件路径。