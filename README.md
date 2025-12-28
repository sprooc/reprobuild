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

# Set output file
./build/reprobuild -o build_record.yaml make
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
