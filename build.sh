#!/bin/bash

# ASFireWire Build Script
# This script bumps the version and builds the complete project

set -e  # Exit on any error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Configuration
PROJECT_NAME="ASFireWire"
SCHEME_NAME="ASFireWire"  # Main app scheme that includes the system extension
BUILD_DIR="./build"
ARCHIVE_PATH="${BUILD_DIR}/${PROJECT_NAME}.xcarchive"
EXPORT_PATH="${BUILD_DIR}/export"
VERBOSE=${VERBOSE:-false}

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -v|--verbose)
            VERBOSE=true
            shift
            ;;
        -h|--help)
            echo "Usage: $0 [OPTIONS]"
            echo ""
            echo "Build script for ASFireWire project"
            echo ""
            echo "Options:"
            echo "  -v, --verbose    Show verbose build output"
            echo "  -h, --help       Show this help message"
            echo ""
            echo "The script will:"
            echo "  1. Bump version number"
            echo "  2. Build the project"
            echo "  3. Filter and display errors/warnings to stdout"
            echo "  4. Show build summary"
            exit 0
            ;;
        *)
            log_error "Unknown option: $1"
            echo "Use -h or --help for usage information"
            exit 1
            ;;
    esac
done

# Logging functions
log_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

log_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

log_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Function to check if command exists
command_exists() {
    command -v "$1" >/dev/null 2>&1
}

# Filter and display build errors and warnings from stdin
filter_build_output() {
    log_info "Analyzing build output for errors and warnings..."

    # Read all input into a variable for multiple processing
    local build_output
    build_output=$(cat)

    # Count various types of messages
    local total_lines=$(echo "$build_output" | wc -l)
    local error_count=$(echo "$build_output" | grep -c -i "error:" 2>/dev/null || echo "0")
    local warning_count=$(echo "$build_output" | grep -c -i "warning:" 2>/dev/null || echo "0")
    local ld_error_count=$(echo "$build_output" | grep -c -i "ld:.*error" 2>/dev/null || echo "0")
    local clang_error_count=$(echo "$build_output" | grep -c -i "clang.*error" 2>/dev/null || echo "0")
    local compile_error_count=$(echo "$build_output" | grep -c -i "failed with exit code" 2>/dev/null || echo "0")

    # Sanitize variables to ensure they contain only digits
    error_count=$(echo "$error_count" | tr -d -c '0-9' | sed 's/^$/0/')
    warning_count=$(echo "$warning_count" | tr -d -c '0-9' | sed 's/^$/0/')
    ld_error_count=$(echo "$ld_error_count" | tr -d -c '0-9' | sed 's/^$/0/')
    clang_error_count=$(echo "$clang_error_count" | tr -d -c '0-9' | sed 's/^$/0/')
    compile_error_count=$(echo "$compile_error_count" | tr -d -c '0-9' | sed 's/^$/0/')

    # Calculate total errors (including linker and compiler errors)
    local total_errors=$((error_count + ld_error_count + clang_error_count + compile_error_count))

    echo ""
    log_info "Build Analysis Summary:"
    echo "  - Total output lines: $total_lines"
    echo "  - Standard errors: $error_count"
    echo "  - Warnings: $warning_count"
    echo "  - Linker errors: $ld_error_count"
    echo "  - Compiler errors: $clang_error_count"
    echo "  - Build failures: $compile_error_count"
    echo ""

    # Show errors if any
    if [[ "$total_errors" -gt 0 ]]; then
        log_error "BUILD ERRORS FOUND:"
        echo "========================================"

        # Show standard errors
        if [[ "$error_count" -gt 0 ]]; then
            echo "Standard Errors:"
            echo "$build_output" | grep -n -i "error:" | head -10
            [[ "$error_count" -gt 10 ]] && echo "... and $((error_count - 10)) more standard errors"
            echo ""
        fi

        # Show linker errors
        if [[ "$ld_error_count" -gt 0 ]]; then
            echo "Linker Errors:"
            echo "$build_output" | grep -n -i "ld:.*error" | head -10
            [[ "$ld_error_count" -gt 10 ]] && echo "... and $((ld_error_count - 10)) more linker errors"
            echo ""
        fi

        # Show compiler errors
        if [[ "$clang_error_count" -gt 0 ]]; then
            echo "Compiler Errors:"
            echo "$build_output" | grep -n -i "clang.*error" | head -10
            [[ "$clang_error_count" -gt 10 ]] && echo "... and $((clang_error_count - 10)) more compiler errors"
            echo ""
        fi

        # Show build failures
        if [[ "$compile_error_count" -gt 0 ]]; then
            echo "Build Failures:"
            echo "$build_output" | grep -n -i "failed with exit code" | head -10
            [[ "$compile_error_count" -gt 10 ]] && echo "... and $((compile_error_count - 10)) more build failures"
            echo ""
        fi

        echo "========================================"
        echo ""
    fi

    # Show warnings if any
    if [[ "$warning_count" -gt 0 ]]; then
        log_warning "BUILD WARNINGS FOUND:"
        echo "========================================"
        echo "$build_output" | grep -n -i "warning:" | head -15
        if [[ "$warning_count" -gt 15 ]]; then
            echo "... and $((warning_count - 15)) more warnings"
        fi
        echo "========================================"
        echo ""
    fi

    # Determine build status from the output
    if echo "$build_output" | grep -q -i "** BUILD SUCCEEDED **\|build succeeded"; then
        log_success "✅ Build completed successfully"
    elif echo "$build_output" | grep -q -i "** BUILD FAILED **\|build failed"; then
        log_error "❌ Build failed"
    elif [[ "$total_errors" -gt 0 ]]; then
        log_error "❌ Build failed"
    else
        log_success "✅ Build completed successfully"
    fi
}

