cmake_minimum_required(VERSION 3.28)

project(RomExplorer LANGUAGES Swift OBJC)

enable_testing()

set(CMAKE_Swift_LANGUAGE_VERSION 6)

set(SWIFT_MODULE_CACHE_DIR "${CMAKE_BINARY_DIR}/swift-module-cache")
file(MAKE_DIRECTORY "${SWIFT_MODULE_CACHE_DIR}")
set(SWIFT_MODULE_CACHE_FLAGS
  "-module-cache-path"
  "${SWIFT_MODULE_CACHE_DIR}"
  "-Xcc"
  "-fmodules-cache-path=${SWIFT_MODULE_CACHE_DIR}"
)

# App target
file(GLOB_RECURSE APP_SOURCES
  "Sources/App/*.swift"
  "Sources/Views/*.swift"
  "Sources/ViewModels/*.swift"
  "Sources/Models/*.swift"
)

# Explicitly include newly added model files to avoid glob caching issues
list(APPEND APP_SOURCES
  ${CMAKE_SOURCE_DIR}/Sources/Models/RomCache.swift
  ${CMAKE_SOURCE_DIR}/Sources/Models/RomInterpreter.swift
  ${CMAKE_SOURCE_DIR}/Sources/Models/RomSummarizer.swift
  ${CMAKE_SOURCE_DIR}/Sources/Support/DebugLog.swift
)

add_executable(RomExplorer MACOSX_BUNDLE ${APP_SOURCES})
target_compile_options(RomExplorer PRIVATE ${SWIFT_MODULE_CACHE_FLAGS})

set_target_properties(RomExplorer PROPERTIES
  MACOSX_BUNDLE TRUE
  XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.example.RomExplorer"
  XCODE_ATTRIBUTE_SWIFT_VERSION "6"
  XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "NO"
)

target_link_libraries(RomExplorer PRIVATE
  "-framework SwiftUI"
  "-framework AppKit"
  "-framework Foundation"
)

set_source_files_properties(${APP_SOURCES} PROPERTIES LANGUAGE Swift)

# Tests: build only under Xcode generator where XCTest is available by default
if(CMAKE_GENERATOR STREQUAL "Xcode")
  file(GLOB_RECURSE TEST_SOURCES
    "Tests/*.swift"
  )
  add_executable(RomExplorerTestsRunner ${TEST_SOURCES})
  target_compile_options(RomExplorerTestsRunner PRIVATE ${SWIFT_MODULE_CACHE_FLAGS})
  set_target_properties(RomExplorerTestsRunner PROPERTIES
    XCODE_ATTRIBUTE_SWIFT_VERSION "6"
  )
  target_link_libraries(RomExplorerTestsRunner PRIVATE
    "-framework XCTest"
    "-framework Foundation"
  )
  set_source_files_properties(${TEST_SOURCES} PROPERTIES LANGUAGE Swift)
  add_test(NAME RomExplorerTests COMMAND RomExplorerTestsRunner)
endif()

# Ninja/plain Swift test runner
if(CMAKE_GENERATOR STREQUAL "Ninja")
  file(GLOB_RECURSE PLAIN_TEST_SOURCES
    "Tests/RomParserUnitTests.swift"
  )
  add_executable(RomParserUnitTests ${PLAIN_TEST_SOURCES}
    Sources/Models/RomModels.swift
    Sources/Models/RomParser.swift
    Sources/Models/RomCache.swift
    Sources/Support/DebugLog.swift)
  target_compile_options(RomParserUnitTests PRIVATE ${SWIFT_MODULE_CACHE_FLAGS})
  set_source_files_properties(${PLAIN_TEST_SOURCES} PROPERTIES LANGUAGE Swift)
  set_source_files_properties(
    Sources/Models/RomModels.swift
    Sources/Models/RomParser.swift
  Sources/Models/RomCache.swift
  Sources/Support/DebugLog.swift
    PROPERTIES LANGUAGE Swift)
  add_test(NAME RomParserUnitTests COMMAND RomParserUnitTests)
endif()
