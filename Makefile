# Default build type
BUILD_TYPE ?= Release

# Build directory
BUILD_DIR = build

# Project name (from CMakeLists.txt)
PROJECT_NAME = reprobuild

# Default target
.PHONY: all
all: configure build

# Configure CMake
.PHONY: configure
configure:
	@echo "Configuring CMake..."
	@mkdir -p $(BUILD_DIR)
	@cd $(BUILD_DIR) && cmake -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) ..

# Build the project
.PHONY: build
build: configure
	@echo "Building project..."
	@cd $(BUILD_DIR) && make -j$$(nproc)

# Run the main executable
.PHONY: run
run: build
	@echo "Running $(PROJECT_NAME)..."
	@$(BUILD_DIR)/bin/$(PROJECT_NAME)

# Build and run tests
.PHONY: test
test: build
	@echo "Running tests..."
	@if [ -f "$(BUILD_DIR)/bin/reprobuild_tests" ]; then \
        $(BUILD_DIR)/bin/reprobuild_tests; \
    else \
        echo "Tests not found. Make sure tests are built."; \
        exit 1; \
    fi

# Run tests with CTest (alternative)
.PHONY: ctest
ctest: build
	@echo "Running tests with CTest..."
	@cd $(BUILD_DIR) && ctest --output-on-failure

# Clean build artifacts
.PHONY: clean
clean:
	@echo "Cleaning build directory..."
	@rm -rf $(BUILD_DIR)

# Debug build
.PHONY: debug
debug:
	@echo "Building in debug mode..."
	@$(MAKE) BUILD_TYPE=Debug configure build

# Debug build and test
.PHONY: debug-test
debug-test:
	@echo "Building and testing in debug mode..."
	@$(MAKE) BUILD_TYPE=Debug configure build test

# Install dependencies
.PHONY: deps
deps:
	@echo "Installing dependencies system-wide..."
	@sudo apt update
	@sudo apt install -y libgtest-dev libgmock-dev libyaml-cpp-dev

# Show help
.PHONY: help
help:
	@echo "Available targets:"
	@echo "  all           - Configure and build (default)"
	@echo "  configure     - Run CMake configuration"
	@echo "  build         - Build the project"
	@echo "  run           - Build and run the main executable"
	@echo "  test          - Build and run unit tests"
	@echo "  ctest         - Run tests using CTest"
	@echo "  clean         - Clean build artifacts"
	@echo "  debug         - Build in debug mode"
	@echo "  debug-test    - Build and test in debug mode"
	@echo "  install-gtest - Install Google Test system-wide"
	@echo "  help          - Show this help message"
	@echo ""
	@echo "Environment variables:"
	@echo "  BUILD_TYPE    - Build type (Release|Debug|RelWithDebInfo|MinSizeRel)"
	@echo ""
	@echo "Example usage:"
	@echo "  make test              # Build and run tests"
	@echo "  make debug-test        # Debug build and test"
	@echo "  make BUILD_TYPE=Debug test  # Debug build and test (alternative)"
