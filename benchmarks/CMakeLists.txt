add_executable(order_books_benchmark
  order_book_benchmark.cpp
  pipeline_ceiling_benchmark.cpp
)

set_target_properties(order_books_benchmark PROPERTIES
  CXX_EXTENSIONS OFF
)

target_include_directories(order_books_benchmark
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(order_books_benchmark
  PRIVATE
    order_books::runtime
    order_books::project_options
)

if(BUILD_TESTING)
  add_test(
    NAME order_books_benchmark_engine_producer_lanes_zero
    COMMAND ${CMAKE_COMMAND}
            "-DPROGRAM=$<TARGET_FILE:order_books_benchmark>"
            -DCLI_ARGUMENT=--engine-producer-lanes=0
            -DEXPECTED_ERROR=engine_producer_lanes_must_be_positive
            -P "${CMAKE_CURRENT_SOURCE_DIR}/check_invalid_cli.cmake"
  )

  add_test(
    NAME order_books_benchmark_engine_producer_lanes_non_numeric
    COMMAND ${CMAKE_COMMAND}
            "-DPROGRAM=$<TARGET_FILE:order_books_benchmark>"
            -DCLI_ARGUMENT=--engine-producer-lanes=abc
            -DEXPECTED_ERROR=engine_producer_lanes_invalid
            -P "${CMAKE_CURRENT_SOURCE_DIR}/check_invalid_cli.cmake"
  )

  add_test(
    NAME order_books_benchmark_engine_producer_lanes_over_capacity
    COMMAND ${CMAKE_COMMAND}
            "-DPROGRAM=$<TARGET_FILE:order_books_benchmark>"
            -DCLI_ARGUMENT=--engine-producer-lanes=65537
            -DEXPECTED_ERROR=engine_producer_lanes_exceed_capacity
            -P "${CMAKE_CURRENT_SOURCE_DIR}/check_invalid_cli.cmake"
  )

  add_test(
    NAME order_books_benchmark_wal_phase_profile_invalid
    COMMAND ${CMAKE_COMMAND}
            "-DPROGRAM=$<TARGET_FILE:order_books_benchmark>"
            -DCLI_ARGUMENT=--workload=wal_write_ceiling
            -DCLI_ARGUMENT_2=--wal-phase-profile=invalid
            -DEXPECTED_ERROR=wal_phase_profile_invalid
            -P "${CMAKE_CURRENT_SOURCE_DIR}/check_invalid_cli.cmake"
  )

  add_test(
    NAME order_books_benchmark_wal_phase_profile_empty
    COMMAND ${CMAKE_COMMAND}
            "-DPROGRAM=$<TARGET_FILE:order_books_benchmark>"
            -DCLI_ARGUMENT=--workload=wal_write_ceiling
            -DCLI_ARGUMENT_2=--wal-phase-profile=
            -DEXPECTED_ERROR=wal_phase_profile_invalid
            -P "${CMAKE_CURRENT_SOURCE_DIR}/check_invalid_cli.cmake"
  )

  add_test(
    NAME order_books_benchmark_wal_phase_profile_requires_wal
    COMMAND ${CMAKE_COMMAND}
            "-DPROGRAM=$<TARGET_FILE:order_books_benchmark>"
            -DCLI_ARGUMENT=--wal-phase-profile=on
            -DEXPECTED_ERROR=wal_phase_profile_requires_wal_workload
            -P "${CMAKE_CURRENT_SOURCE_DIR}/check_invalid_cli.cmake"
  )

  add_test(
    NAME order_books_benchmark_wal_phase_profile_smoke
    COMMAND $<TARGET_FILE:order_books_benchmark>
            --workload=wal_write_ceiling
            --wal-phase-profile=on
            --wal-sync=none
            --wal-group-size=2
            --iterations=2
            --warmup=1
  )
  set_tests_properties(
    order_books_benchmark_wal_phase_profile_smoke
    PROPERTIES
      PASS_REGULAR_EXPRESSION
      "sync_samples=0.*sync_p50_us=na.*replay_verified=true.*phase_profile=on.*profiled_groups=2.*profiled_commands=4.*sync_total_us=0.*group_total_us=.*fixture_build_group_total_us=.*wal_append_call_group_total_us=.*wal_lock_wait_group_total_us=.*wal_prepare_group_total_us=.*wal_plan_copy_group_total_us=.*wal_rotation_group_total_us=0.*wal_write_group_total_us=.*wal_publish_group_total_us=.*sync_share_percent=na"
  )

  add_test(
    NAME order_books_benchmark_wal_phase_profile_off_smoke
    COMMAND $<TARGET_FILE:order_books_benchmark>
            --workload=wal_write_ceiling
            --wal-phase-profile=off
            --wal-sync=none
            --wal-group-size=2
            --iterations=2
            --warmup=1
  )
  set_tests_properties(
    order_books_benchmark_wal_phase_profile_off_smoke
    PROPERTIES
      PASS_REGULAR_EXPRESSION "replay_verified=true.*phase_profile=off"
      FAIL_REGULAR_EXPRESSION "profiled_groups=;sync_total_us=;group_total_us=;fixture_build_group_total_us=;wal_append_call_group_total_us=;wal_lock_wait_group_total_us=;wal_prepare_group_total_us=;wal_plan_copy_group_total_us=;wal_rotation_group_total_us=;wal_write_group_total_us=;wal_publish_group_total_us=;lock_wait_share_percent="
  )

  add_test(
    NAME order_books_benchmark_wal_phase_profile_per_group_smoke
    COMMAND $<TARGET_FILE:order_books_benchmark>
            --workload=wal_write_ceiling
            --wal-phase-profile=on
            --wal-sync=per_group
            --wal-group-size=2
            --iterations=2
            --warmup=1
  )
  set_tests_properties(
    order_books_benchmark_wal_phase_profile_per_group_smoke
    PROPERTIES
      PASS_REGULAR_EXPRESSION "sync_samples=2.*replay_verified=true.*phase_profile=on.*sync_total_us="
  )
endif()
