include(FetchContent)
FetchContent_Declare(Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.7.1)
FetchContent_MakeAvailable(Catch2)

add_executable(pgain_tests wire_test.cpp dedup_test.cpp auth_test.cpp udp_test.cpp multipath_test.cpp tunnel_test.cpp)
target_link_libraries(pgain_tests PRIVATE pgain_core Catch2::Catch2WithMain)

include(Catch)
catch_discover_tests(pgain_tests)
