# Contributing to FlySight Viewer

Thank you for your interest in contributing to FlySight Viewer! This document provides information about the build process, CI/CD pipeline, and how to create releases.

## Table of Contents

- [Development Setup](#development-setup)
- [Building Locally](#building-locally)
- [CI/CD Pipeline](#cicd-pipeline)
- [Creating Releases](#creating-releases)
- [Troubleshooting](#troubleshooting)

## Development Setup

### Prerequisites

FlySight Viewer requires the following tools and dependencies:

| Dependency | Version | Platform |
|------------|---------|----------|
| CMake | 3.18+ | All |
| Qt | 6.7.x | All |
| Python | 3.8+ | All |
| C++ Compiler | C++17 compatible | All |
| Ninja | Latest | All (recommended) |

### Platform-Specific Setup

#### Windows

1. Install Visual Studio 2022 with C++ workload
2. Install Qt 6.7.x with the following modules:
   - Qt Location
   - Qt Positioning
   - Qt Multimedia
3. Install CMake 3.18+ and Ninja

#### macOS

```bash
# Install Xcode Command Line Tools
xcode-select --install

# Install dependencies via Homebrew
brew install cmake ninja python@3.13

# Install Qt via aqtinstall or Qt Online Installer
pip3 install aqtinstall
aqt install-qt mac desktop 6.7.3 -m qtlocation qtpositioning qtmultimedia
```

#### Linux (Ubuntu 22.04)

```bash
# Install build tools
sudo apt-get update
sudo apt-get install -y cmake ninja-build g++

# Install Qt dependencies
sudo apt-get install -y \
    libxcb-cursor0 \
    libxcb-icccm4 \
    libxcb-image0 \
    libxcb-keysyms1 \
    libxcb-randr0 \
    libxcb-render-util0 \
    libxcb-shape0 \
    libxcb-xinerama0 \
    libxcb-xkb1 \
    libxkbcommon-x11-0 \
    libfuse2 \
    libgl1-mesa-dev \
    libegl1-mesa-dev

# Install Qt via aqtinstall
pip3 install aqtinstall
aqt install-qt linux desktop 6.7.3 -m qtlocation qtpositioning qtmultimedia
```

## Building Locally

### Clone the Repository

```bash
git clone --recursive https://github.com/your-username/flysight-viewer-2.git
cd flysight-viewer-2
```

If you already cloned without `--recursive`:

```bash
git submodule update --init --recursive
```

### Build Third-Party Dependencies

The first build requires building third-party dependencies (GeographicLib, KDDockWidgets):

```bash
# Configure the superbuild
cmake -G Ninja -B build-third-party -S . \
    -DCMAKE_BUILD_TYPE=Release \
    -DFLYSIGHT_BUILD_THIRD_PARTY=ON \
    -DFLYSIGHT_THIRD_PARTY_ONLY=ON

# Build third-party dependencies
cmake --build build-third-party --parallel
```

This step only needs to be done once. The built libraries are cached in `third-party/*-install` directories.

### Build the Application

```bash
# Configure the application
cmake -G Ninja -B build -S src \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=build/install \
    -DTHIRD_PARTY_DIR="$(pwd)/third-party" \
    -DGEOGRAPHIC_ROOT="$(pwd)/third-party/GeographicLib-install" \
    -DKDDW_ROOT="$(pwd)/third-party/KDDockWidgets-install"

# Build
cmake --build build --parallel

# Install (creates deployment package)
cmake --install build
```

### Platform-Specific Packaging

#### Windows (ZIP)

```bash
cd build
cpack -G ZIP -C Release
```

#### macOS (DMG)

```bash
cd build
cpack -G DragNDrop -C Release
```

#### Linux (AppImage)

```bash
# The AppImage is created during install, or run:
cmake --build build --target appimage
```

## CI/CD Pipeline

### Overview

The CI/CD pipeline is defined in `.github/workflows/build.yml` and automatically:

1. Builds the application on Windows, macOS (ARM64), and Linux
2. Creates platform-specific packages (ZIP, DMG, AppImage)
3. Uploads build artifacts
4. Creates draft releases when version tags are pushed

### Build Matrix

| Platform | Runner | Architecture | Package Format |
|----------|--------|--------------|----------------|
| Windows | windows-2022 | x64 | ZIP |
| macOS | macos-14 | arm64 | DMG |
| Linux | ubuntu-22.04 | x86_64 | AppImage |

### Triggers

The workflow runs on:

- **Push**: Any branch push triggers a build
- **Pull Request**: All PRs are built for validation
- **Manual**: Use "Run workflow" button in GitHub Actions
- **Tags**: Tags starting with `v` (e.g., `v1.0.0`) trigger release creation

### Caching

Third-party dependencies are cached to speed up builds. The cache key is based on:

- Platform (OS)
- Hash of `cmake/ThirdPartySuperbuild.cmake` and `.gitmodules`

If you modify third-party build configuration, the cache will be invalidated automatically.

### Build Artifacts

Each successful build uploads artifacts with 30-day retention:

- `FlySightViewer-Windows-x64` - Windows ZIP package
- `FlySightViewer-macOS-arm64` - macOS DMG installer
- `FlySightViewer-Linux-x86_64` - Linux AppImage

## Creating Releases

### Version Tagging

To create a new release:

1. Update version number in `src/CMakeLists.txt` (if needed):
   ```cmake
   project(FlySightViewer VERSION 1.0.0 LANGUAGES CXX)
   ```

2. Commit and push your changes:
   ```bash
   git add .
   git commit -m "Prepare release v1.0.0"
   git push origin main
   ```

3. Create and push a version tag:
   ```bash
   git tag v1.0.0
   git push origin v1.0.0
   ```

### Release Process

When a `v*` tag is pushed:

1. The CI builds all three platforms
2. If all builds succeed, a draft release is created
3. Platform artifacts are attached to the release
4. Release notes are auto-generated from commit history

### Publishing the Release

1. Go to the [Releases page](../../releases)
2. Find the draft release created by CI
3. Review the release notes and artifacts
4. Edit the release notes if needed
5. Click "Publish release"

### Versioning Scheme

We recommend [Semantic Versioning](https://semver.org/):

- **Major** (X.0.0): Breaking changes
- **Minor** (0.X.0): New features, backward compatible
- **Patch** (0.0.X): Bug fixes, backward compatible

## Troubleshooting

### Common Build Issues

#### CMake can't find Qt

Ensure Qt is in your PATH or set `CMAKE_PREFIX_PATH`:

```bash
cmake -B build -S src -DCMAKE_PREFIX_PATH=/path/to/Qt/6.7.3/gcc_64
```

#### Third-party build fails

Clean and rebuild third-party dependencies:

```bash
rm -rf third-party/*-install third-party/*-build build-third-party
# Then rebuild from scratch
```

#### AppImage creation fails

Ensure FUSE is available or set the extraction flag:

```bash
export APPIMAGE_EXTRACT_AND_RUN=1
```

### CI Build Failures

1. Check the workflow run logs in GitHub Actions
2. Look for error messages in the failing step
3. Try reproducing locally with the same commands
4. If caching issues are suspected, manually clear the cache in repository settings

### Getting Help

- Open an issue for bug reports
- Use discussions for questions
- Check existing issues before creating new ones