# Pre-build checks
pre_build_checks() {
    log_info "Performing pre-build checks..."

    # Check if Xcode is available
    if ! command_exists xcodebuild; then
        log_error "xcodebuild not found. Please install Xcode and Xcode Command Line Tools."
        exit 1
    fi

    # Check if we're in the right directory
    if [[ ! -f "ASFireWire.xcodeproj/project.pbxproj" ]]; then
        log_error "ASFireWire.xcodeproj not found. Please run this script from the ASFireWire directory."
        exit 1
    fi

    # Check if bump.sh exists and is executable
    if [[ ! -x "bump.sh" ]]; then
        log_error "bump.sh not found or not executable."
        exit 1
    fi

    log_success "Pre-build checks passed"
}

# Clean build directory
clean_build() {
    log_info "Cleaning previous build artifacts..."
    rm -rf "$BUILD_DIR"
    mkdir -p "$BUILD_DIR"
    log_success "Build directory cleaned"
}

# Bump version
bump_version() {
    log_info "Bumping version number..."
    if ./bump.sh; then
        log_success "Version bumped successfully"
    else
        log_error "Failed to bump version"
        exit 1
    fi
}

# Build the project
build_project() {
    log_info "Building $PROJECT_NAME..."

    # Get available schemes
    log_info "Available schemes:"
    xcodebuild -list -project "$PROJECT_NAME.xcodeproj" 2>/dev/null | grep -A 10 "Schemes:" | tail -n +2 || true

    # Build the main scheme (includes system extension)
    log_info "Building scheme: $SCHEME_NAME"

    # Set xcodebuild verbosity based on flag
    local xcodebuild_opts=""
    if [[ "$VERBOSE" == "true" ]]; then
        xcodebuild_opts="-verbose"
        log_info "Verbose mode enabled"
    fi

    # Run xcodebuild and filter output in real-time
    local build_output
    local build_exit_code
    
    # Capture the build output
    build_output=$(xcodebuild \
        -project "$PROJECT_NAME.xcodeproj" \
        -scheme "$SCHEME_NAME" \
        -configuration Release \
        -derivedDataPath "$BUILD_DIR/DerivedData" \
        $xcodebuild_opts \
        build 2>&1)
    
    build_exit_code=$?

    # Filter and display errors and warnings from the captured output
    echo "$build_output" | filter_build_output

    if [[ $build_exit_code -eq 0 ]]; then
        log_info "Build products created in: $BUILD_DIR/DerivedData"
        return 0
    else
        log_error "Build failed with exit code $build_exit_code"
        return 1
    fi
}

# Export the archive (optional)
export_archive() {
    log_info "Exporting archive for distribution..."

    # Create export options plist
    cat > "$BUILD_DIR/export_options.plist" << EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>method</key>
    <string>developer-id</string>
    <key>teamID</key>
    <string>\$(DEVELOPMENT_TEAM)</string>
    <key>signingStyle</key>
    <string>automatic</string>
    <key>destination</key>
    <string>export</string>
</dict>
</plist>
EOF

    if xcodebuild \
        -exportArchive \
        -archivePath "$ARCHIVE_PATH.xcarchive" \
        -exportPath "$EXPORT_PATH" \
        -exportOptionsPlist "$BUILD_DIR/export_options.plist"; then

        log_success "Archive exported successfully"
        log_info "Exported app at: $EXPORT_PATH"
    else
        log_warning "Archive export failed (this may be expected if not signed for distribution)"
    fi
}

# Show build summary
show_summary() {
    local build_status="$1"
    
    if [[ "$build_status" == "success" ]]; then
        log_success "Build process completed!"
        echo ""
        log_info "Build Summary:"
        echo "  - Version bumped ✅"
        echo "  - Project built successfully ✅"
        if [[ -d "$EXPORT_PATH" ]]; then
            echo "  - Archive exported for distribution ✅"
        fi
        echo ""
        log_info "Output locations:"
        echo "  - Build products: $BUILD_DIR/DerivedData/Build/Products"
        echo "  - Derived data: $BUILD_DIR/DerivedData"
        if [[ -d "$EXPORT_PATH" ]]; then
            echo "  - Exported app: $EXPORT_PATH"
        fi
        echo ""
        log_info "Next steps:"
        echo "  1. Test the built app: open $BUILD_DIR/DerivedData/Build/Products/Release/"
        echo "  2. For distribution: create archive with 'xcodebuild archive' command"
        echo "  3. Install system extension via System Settings if needed"
    else
        log_error "Build process failed!"
        echo ""
        log_info "Build Summary:"
        echo "  - Version bumped ✅"
        echo "  - Project build failed ❌"
        echo ""
        log_info "To investigate the failure:"
        echo "  - Re-run with --verbose flag to see full build output"
        echo "  - Check for missing header files or dependencies"
    fi
}

# Main build process
main() {
    echo "========================================"
    echo "  ASFireWire Build Script"
    echo "========================================"
    echo ""

    pre_build_checks
    clean_build
    bump_version
    
    # Build the project and capture result
    if build_project; then
        build_status="success"
    else
        build_status="failure"
    fi

    # Optional: export for distribution (comment out if not needed)
    # export_archive

    show_summary "$build_status"
}

# Run main function
main "$@"