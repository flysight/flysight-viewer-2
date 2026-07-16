# FlySight Viewer 2

A desktop application for viewing and analyzing FlySight GPS data with advanced trajectory smoothing, interactive plots, and video synchronization.

## Table of Contents

- [Quick Start](#quick-start)
- [Prerequisites](#prerequisites)
- [Build Instructions](#build-instructions)
  - [Full Build (Third-Party + Application)](#full-build-third-party--application)
  - [Third-Party Only Build](#third-party-only-build)
  - [Application Only Build](#application-only-build)
  - [Build Options](#build-options)
  - [Build Output Locations](#build-output-locations)
- [Clean Targets](#clean-targets)
  - [Individual Clean Targets](#individual-clean-targets)
  - [Manual Reset](#manual-reset)
  - [Clean Target Reference](#clean-target-reference)
- [Project Structure](#project-structure)
- [Deployment](#deployment)
  - [How It Works](#how-it-works)
  - [Windows Deployment](#windows-deployment)
  - [macOS Deployment](#macos-deployment)
  - [Linux Deployment](#linux-deployment)
  - [CI/CD](#cicd)
  - [CI/CD Secrets](#cicd-secrets)
  - [Package Structures](#package-structures)
  - [Deployment Tips](#deployment-tips)
- [Troubleshooting](#troubleshooting)

## Quick Start

**Windows:**

```bash
cmake -G "Visual Studio 17 2022" -A x64 -B build -S . -DCMAKE_PREFIX_PATH="C:/Qt/6.9.3/msvc2022_64" -DGOOGLE_MAPS_API_KEY="your-api-key"
cmake --build build --config Release
cmake --install build --config Release --prefix dist
```

**macOS (MacPorts):**

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/opt/local/libexec/qt6 \
  -DPython_ROOT_DIR=/opt/local \
  -DPython_EXECUTABLE=/opt/local/bin/python3.13 \
  -DGOOGLE_MAPS_API_KEY="your-api-key"
cmake --build build
cmake --install build --prefix dist
```

**Linux:**

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release -DGOOGLE_MAPS_API_KEY="your-api-key"
cmake --build build
cmake --install build --prefix dist
```

The install step produces a self-contained deployment (ZIP-ready directory on Windows, `.app` bundle on macOS, AppDir on Linux) with Qt libraries, third-party dependencies, and a bundled Python runtime all included.

## Prerequisites

### Required Software

| Software | Version | Windows | macOS | Linux |
|----------|---------|---------|-------|-------|
| CMake | 3.18+ | [cmake.org](https://cmake.org/download/) | `brew install cmake` | `apt install cmake` |
| C++ Compiler | C++17 | Visual Studio 2022 | Xcode Command Line Tools | GCC 9+ or Clang 10+ |
| Ninja | (optional) | [ninja-build.org](https://ninja-build.org/) | `brew install ninja` | `apt install ninja-build` |
| Qt | 6.x | [Qt Online Installer](https://www.qt.io/download-qt-installer) | Qt Online Installer | Qt Online Installer |
| Boost | 1.65+ | [Prebuilt binaries](https://sourceforge.net/projects/boost/files/boost-binaries/) | `brew install boost` | `apt install libboost-all-dev` |
| Python | 3.10+ | [python.org](https://www.python.org/downloads/) | `port install python313` or `brew install python` | `apt install python3-dev` |
| patchelf | (Linux only) | N/A | N/A | `apt install patchelf` |

On Windows, Visual Studio is required even when using Ninja as the generator, since MSVC provides the compiler toolchain. You can also use the Visual Studio generator directly (`-G "Visual Studio 17 2022" -A x64`) instead of Ninja.

### Qt Components

The following Qt 6 components are required:

- Core, Widgets, PrintSupport
- QuickWidgets, Quick, Qml, QuickControls2
- WebEngineWidgets, WebChannel
- Multimedia, MultimediaWidgets

Qt is typically installed via the Qt Online Installer. Ensure the Qt installation is discoverable by CMake (either in your PATH or via `CMAKE_PREFIX_PATH`).

**MacPorts (macOS):** MacPorts installs Qt6 to `/opt/local/libexec/qt6`, which is not on CMake's default search path. You must pass the prefix explicitly:

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/opt/local/libexec/qt6
```

This is forwarded automatically to third-party and application sub-builds.

### Boost Components

Required Boost components: header-only (boost::geometry).

If Boost is not found automatically, set `BOOST_ROOT`:

```bash
# Windows (prebuilt binaries)
cmake ... -DBOOST_ROOT="C:/Program Files/Boost/boost_1_87_0"

# macOS (Homebrew)
cmake ... -DBOOST_ROOT=/opt/homebrew   # Apple Silicon
cmake ... -DBOOST_ROOT=/usr/local      # Intel

# Linux (apt)
cmake ... -DBOOST_ROOT=/usr
```

### Python

Python 3.10+ with development headers is required for pybind11 embedding. The build downloads a matching [python-build-standalone](https://github.com/astral-sh/python-build-standalone) release to bundle with the application, so the build-time Python version must be available in that project's releases.

- **Windows:** Install from [python.org](https://www.python.org/downloads/). Check "Download debug binaries" during installation if you plan to build Debug configurations.
- **macOS:** The system Python (`/usr/bin/python3`, typically 3.9.6) is too old — python-build-standalone no longer publishes 3.9.x builds. Install a modern Python via MacPorts (`port install python313`) or Homebrew (`brew install python`), then point CMake at it (see below).
- **Linux:** System Python is usually sufficient. Ensure development headers are available (`Python.h`).

**MacPorts (macOS):** MacPorts Python is not on CMake's default search path. You must tell CMake where to find it:

```bash
cmake ... -DPython_ROOT_DIR=/opt/local -DPython_EXECUTABLE=/opt/local/bin/python3.13
```

These flags are forwarded automatically to sub-builds.

## Build Instructions

### Full Build (Third-Party + Application)

This is the default build mode that builds all third-party dependencies (GeographicLib, KDDockWidgets) and the main application.

**Windows:**

```bash
cmake -G "Visual Studio 17 2022" -A x64 -B build -S . -DCMAKE_PREFIX_PATH="C:/Qt/6.9.3/msvc2022_64" -DGOOGLE_MAPS_API_KEY="your-api-key"
cmake --build build --config Release
```

**macOS (MacPorts):**

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/opt/local/libexec/qt6 \
  -DPython_ROOT_DIR=/opt/local \
  -DPython_EXECUTABLE=/opt/local/bin/python3.13 \
  -DGOOGLE_MAPS_API_KEY="your-api-key"
cmake --build build
```

**Linux:**

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release -DGOOGLE_MAPS_API_KEY="your-api-key"
cmake --build build
```

### Third-Party Only Build

Build only the third-party dependencies without building the application:

**Windows:**

```bash
cmake -G "Visual Studio 17 2022" -A x64 -B build -S . -DCMAKE_PREFIX_PATH="C:/Qt/6.9.3/msvc2022_64" -DFLYSIGHT_THIRD_PARTY_ONLY=ON
cmake --build build --config Release
```

**macOS / Linux:**

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release -DFLYSIGHT_THIRD_PARTY_ONLY=ON
cmake --build build
```

Alternatively, build from the `third-party/` directory directly:

```bash
cd third-party
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### Application Only Build

If third-party dependencies are already built and installed in the `third-party/*-install` directories:

**Windows:**

```bash
cmake -G "Visual Studio 17 2022" -A x64 -B build -S . -DCMAKE_PREFIX_PATH="C:/Qt/6.9.3/msvc2022_64" -DGOOGLE_MAPS_API_KEY="your-api-key" -DFLYSIGHT_BUILD_THIRD_PARTY=OFF
cmake --build build --config Release
```

**macOS / Linux:**

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release -DGOOGLE_MAPS_API_KEY="your-api-key" -DFLYSIGHT_BUILD_THIRD_PARTY=OFF
cmake --build build
```

### Build Options

| Option | Default | Description |
|--------|---------|-------------|
| `FLYSIGHT_BUILD_THIRD_PARTY` | `ON` | Build third-party dependencies (GeographicLib, KDDockWidgets) |
| `FLYSIGHT_BUILD_APP` | `ON` | Build the main FlySight Viewer application |
| `FLYSIGHT_THIRD_PARTY_ONLY` | `OFF` | Build only third-party dependencies (automatically disables app build) |
| `GOOGLE_MAPS_API_KEY` | (none) | Google Maps JavaScript API key for the map view |

**Path Variables:**

| Variable | Default | Description |
|----------|---------|-------------|
| `THIRD_PARTY_DIR` | `<project>/third-party` | Root directory for third-party dependencies |
| `GEOGRAPHIC_INSTALL_DIR` | `<third-party>/GeographicLib-install` | GeographicLib installation directory |
| `KDDW_INSTALL_DIR` | `<third-party>/KDDockWidgets-install` | KDDockWidgets installation directory |
| `BOOST_ROOT` | Platform-dependent | Boost root directory |

### Build Output Locations

After a successful build:

| Component | Install Directory |
|-----------|-------------------|
| GeographicLib | `third-party/GeographicLib-install/` |
| KDDockWidgets | `third-party/KDDockWidgets-install/` |
| Application | `build/` (executables in build root) |

## Clean Targets

### Individual Clean Targets

Clean individual third-party components:

```bash
cmake --build build --target clean-GeographicLib
cmake --build build --target clean-KDDockWidgets
cmake --build build --target clean-third-party   # all third-party
```

When building from the `third-party/` directory, `clean-all` is available as an alias for `clean-third-party`.

### Manual Reset

For a complete reset, delete the build and install directories:

**Windows (PowerShell):**

```powershell
Remove-Item -Recurse -Force build
Remove-Item -Recurse -Force third-party\GeographicLib-build, third-party\GeographicLib-install
Remove-Item -Recurse -Force third-party\KDDockWidgets-build, third-party\KDDockWidgets-install
```

**macOS / Linux:**

```bash
rm -rf build
rm -rf third-party/{GeographicLib,KDDockWidgets}-{build,install}
```

### Clean Target Reference

| Target | Description |
|--------|-------------|
| `clean-GeographicLib` | Removes `third-party/GeographicLib-build` and `third-party/GeographicLib-install` |
| `clean-KDDockWidgets` | Removes `third-party/KDDockWidgets-build` and `third-party/KDDockWidgets-install` |
| `clean-third-party` | Cleans all third-party components |

## Project Structure

```
flysight-viewer-2/
├── CMakeLists.txt                         # Root superbuild configuration
├── README.md                              # This file
├── cmake/
│   ├── ThirdPartySuperbuild.cmake         # ExternalProject definitions for GeographicLib, KDDockWidgets
│   ├── BoostDiscovery.cmake               # Cross-platform Boost discovery
│   ├── QtPathDiscovery.cmake              # Finds Qt plugin/QML directories
│   ├── GenerateIcon.cmake                 # Application icon generation
│   ├── BundlePythonWindows.cmake          # Downloads/installs Python embeddable package
│   ├── BundlePythonMacOS.cmake            # Downloads/installs python-build-standalone
│   ├── BundlePythonLinux.cmake            # Downloads/installs python-build-standalone
│   ├── DeployThirdPartyWindows.cmake      # Copies GeographicLib/KDDW DLLs
│   ├── DeployThirdPartyMacOS.cmake        # Copies dylibs to Frameworks, fixes paths
│   ├── DeployThirdPartyLinux.cmake        # Copies .so files to AppDir, sets RPATH
│   ├── CreateAppDir.cmake                 # Creates Linux AppDir structure with AppRun
│   ├── CreateAppImage.cmake               # Generates AppImage via appimagetool
│   ├── RunLinuxDeployQt.cmake             # Optional linuxdeployqt integration
│   ├── fix_macos_rpaths.sh                # Post-install rpath repair for macOS bundles
│   └── diagnose_macos_bundle.sh           # Diagnostic tool for macOS bundle issues
├── src/
│   └── CMakeLists.txt                     # Main application build configuration
├── third-party/
│   ├── CMakeLists.txt                     # Standalone third-party build
│   ├── GeographicLib/                     # GeographicLib source
│   ├── KDDockWidgets/                     # KDDockWidgets submodule
│   ├── QCustomPlot/                       # QCustomPlot library
│   └── pybind11/                          # pybind11 submodule
├── third-party/GeographicLib-install/     # [Build artifact] GeographicLib installation
├── third-party/KDDockWidgets-install/     # [Build artifact] KDDockWidgets installation
└── build/                                 # [Build artifact] CMake build directory
```

## Deployment

FlySight Viewer deploys as a self-contained application with a bundled Python interpreter, requiring no user-installed dependencies. **The `cmake --install` command handles all deployment steps automatically**, including Qt library bundling, third-party library copying, Python bundling, and platform-specific path fixups.

### How It Works

`cmake --install` triggers platform-specific install rules that handle:

| Concern | Windows | macOS | Linux |
|---------|---------|-------|-------|
| Qt libraries & plugins | `qt_generate_deploy_app_script` | `qt_generate_deploy_app_script` | `qt_generate_deploy_app_script` |
| map.html (Google Maps) | Installed to `resources/` | Installed to `Resources/resources/` | Installed to `usr/resources/` |
| Third-party libs (GeographicLib, KDDW) | DLLs copied to app root | dylibs copied to `Frameworks/` | `.so` files copied to `usr/lib/` |
| Python runtime | Embeddable package downloaded | python-build-standalone downloaded | python-build-standalone downloaded |
| Library path fixups | N/A (flat DLL layout) | `install_name_tool` (automatic) | `patchelf` (automatic) |
| `qt.conf` | Generated in app root | Generated in `Resources/` | Generated in `usr/bin/` |
| AppDir + AppRun | N/A | N/A | Created automatically |

There is no need to manually run `macdeployqt`, `windeployqt`, copy libraries, or fix rpaths. The CMake install rules handle all of this.

### Windows Deployment

```bash
# 1. Build
cmake -G "Visual Studio 17 2022" -A x64 -B build -S . -DCMAKE_PREFIX_PATH="C:/Qt/6.9.3/msvc2022_64" -DGOOGLE_MAPS_API_KEY="your-api-key"
cmake --build build --config Release

# 2. Install (creates self-contained deployment)
cmake --install build --config Release --prefix dist

# 3. Package (ZIP)
cpack --config build/FlySightViewer-build/CPackConfig.cmake -G ZIP -C Release
```

The install step produces a flat directory with the executable, all DLLs, Qt plugins, QML modules, and the bundled Python runtime.

### macOS Deployment

```bash
# 1. Build (MacPorts example — adjust Python path for Homebrew)
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/opt/local/libexec/qt6 \
  -DPython_ROOT_DIR=/opt/local \
  -DPython_EXECUTABLE=/opt/local/bin/python3.13 \
  -DGOOGLE_MAPS_API_KEY="your-api-key"
cmake --build build

# 2. Install (creates .app bundle with Frameworks, Python, etc.)
#    The install step automatically ad-hoc signs all binaries, so the
#    app is immediately runnable for development and testing.
cmake --install build --prefix dist
```

**Distribution builds** require a Developer ID certificate. Pass your signing identity at configure time — the install step will sign everything with your certificate instead of ad-hoc, including `--timestamp` and `--options runtime` (required for notarization):

```bash
# Tip: Run `security find-identity -v -p codesigning` to list your
# installed certificates. The 10-character code in parentheses after
# "Developer ID Application: Name (XXXXXXXXXX)" is your team-id.

# 1. Configure with signing identity
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/opt/local/libexec/qt6 \
  -DPython_ROOT_DIR=/opt/local \
  -DPython_EXECUTABLE=/opt/local/bin/python3.13 \
  -DGOOGLE_MAPS_API_KEY="your-api-key" \
  -DFLYSIGHT_CODESIGN_IDENTITY="Developer ID Application: Your Name (XXXXXXXXXX)"
cmake --build build

# 2. Install (creates signed .app bundle)
cmake --install build --prefix dist

# 3. Create DMG
hdiutil create -volname "FlySight Viewer" \
  -srcfolder "dist/FlySight Viewer.app" \
  -ov -format UDZO FlySightViewer.dmg

# 4. Notarize
xcrun notarytool submit FlySightViewer.dmg \
  --apple-id your@email.com \
  --team-id XXXXXXXXXX \
  --password @keychain:notary-password \
  --wait
xcrun stapler staple FlySightViewer.dmg
```

### Linux Deployment

```bash
# 1. Build
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release -DGOOGLE_MAPS_API_KEY="your-api-key"
cmake --build build

# 2. Install (creates AppDir with AppRun, libraries, Python, etc.)
cmake --install build --prefix dist

# 3. Create AppImage
cmake --build build --target appimage
```

The `appimage` target runs pre-flight verification (checks for all critical libraries, Qt plugins, Python bundle) before invoking `appimagetool`.

### CI/CD

GitHub Actions (`.github/workflows/build.yml`) automates the full pipeline on all three platforms. The workflow:

1. Caches third-party dependencies between runs
2. Builds third-party deps (on cache miss) then the application
3. Runs `cmake --install` to trigger all deployment logic
4. Packages with CPack (ZIP on Windows, DMG on macOS, AppImage on Linux)
5. Uploads artifacts and creates GitHub Releases on version tags

### CI/CD Secrets

The CI workflow uses GitHub repository secrets to enable the Google Maps map view and macOS code signing/notarization. Without secrets (e.g. forks, PRs from external contributors), the map view is disabled and macOS builds fall back to ad-hoc signing.

To configure secrets, go to your repository on GitHub: **Settings > Secrets and variables > Actions > New repository secret**, and add the secrets listed below.

#### Google Maps API Key (all platforms)

| Secret | Value |
|--------|-------|
| `GOOGLE_MAPS_API_KEY` | Your Google Maps JavaScript API key |

To obtain a key, create a project in the [Google Cloud Console](https://console.cloud.google.com/), enable the **Maps JavaScript API**, and create an API key under **APIs & Services > Credentials**. Since the key is embedded in the distributed application binary, it is recommended to restrict the key to the Maps JavaScript API only and set appropriate usage quotas and billing alerts.

#### macOS Code Signing and Notarization

To set up code signing, you need a Developer ID Application certificate installed on your Mac:

**1. Find your signing identity:**

```bash
security find-identity -v -p codesigning
```

Look for the line containing `Developer ID Application: Your Name (XXXXXXXXXX)`. The full string is your signing identity, and the 10-character code in parentheses is your team ID.

**2. Export the certificate as a .p12 file:**

Open **Keychain Access** (Applications > Utilities), select **login** in the left sidebar, then switch to the **My Certificates** tab across the top (not "Certificates" — only "My Certificates" shows certificates paired with their private keys). Find your "Developer ID Application" certificate, expand the disclosure triangle next to it, select the **private key** underneath, right-click it, and choose **Export**. Save as `.p12` format and set a strong password.

**3. Base64-encode the .p12 file:**

```bash
base64 -i Certificates.p12 | pbcopy
```

This copies the base64 string to your clipboard.

**4. Generate an app-specific password for notarization:**

Go to [appleid.apple.com](https://appleid.apple.com) > Sign-In and Security > App-Specific Passwords, and generate a new password for "FlySight Notarization" (or similar).

**5. Add these secrets to the repository:**

| Secret | Value |
|--------|-------|
| `MACOS_CERTIFICATE_P12_BASE64` | The base64 string from step 3 |
| `MACOS_CERTIFICATE_PASSWORD` | The password you set when exporting the .p12 |
| `MACOS_CODESIGN_IDENTITY` | Full identity, e.g. `Developer ID Application: Your Name (XXXXXXXXXX)` |
| `MACOS_NOTARIZATION_APPLE_ID` | Your Apple ID email address |
| `MACOS_NOTARIZATION_PASSWORD` | The app-specific password from step 4 |
| `MACOS_NOTARIZATION_TEAM_ID` | Your 10-character team ID from step 1 |

Once configured, all macOS CI builds will be distribution-signed, and tag builds (`v*`) will additionally be notarized and stapled so end users can run the app without Gatekeeper warnings.

### Package Structures

**Windows:**

```
FlySightViewer/
├── FlySightViewer.exe
├── flysight_cpp_bridge.pyd
├── Qt6Core.dll, Qt6Widgets.dll, ...
├── Geographic.dll, ...
├── qt.conf
├── plugins/
│   ├── platforms/
│   └── ...
├── qml/
│   └── ...
├── resources/
│   └── map.html
└── python/
    ├── python313.dll
    ├── python313.zip
    └── Lib/site-packages/
```

**macOS:**

```
FlySightViewer.app/
└── Contents/
    ├── MacOS/FlySightViewer
    ├── Frameworks/
    │   ├── QtCore.framework, ...
    │   ├── libGeographic.dylib, ...
    │   ├── libkddockwidgets-qt6.3.dylib
    │   └── libpython3.XX.dylib
    ├── PlugIns/
    │   ├── platforms/
    │   └── ...
    └── Resources/
        ├── qt.conf
        ├── qml/
        │   └── ...
        ├── resources/
        │   └── map.html
        ├── python/
        │   └── lib/python3.XX/site-packages/
        └── python_plugins/
```

**Linux (AppImage contents):**

```
FlySightViewer.AppDir/
├── AppRun
├── FlySightViewer.desktop
├── FlySightViewer.png
└── usr/
    ├── bin/
    │   ├── FlySightViewer
    │   └── qt.conf
    ├── lib/
    │   ├── libQt6*.so.*, libGeographic.so.*, ...
    │   └── libkddockwidgets-qt6.so.*
    ├── plugins/
    │   ├── platforms/
    │   └── ...
    ├── qml/
    │   └── ...
    ├── resources/
    │   └── map.html
    └── share/
        └── python/
            ├── bin/python3
            └── lib/python3.XX/site-packages/
```

### Deployment Tips

1. **Python Version Matching**: The bundled Python version must exactly match the version used to build the pybind11 bindings. A version mismatch will cause crashes at startup.

2. **Package Sizes**: Expect approximately 80-120 MB per platform (includes Qt, Python, and NumPy).

3. **Testing**: Always test the deployed package on a clean machine without development tools installed.

4. **macOS Signing**: The install step handles code signing automatically — ad-hoc by default, or with your Developer ID certificate if `FLYSIGHT_CODESIGN_IDENTITY` is set at configure time. No manual signing step is needed.

## Troubleshooting

### 1. Qt not found

**Symptom:** CMake error: `Could not find a package configuration file provided by "Qt6"` or a Qt6 sub-module like `Qt6QuickControls2`.

**Solution:**
- Ensure Qt 6 is installed with all required components (see [Qt Components](#qt-components))
- Set `CMAKE_PREFIX_PATH` to your Qt installation:
  ```bash
  # Windows
  cmake ... -DCMAKE_PREFIX_PATH="C:/Qt/6.7.3/msvc2019_64"

  # macOS (Homebrew Qt or Qt Online Installer)
  cmake ... -DCMAKE_PREFIX_PATH="$HOME/Qt/6.7.3/macos"

  # macOS (MacPorts) — required, see Prerequisites
  cmake ... -DCMAKE_PREFIX_PATH=/opt/local/libexec/qt6

  # Linux
  cmake ... -DCMAKE_PREFIX_PATH="$HOME/Qt/6.7.3/gcc_64"
  ```

### 2. Boost not found

**Symptom:** CMake error: `Could not find a configuration file for package "Boost"`.

**Solution:**
- Verify Boost is installed (header-only usage for boost::geometry)
- Set `BOOST_ROOT` as shown in [Prerequisites > Boost Components](#boost-components)

### 3. Python development headers not found

**Symptom:** CMake error: `Could not find a package configuration file provided by "Python"` or missing `Python.h`.

**Solution:**
- Install Python 3.10+ with development headers
- On Windows, ensure "Download debug binaries" was checked during installation for Debug builds
- Verify Python is in your PATH: `python --version`
- If multiple Python versions are installed, set `Python_ROOT_DIR`:
  ```bash
  cmake ... -DPython_ROOT_DIR="/path/to/python"
  ```

### 3a. Python bundling download fails

**Symptom:** CMake error: `Failed to download after 3 attempts` when downloading from `python-build-standalone`.

**Cause:** The build-time Python version doesn't exist in the python-build-standalone release. This commonly happens on macOS when CMake finds the system Python 3.9.6, which is too old to have a matching standalone build.

**Solution:**
- Install a modern Python (3.10+) via MacPorts or Homebrew
- Point CMake at it with `-DPython_ROOT_DIR` and `-DPython_EXECUTABLE` (see [Prerequisites > Python](#python))

### 4. KDDockWidgets Qt6 not found

**Symptom:** KDDockWidgets build fails with Qt6 not found errors.

**Solution:**
- Ensure Qt6 is properly installed and discoverable
- The KDDockWidgets build uses `-DKDDockWidgets_QT6=ON` to enable Qt6 support
- Set `CMAKE_PREFIX_PATH` if Qt is not in a standard location

### 5. Build takes too long

**Solution:**
- Use Ninja for faster builds: `-G Ninja`
- Use parallel builds: `cmake --build build --parallel`
- After initial build, use `-DFLYSIGHT_BUILD_THIRD_PARTY=OFF` to skip rebuilding dependencies

### 6. Resetting Application Preferences (macOS)

To reset all stored preferences for FlySight Viewer on macOS:

```bash
defaults delete com.flysight.Viewer
```

### 7. Resetting Application Preferences (Windows)

To reset all stored preferences for FlySight Viewer on Windows:

```cmd
reg delete "HKEY_CURRENT_USER\Software\FlySight\FlySightViewer" /f
```

To also reset built-in profiles (so they are re-copied on next launch), delete the profiles directory:

```cmd
del /q "%USERPROFILE%\Documents\FlySight Viewer\profiles\*.fvprofile"
```

### 8. Visual Studio shows many projects

**Symptom:** The Visual Studio solution contains many projects (GeographicLib, KDDockWidgets targets).

**Explanation:** This is expected behavior for a superbuild. The solution includes ExternalProject targets for third-party dependencies, the main application targets, and clean targets.

**Tip:** Set `FlySightViewer` as the startup project for development.
