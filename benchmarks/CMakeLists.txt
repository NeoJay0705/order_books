add_executable(order_books_benchmark
  order_book_benchmark.cpp
  engine_tail_telemetry.cpp
  engine_writer_profile_benchmark.cpp
  pipeline_ceiling_benchmark.cpp
)

if(BUILD_TESTING)
  add_executable(order_books_wal_latency_sampler_test
    wal_latency_sampler_test.cpp
  )
  set_target_properties(order_books_wal_latency_sampler_test PROPERTIES
    CXX_EXTENSIONS OFF
  )
  target_link_libraries(order_books_wal_latency_sampler_test
    PRIVATE
      order_books::project_options
  )
  add_test(
    NAME order_books_benchmark_wal_latency_sampler
    COMMAND $<TARGET_FILE:order_books_wal_latency_sampler_test>
  )
endif()

find_program(ENGINE_WRITER_DIAGNOSTICS_BASH bash)

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
  foreach(scenario IN ITEMS new_crossing_pair new_resting_cancel amend_quantity replace_order)
    if(scenario STREQUAL "new_crossing_pair")
      set(expected_scenario_results "trades=2.*events=8")
      set(expected_active_orders "active_orders=0")
    else()
      set(expected_scenario_results "trades=0.*events=4")
      if(scenario STREQUAL "new_resting_cancel")
        set(expected_active_orders "active_orders=0")
      else()
        set(expected_active_orders "active_orders=1")
      endif()
    endif()
    add_test(
      NAME "order_books_benchmark_pipeline_state_machine_${scenario}"
      COMMAND $<TARGET_FILE:order_books_benchmark>
              --workload=engine_pipeline_ceiling
              --pipeline-stage=state_machine
              --pipeline-command-scenario=${scenario}
              --pipeline-batch-size=2
              --pipeline-active-orders=0
              --iterations=2
              --warmup=1
    )
    set_tests_properties(
      "order_books_benchmark_pipeline_state_machine_${scenario}"
      PROPERTIES
        PASS_REGULAR_EXPRESSION
        "scenario=${scenario}.*measured_commands=4.*${expected_scenario_results}.*${expected_active_orders}.*correctness_verified=true"
    )
  endforeach()

  add_test(
    NAME order_books_benchmark_pipeline_command_scenario_invalid
    COMMAND ${CMAKE_COMMAND}
            "-DPROGRAM=$<TARGET_FILE:order_books_benchmark>"
            -DCLI_ARGUMENT=--workload=engine_pipeline_ceiling
            -DCLI_ARGUMENT_2=--pipeline-command-scenario=invalid
            -DEXPECTED_ERROR=pipeline_command_scenario_invalid
            -P "${CMAKE_CURRENT_SOURCE_DIR}/check_invalid_cli.cmake"
  )

  add_test(
    NAME order_books_benchmark_pipeline_state_machine_odd_batch
    COMMAND ${CMAKE_COMMAND}
            "-DPROGRAM=$<TARGET_FILE:order_books_benchmark>"
            -DWORKLOAD=engine_pipeline_ceiling
            -DCLI_ARGUMENT=--pipeline-stage=state_machine
            -DCLI_ARGUMENT_2=--pipeline-batch-size=3
            -DEXPECTED_ERROR=batch_size_must_be_even
            -P "${CMAKE_CURRENT_SOURCE_DIR}/check_invalid_cli.cmake"
  )

  if(ENGINE_WRITER_DIAGNOSTICS_BASH)
    add_test(
      NAME order_books_engine_writer_diagnostics_runner_dry_run
      COMMAND "${ENGINE_WRITER_DIAGNOSTICS_BASH}"
              "${CMAKE_CURRENT_SOURCE_DIR}/test_engine_writer_diagnostics_runner.sh"
    )
    set_tests_properties(
      order_books_engine_writer_diagnostics_runner_dry_run
      PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/.."
    )
  endif()

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
    NAME order_books_benchmark_wal_no_rotation_epoch_invalid
    COMMAND ${CMAKE_COMMAND}
            "-DPROGRAM=$<TARGET_FILE:order_books_benchmark>"
            -DCLI_ARGUMENT=--workload=wal_write_ceiling
            -DCLI_ARGUMENT_2=--wal-no-rotation-epoch-commands=invalid
            -DEXPECTED_ERROR=wal_no_rotation_epoch_commands_invalid
            -P "${CMAKE_CURRENT_SOURCE_DIR}/check_invalid_cli.cmake"
  )

  add_test(
    NAME order_books_benchmark_wal_no_rotation_epoch_requires_wal_workload
    COMMAND ${CMAKE_COMMAND}
            "-DPROGRAM=$<TARGET_FILE:order_books_benchmark>"
            -DCLI_ARGUMENT=--wal-no-rotation-epoch-commands=2
            -DEXPECTED_ERROR=wal_no_rotation_epoch_requires_wal_workload
            -P "${CMAKE_CURRENT_SOURCE_DIR}/check_invalid_cli.cmake"
  )

  add_test(
    NAME order_books_benchmark_wal_no_rotation_epoch_requires_sync_none
    COMMAND ${CMAKE_COMMAND}
            "-DPROGRAM=$<TARGET_FILE:order_books_benchmark>"
            -DCLI_ARGUMENT=--workload=wal_write_ceiling
            -DCLI_ARGUMENT_2=--wal-no-rotation-epoch-commands=2
            -DCLI_ARGUMENT_3=--wal-sync=per_group
            -DEXPECTED_ERROR=wal_no_rotation_epoch_requires_sync_none
            -P "${CMAKE_CURRENT_SOURCE_DIR}/check_invalid_cli.cmake"
  )

  add_test(
    NAME order_books_benchmark_wal_rotation_diagnostic_invalid
    COMMAND ${CMAKE_COMMAND}
            "-DPROGRAM=$<TARGET_FILE:order_books_benchmark>"
            -DCLI_ARGUMENT=--workload=wal_write_ceiling
            -DCLI_ARGUMENT_2=--wal-rotation-diagnostic=invalid
            -DEXPECTED_ERROR=wal_rotation_diagnostic_invalid
            -P "${CMAKE_CURRENT_SOURCE_DIR}/check_invalid_cli.cmake"
  )

  add_test(
    NAME order_books_benchmark_wal_rotation_diagnostic_requires_wal_workload
    COMMAND ${CMAKE_COMMAND}
            "-DPROGRAM=$<TARGET_FILE:order_books_benchmark>"
            -DCLI_ARGUMENT=--wal-rotation-diagnostic=control
            -DEXPECTED_ERROR=wal_rotation_diagnostic_requires_wal_workload
            -P "${CMAKE_CURRENT_SOURCE_DIR}/check_invalid_cli.cmake"
  )

  add_test(
    NAME order_books_benchmark_wal_rotation_diagnostic_requires_profiled_single_group
    COMMAND ${CMAKE_COMMAND}
            "-DPROGRAM=$<TARGET_FILE:order_books_benchmark>"
            -DCLI_ARGUMENT=--workload=wal_write_ceiling
            -DCLI_ARGUMENT_2=--wal-rotation-diagnostic=control
            -DCLI_ARGUMENT_3=--wal-sync=none
            -DCLI_ARGUMENT_4=--wal-phase-profile=off
            -DEXPECTED_ERROR=wal_rotation_diagnostic_requires_b1_profiled_single_group
            -P "${CMAKE_CURRENT_SOURCE_DIR}/check_invalid_cli.cmake"
  )

  add_test(
    NAME order_books_benchmark_wal_rotation_diagnostic_requires_sync_none
    COMMAND ${CMAKE_COMMAND}
            "-DPROGRAM=$<TARGET_FILE:order_books_benchmark>"
            -DCLI_ARGUMENT=--workload=wal_write_ceiling
            -DCLI_ARGUMENT_2=--wal-rotation-diagnostic=control
            -DCLI_ARGUMENT_3=--wal-group-size=1
            -DCLI_ARGUMENT_4=--iterations=1
            -DCLI_ARGUMENT_5=--wal-sync=per_group
            -DCLI_ARGUMENT_6=--wal-phase-profile=on
            -DEXPECTED_ERROR=wal_rotation_diagnostic_requires_b1_profiled_single_group
            -P "${CMAKE_CURRENT_SOURCE_DIR}/check_invalid_cli.cmake"
  )

  add_test(
    NAME order_books_benchmark_wal_rotation_diagnostic_requires_profile_on
    COMMAND ${CMAKE_COMMAND}
            "-DPROGRAM=$<TARGET_FILE:order_books_benchmark>"
            -DCLI_ARGUMENT=--workload=wal_write_ceiling
            -DCLI_ARGUMENT_2=--wal-rotation-diagnostic=control
            -DCLI_ARGUMENT_3=--wal-group-size=1
            -DCLI_ARGUMENT_4=--iterations=1
            -DCLI_ARGUMENT_5=--wal-sync=none
            -DCLI_ARGUMENT_6=--wal-phase-profile=off
            -DEXPECTED_ERROR=wal_rotation_diagnostic_requires_b1_profiled_single_group
            -P "${CMAKE_CURRENT_SOURCE_DIR}/check_invalid_cli.cmake"
  )

  add_test(
    NAME order_books_benchmark_wal_rotation_diagnostic_requires_group_one
    COMMAND ${CMAKE_COMMAND}
            "-DPROGRAM=$<TARGET_FILE:order_books_benchmark>"
            -DCLI_ARGUMENT=--workload=wal_write_ceiling
            -DCLI_ARGUMENT_2=--wal-rotation-diagnostic=control
            -DCLI_ARGUMENT_3=--wal-group-size=2
            -DCLI_ARGUMENT_4=--iterations=1
            -DCLI_ARGUMENT_5=--wal-sync=none
            -DCLI_ARGUMENT_6=--wal-phase-profile=on
            -DEXPECTED_ERROR=wal_rotation_diagnostic_requires_b1_profiled_single_group
            -P "${CMAKE_CURRENT_SOURCE_DIR}/check_invalid_cli.cmake"
  )

  add_test(
    NAME order_books_benchmark_wal_rotation_diagnostic_requires_one_iteration
    COMMAND ${CMAKE_COMMAND}
            "-DPROGRAM=$<TARGET_FILE:order_books_benchmark>"
            -DCLI_ARGUMENT=--workload=wal_write_ceiling
            -DCLI_ARGUMENT_2=--wal-rotation-diagnostic=control
            -DCLI_ARGUMENT_3=--wal-group-size=1
            -DCLI_ARGUMENT_4=--iterations=2
            -DCLI_ARGUMENT_5=--wal-sync=none
            -DCLI_ARGUMENT_6=--wal-phase-profile=on
            -DEXPECTED_ERROR=wal_rotation_diagnostic_requires_b1_profiled_single_group
            -P "${CMAKE_CURRENT_SOURCE_DIR}/check_invalid_cli.cmake"
  )

  add_test(
    NAME order_books_benchmark_wal_rotation_diagnostic_conflicts_no_rotation_epoch
    COMMAND ${CMAKE_COMMAND}
            "-DPROGRAM=$<TARGET_FILE:order_books_benchmark>"
            -DCLI_ARGUMENT=--workload=wal_write_ceiling
            -DCLI_ARGUMENT_2=--wal-rotation-diagnostic=control
            -DCLI_ARGUMENT_3=--wal-group-size=1
            -DCLI_ARGUMENT_4=--iterations=1
            -DCLI_ARGUMENT_5=--wal-sync=none
            -DCLI_ARGUMENT_6=--wal-phase-profile=on
            -DCLI_ARGUMENT_7=--wal-no-rotation-epoch-commands=2
            -DEXPECTED_ERROR=wal_rotation_diagnostic_requires_b1_profiled_single_group
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
      "service_target_attainment_percent=.*sync_samples=0.*replay_verified=true.*phase_profile=on.*profiled_groups=2.*profiled_commands=4.*sync_total_us=0.*group_total_us=.*fixture_build_group_total_us=.*wal_append_call_group_total_us=.*wal_lock_wait_group_total_us=.*wal_prepare_group_total_us=.*wal_plan_copy_group_total_us=.*wal_rotation_group_total_us=0.*wal_write_group_total_us=.*wal_publish_group_total_us=.*sync_share_percent=na"
  )

  add_test(
    NAME order_books_benchmark_wal_no_rotation_epoch_smoke
    COMMAND ${CMAKE_COMMAND}
            "-DPROGRAM=$<TARGET_FILE:order_books_benchmark>"
            -DCLI_ARGUMENT_1=--workload=wal_write_ceiling
            -DCLI_ARGUMENT_2=--wal-sync=none
            -DCLI_ARGUMENT_3=--wal-no-rotation-epoch-commands=2
            -DCLI_ARGUMENT_4=--wal-group-size=2
            -DCLI_ARGUMENT_5=--iterations=2
            -DCLI_ARGUMENT_6=--warmup=1
            -P "${CMAKE_CURRENT_SOURCE_DIR}/check_wal_no_rotation_output.cmake"
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
      PASS_REGULAR_EXPRESSION "service_target_attainment_percent=.*replay_verified=true.*phase_profile=off"
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
      PASS_REGULAR_EXPRESSION "service_target_attainment_percent=.*sync_samples=2.*replay_verified=true.*phase_profile=on.*sync_total_us="
  )
  add_test(
    NAME order_books_benchmark_writer_phase_profile_invalid
    COMMAND ${CMAKE_COMMAND}
            "-DPROGRAM=$<TARGET_FILE:order_books_benchmark>"
            -DCLI_ARGUMENT=--workload=engine_writer_hot_path_profile
            -DCLI_ARGUMENT_2=--writer-phase-profile=invalid
            -DEXPECTED_ERROR=writer_phase_profile_invalid
            -P "${CMAKE_CURRENT_SOURCE_DIR}/check_invalid_cli.cmake"
  )

  add_test(
    NAME order_books_benchmark_writer_phase_profile_empty
    COMMAND ${CMAKE_COMMAND}
            "-DPROGRAM=$<TARGET_FILE:order_books_benchmark>"
            -DCLI_ARGUMENT=--workload=engine_writer_hot_path_profile
            -DCLI_ARGUMENT_2=--writer-phase-profile=
            -DEXPECTED_ERROR=writer_phase_profile_invalid
            -P "${CMAKE_CURRENT_SOURCE_DIR}/check_invalid_cli.cmake"
  )

  add_test(
    NAME order_books_benchmark_writer_group_size_over_capacity
    COMMAND ${CMAKE_COMMAND}
            "-DPROGRAM=$<TARGET_FILE:order_books_benchmark>"
            -DCLI_ARGUMENT=--workload=engine_writer_hot_path_profile
            -DCLI_ARGUMENT_2=--engine-group-size=65537
            -DEXPECTED_ERROR=engine_group_size_exceed_capacity
            -P "${CMAKE_CURRENT_SOURCE_DIR}/check_invalid_cli.cmake"
  )

  add_test(
    NAME order_books_benchmark_writer_phase_profile_requires_writer_workload
    COMMAND ${CMAKE_COMMAND}
            "-DPROGRAM=$<TARGET_FILE:order_books_benchmark>"
            -DCLI_ARGUMENT=--writer-phase-profile=on
            -DEXPECTED_ERROR=writer_phase_profile_requires_writer_profile_workload
            -P "${CMAKE_CURRENT_SOURCE_DIR}/check_invalid_cli.cmake"
  )

  add_test(
    NAME order_books_benchmark_writer_apply_subprofile_invalid
    COMMAND ${CMAKE_COMMAND}
            "-DPROGRAM=$<TARGET_FILE:order_books_benchmark>"
            -DCLI_ARGUMENT=--workload=engine_writer_hot_path_profile
            -DCLI_ARGUMENT_2=--writer-apply-subprofile=invalid
            -DEXPECTED_ERROR=writer_apply_subprofile_invalid
            -P "${CMAKE_CURRENT_SOURCE_DIR}/check_invalid_cli.cmake"
  )

  add_test(
    NAME order_books_benchmark_writer_apply_subprofile_empty
    COMMAND ${CMAKE_COMMAND}
            "-DPROGRAM=$<TARGET_FILE:order_books_benchmark>"
            -DCLI_ARGUMENT=--workload=engine_writer_hot_path_profile
            -DCLI_ARGUMENT_2=--writer-apply-subprofile=
            -DEXPECTED_ERROR=writer_apply_subprofile_invalid
            -P "${CMAKE_CURRENT_SOURCE_DIR}/check_invalid_cli.cmake"
  )

  add_test(
    NAME order_books_benchmark_writer_apply_subprofile_requires_profile
    COMMAND ${CMAKE_COMMAND}
            "-DPROGRAM=$<TARGET_FILE:order_books_benchmark>"
            -DCLI_ARGUMENT=--workload=engine_writer_hot_path_profile
            -DCLI_ARGUMENT_2=--writer-apply-subprofile=on
            -DEXPECTED_ERROR=writer_apply_subprofile_requires_profile_on
            -P "${CMAKE_CURRENT_SOURCE_DIR}/check_invalid_cli.cmake"
  )

  add_test(
    NAME order_books_benchmark_writer_apply_subprofile_requires_writer_workload
    COMMAND ${CMAKE_COMMAND}
            "-DPROGRAM=$<TARGET_FILE:order_books_benchmark>"
            -DCLI_ARGUMENT=--writer-apply-subprofile=on
            -DEXPECTED_ERROR=writer_apply_subprofile_requires_writer_profile_workload
            -P "${CMAKE_CURRENT_SOURCE_DIR}/check_invalid_cli.cmake"
  )

  add_test(
    NAME order_books_benchmark_wal_prepare_workers_invalid
    COMMAND ${CMAKE_COMMAND}
            "-DPROGRAM=$<TARGET_FILE:order_books_benchmark>"
            -DCLI_ARGUMENT=--workload=wal_write_ceiling
            -DCLI_ARGUMENT_2=--wal-prepare-workers=abc
            -DEXPECTED_ERROR=wal_prepare_workers_invalid
            -P "${CMAKE_CURRENT_SOURCE_DIR}/check_invalid_cli.cmake"
  )

  add_test(
    NAME order_books_benchmark_wal_prepare_workers_unsupported
    COMMAND ${CMAKE_COMMAND}
            "-DPROGRAM=$<TARGET_FILE:order_books_benchmark>"
            -DCLI_ARGUMENT=--workload=wal_write_ceiling
            -DCLI_ARGUMENT_2=--wal-prepare-workers=3
            -DEXPECTED_ERROR=wal_prepare_workers_must_be_1_2_or_4
            -P "${CMAKE_CURRENT_SOURCE_DIR}/check_invalid_cli.cmake"
  )

  add_test(
    NAME order_books_benchmark_wal_prepare_min_commands_zero
    COMMAND ${CMAKE_COMMAND}
            "-DPROGRAM=$<TARGET_FILE:order_books_benchmark>"
            -DCLI_ARGUMENT=--workload=wal_write_ceiling
            -DCLI_ARGUMENT_2=--wal-parallel-prepare-min-commands=0
            -DEXPECTED_ERROR=wal_parallel_prepare_min_commands_must_be_positive
            -P "${CMAKE_CURRENT_SOURCE_DIR}/check_invalid_cli.cmake"
  )

  add_test(
    NAME order_books_benchmark_wal_prepare_options_engine_smoke
    COMMAND $<TARGET_FILE:order_books_benchmark>
            --workload=engine_durable_single_instrument
            --wal-prepare-workers=2
            --wal-parallel-prepare-min-commands=2
            --engine-group-size=2
            --engine-group-delay-us=0
            --engine-producer-lanes=2
            --iterations=2
            --warmup=1
  )
  set_tests_properties(
    order_books_benchmark_wal_prepare_options_engine_smoke
    PROPERTIES
      PASS_REGULAR_EXPRESSION
      "engine_durable_single_instrument.*wal_prepare_workers=2.*actual_parallel_prepare_groups=[1-9][0-9]*.*wal_sync_full_run_p99_us=.*"
  )

  add_test(
    NAME order_books_benchmark_engine_tail_telemetry_output_empty
    COMMAND ${CMAKE_COMMAND}
            "-DPROGRAM=$<TARGET_FILE:order_books_benchmark>"
            -DCLI_ARGUMENT=--engine-tail-telemetry-output=
            -DEXPECTED_ERROR=engine_tail_telemetry_output_invalid
            -P "${CMAKE_CURRENT_SOURCE_DIR}/check_invalid_cli.cmake"
  )

  add_test(
    NAME order_books_benchmark_engine_tail_state_sampling_invalid
    COMMAND ${CMAKE_COMMAND}
            "-DPROGRAM=$<TARGET_FILE:order_books_benchmark>"
            -DCLI_ARGUMENT=--engine-tail-state-sampling=invalid
            -DEXPECTED_ERROR=engine_tail_state_sampling_invalid
            -P "${CMAKE_CURRENT_SOURCE_DIR}/check_invalid_cli.cmake"
  )

  add_test(
    NAME order_books_benchmark_engine_tail_state_sampling_requires_telemetry
    COMMAND ${CMAKE_COMMAND}
            "-DPROGRAM=$<TARGET_FILE:order_books_benchmark>"
            -DCLI_ARGUMENT=--engine-tail-state-sampling=off
            -DEXPECTED_ERROR=engine_tail_state_sampling_requires_telemetry
            -P "${CMAKE_CURRENT_SOURCE_DIR}/check_invalid_cli.cmake"
  )

  add_test(
    NAME order_books_benchmark_engine_tail_telemetry_wrong_workload
    COMMAND ${CMAKE_COMMAND}
            "-DPROGRAM=$<TARGET_FILE:order_books_benchmark>"
            -DWORKLOAD=wal_write_ceiling
            "-DCLI_ARGUMENT=--engine-tail-telemetry-output=${CMAKE_CURRENT_BINARY_DIR}/order_books-invalid-tail.csv"
            -DEXPECTED_ERROR=engine_tail_telemetry_requires_engine_durable_workload
            -P "${CMAKE_CURRENT_SOURCE_DIR}/check_invalid_cli.cmake"
  )

  add_test(
    NAME order_books_benchmark_engine_tail_telemetry_smoke
    COMMAND ${CMAKE_COMMAND}
            "-DPROGRAM=$<TARGET_FILE:order_books_benchmark>"
            "-DOUTPUT_PATH=${CMAKE_CURRENT_BINARY_DIR}/engine-tail-telemetry-smoke.csv"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/check_engine_tail_telemetry.cmake"
  )

  add_test(
    NAME order_books_benchmark_engine_tail_telemetry_collector_only_smoke
    COMMAND ${CMAKE_COMMAND}
            "-DPROGRAM=$<TARGET_FILE:order_books_benchmark>"
            "-DOUTPUT_PATH=${CMAKE_CURRENT_BINARY_DIR}/engine-tail-telemetry-collector-only.csv"
            -DSTATE_SAMPLING=off
            -P "${CMAKE_CURRENT_SOURCE_DIR}/check_engine_tail_telemetry.cmake"
  )

  add_test(
    NAME order_books_benchmark_writer_phase_profile_off_smoke
    COMMAND $<TARGET_FILE:order_books_benchmark>
            --workload=engine_writer_hot_path_profile
            --writer-phase-profile=off
            --engine-group-size=2
            --engine-group-delay-us=0
            --engine-producer-lanes=2
            --iterations=2
            --warmup=1
  )
  set_tests_properties(
    order_books_benchmark_writer_phase_profile_off_smoke
    PROPERTIES
      PASS_REGULAR_EXPRESSION
      "workload=engine_writer_hot_path_profile phase=summary.*profile=off.*correctness_verified=true"
  )

  add_test(
    NAME order_books_benchmark_writer_phase_profile_on_smoke
    COMMAND $<TARGET_FILE:order_books_benchmark>
            --workload=engine_writer_hot_path_profile
            --writer-phase-profile=on
            --engine-group-size=2
            --engine-group-delay-us=0
            --engine-producer-lanes=2
            --iterations=2
            --warmup=8
  )
  set_tests_properties(
    order_books_benchmark_writer_phase_profile_on_smoke
    PROPERTIES
      PASS_REGULAR_EXPRESSION
      "workload=engine_writer_hot_path_profile phase=profile.*profiled_groups=.*profiled_input_commands=4.*profiled_accepted_commands=4.*wal_lock_wait_ns=.*completion_service_rps=.*completion_max_queue_depth=.*"
  )

  add_test(
    NAME order_books_benchmark_writer_apply_subprofile_on_smoke
    COMMAND ${CMAKE_COMMAND}
            "-DPROGRAM=$<TARGET_FILE:order_books_benchmark>"
            -DCLI_ARGUMENT_1=--workload=engine_writer_hot_path_profile
            -DCLI_ARGUMENT_2=--writer-phase-profile=on
            -DCLI_ARGUMENT_3=--writer-apply-subprofile=on
            -DCLI_ARGUMENT_4=--engine-group-size=2
            -DCLI_ARGUMENT_5=--engine-group-delay-us=0
            -DCLI_ARGUMENT_6=--engine-producer-lanes=2
            -DCLI_ARGUMENT_7=--iterations=2
            -DCLI_ARGUMENT_8=--warmup=8
            "-DREQUIRED_REGEXES=workload=engine_writer_hot_path_profile phase=profile.*writer_apply_subprofile=on.*apply_precheck_ns=.*apply_book_apply_ns=.*apply_state_update_ns=.*apply_output_events_ns=.*apply_producer_result_ns=.*apply_incremental_validation_ns=.*apply_unattributed_ns=.*apply_unattributed_ns_per_command=.*apply_unattributed_apply_parent_share_percent=.*apply_unattributed_writer_service_share_percent=.*apply_commands=4.*apply_events=.*apply_trades=.*;writer_service_ceiling_observed=true.*writer_service_derived_ceiling_commands_per_second=[0-9]"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/check_writer_profile_output.cmake"
  )

  add_test(
    NAME order_books_benchmark_writer_apply_subprofile_off_smoke
    COMMAND ${CMAKE_COMMAND}
            "-DPROGRAM=$<TARGET_FILE:order_books_benchmark>"
            -DCLI_ARGUMENT_1=--workload=engine_writer_hot_path_profile
            -DCLI_ARGUMENT_2=--writer-phase-profile=on
            -DCLI_ARGUMENT_3=--writer-apply-subprofile=off
            -DCLI_ARGUMENT_4=--engine-group-size=2
            -DCLI_ARGUMENT_5=--engine-group-delay-us=0
            -DCLI_ARGUMENT_6=--engine-producer-lanes=2
            -DCLI_ARGUMENT_7=--iterations=2
            -DCLI_ARGUMENT_8=--warmup=8
            "-DREQUIRED_REGEXES=workload=engine_writer_hot_path_profile phase=summary.*commands=4.*wal_group_commits=[1-9][0-9]*.*wal_group_commands=4.*actual_commands_per_group=[0-9].*publisher_lag_events=[0-9][0-9]*.*publisher_lag_bytes=[0-9][0-9]*.*publisher_lag_age_ns=[0-9][0-9]*.*correctness_verified=true;workload=engine_writer_hot_path_profile phase=profile.*writer_apply_subprofile=off.*apply_share_percent=.*wal_rotation_ceiling_observed=false"
            "-DFORBIDDEN_REGEXES=apply_precheck_ns=;apply_book_apply_ns=;apply_state_update_ns=;apply_output_events_ns=;apply_producer_result_ns=;apply_incremental_validation_ns=;apply_children_ns=;apply_unattributed_ns=;apply_commands=;apply_events=;apply_trades=;wal_rotation_derived_ceiling_commands_per_second=;wal_rotation_headroom_percent="
            -P "${CMAKE_CURRENT_SOURCE_DIR}/check_writer_profile_output.cmake"
  )

  add_test(
    NAME order_books_benchmark_writer_parallel_prepare_smoke
    COMMAND $<TARGET_FILE:order_books_benchmark>
            --workload=engine_writer_hot_path_profile
            --writer-phase-profile=on
            --engine-group-size=4
            --engine-group-delay-us=1000
            --engine-producer-lanes=4
            --wal-prepare-workers=4
            --wal-parallel-prepare-min-commands=2
            --iterations=2
            --warmup=8
  )
  set_tests_properties(
    order_books_benchmark_writer_parallel_prepare_smoke
    PROPERTIES
      PASS_REGULAR_EXPRESSION
      "workload=engine_writer_hot_path_profile phase=summary.*actual_parallel_prepare_groups=[1-9][0-9]*.*actual_prepare_tasks=[1-9][0-9]*"
  )

  add_test(
    NAME order_books_benchmark_wal_prepare_workers_two_smoke
    COMMAND $<TARGET_FILE:order_books_benchmark>
            --workload=wal_write_ceiling
            --wal-prepare-workers=2
            --wal-parallel-prepare-min-commands=2
            --wal-sync=none
            --wal-group-size=2
            --iterations=2
            --warmup=1
  )
  set_tests_properties(
    order_books_benchmark_wal_prepare_workers_two_smoke
    PROPERTIES
      PASS_REGULAR_EXPRESSION
      "wal_prepare_workers=2.*actual_parallel_prepare_groups=[1-9][0-9]*.*actual_prepare_tasks=[1-9][0-9]*.*replay_verified=true"
  )

  add_test(
    NAME order_books_benchmark_writer_parallel_prepare_profile_off_smoke
    COMMAND $<TARGET_FILE:order_books_benchmark>
            --workload=engine_writer_hot_path_profile
            --writer-phase-profile=off
            --engine-group-size=4
            --engine-group-delay-us=1000
            --engine-producer-lanes=4
            --wal-prepare-workers=4
            --wal-parallel-prepare-min-commands=2
            --iterations=2
            --warmup=8
  )
  set_tests_properties(
    order_books_benchmark_writer_parallel_prepare_profile_off_smoke
    PROPERTIES
      PASS_REGULAR_EXPRESSION
      "workload=engine_writer_hot_path_profile phase=summary.*profile=off.*actual_parallel_prepare_groups=[1-9][0-9]*.*actual_prepare_tasks=[1-9][0-9]*"
  )
endif()
