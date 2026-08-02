# ProjFS is Windows-only; nothing to build here elsewhere.
if(NOT WIN32)
  return()
endif()

add_executable(projfs_experiment projfs.m.cpp)
target_link_libraries(projfs_experiment PRIVATE ProjectedFSLib Ole32)
target_compile_definitions(projfs_experiment PRIVATE UNICODE _UNICODE)
makebelieve_apply_clang_tidy(projfs_experiment)

add_executable(filewatch filewatch.m.cpp)
target_compile_definitions(filewatch PRIVATE UNICODE _UNICODE)
makebelieve_apply_clang_tidy(filewatch)

# Black-box test: mounts the real projfs_experiment binary and drives it
# through smoke_test.ps1.
add_test(NAME projfs_experiment_smoke
  COMMAND powershell -NoProfile -ExecutionPolicy Bypass
    -File "${CMAKE_CURRENT_SOURCE_DIR}/smoke_test.ps1"
    -ProjfsExe "$<TARGET_FILE:projfs_experiment>"
    -FilewatchExe "$<TARGET_FILE:filewatch>"
)
set_tests_properties(projfs_experiment_smoke PROPERTIES TIMEOUT 30)
