#!/bin/bash
# Build script for asfw-version CLI tool

set -e

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
OUTPUT_DIR="$SCRIPT_DIR/../../build/tools"

mkdir -p "$OUTPUT_DIR"

echo "Building asfw-version CLI tool..."
swiftc -o "$OUTPUT_DIR/asfw-version" \
    -framework Foundation \
    -framework IOKit \
    "$SCRIPT_DIR/main.swift"

echo "✅ Built: $OUTPUT_DIR/asfw-version"
echo ""
echo "Usage:"
echo "  $OUTPUT_DIR/asfw-version"
echo ""
echo "Or install to /usr/local/bin:"
echo "  sudo cp $OUTPUT_DIR/asfw-version /usr/local/bin/"
