# tests/support/CMakeLists.txt

# Standalone support tests (non-GTest, standalone main)
add_executable(CallbackInvocationTest CallbackInvocationTest.cpp)
target_link_libraries(CallbackInvocationTest PRIVATE asfw_test_support)
add_test(NAME CallbackInvocationTest COMMAND CallbackInvocationTest)

add_executable(simple_mock_test simple_mock_test.cpp)
target_link_libraries(simple_mock_test PRIVATE asfw_test_support)
add_test(NAME simple_mock_test COMMAND simple_mock_test)
