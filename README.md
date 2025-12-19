# ReproBuild - C++ Project Template

A modern C++ project template with CMake and Make build system.

## Project Structure

```
reprobuild/
├── CMakeLists.txt      # CMake configuration file
├── Makefile           # Convenience wrapper for build commands
├── README.md          # Project documentation
├── .gitignore         # Git ignore file
├── include/           # Header files
│   └── calculator.h   # Calculator class header
├── src/               # Source files
│   ├── main.cpp       # Main application entry point
│   └── calculator.cpp # Calculator class implementation
└── build/             # Build output directory (auto-generated)
```

## Build Requirements

- CMake 3.10 or higher
- C++17 compatible compiler (GCC, Clang, or MSVC)
- Make

## Build Instructions

### Using Make (Recommended)

```bash
# Build the project
make

# Or build and run
make run

# Clean build files
make clean

# Build in debug mode
make debug

# Show all available targets
make help
```

### Using CMake directly

```bash
# Create build directory
mkdir -p build
cd build

# Configure
cmake -DCMAKE_BUILD_TYPE=Release ..

# Build
make -j$(nproc)

# Run
./bin/ReproBuild
```

## Features

- **Modern C++17** standard
- **CMake** build system with proper configuration
- **Cross-platform** compatibility
- **Header/Source separation** with proper include guards
- **Comprehensive documentation** with Doxygen-style comments
- **Error handling** with exception safety
- **Compiler warnings** enabled for better code quality

## Example Usage

The project includes a simple calculator demo that demonstrates:
- Class instantiation and destruction
- Basic arithmetic operations
- Exception handling
- Input validation

## Development

### Adding New Files

1. Add header files to the `include/` directory
2. Add source files to the `src/` directory
3. CMake will automatically detect and include new files

### Build Types

- **Release**: Optimized for performance (`-O3`)
- **Debug**: Optimized for debugging (`-g -O0`)
- **RelWithDebInfo**: Release with debug information
- **MinSizeRel**: Optimized for size

Example:
```bash
make BUILD_TYPE=Debug
```

## License

This project template is free to use and modify.
