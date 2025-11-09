set -e

cd /Users/mrmidi/DEV/FirWireDriver/RomExplorer/build-ninja
/opt/homebrew/bin/cmake --regenerate-during-build -S$(CMAKE_SOURCE_DIR) -B$(CMAKE_BINARY_DIR)
