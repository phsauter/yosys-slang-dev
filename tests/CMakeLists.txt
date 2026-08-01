if (WITH_COVERAGE)
    if (CMAKE_CXX_COMPILER_ID STREQUAL Clang)
        set(test_launcher
            ${CMAKE_COMMAND} -E env
            LLVM_PROFILE_FILE=${CMAKE_BINARY_DIR}/yosys-slang.%p.profraw
        )

        add_custom_target(
            coverage
            COMMAND
                llvm-profdata merge -sparse
                yosys-slang.*.profraw
                -o yosys-slang.profdata
            COMMAND
                llvm-cov export -format lcov
                $<TARGET_FILE:yosys-slang>
                -instr-profile=yosys-slang.profdata
                --ignore-filename-regex=/third_party/
                --ignore-filename-regex=^/usr
                > yosys-slang.info
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        )
    endif()
endif()

set(ALL_TESTS
    unit/async.ys
    unit/dualedge.ys
    unit/function_call.ys
    unit/latch.ys
    unit/selftests.tcl
    various/arch_variants.ys
    various/arch_variants_diag.ys
    various/assign_mixing.ys
    various/aggregate_masked_write.ys
    various/assignment_pattern_lhs.ys
    various/bb_detect.ys
    various/write_domains.ys
    various/blackbox_scenarios.ys
    various/bus_range.ys
    various/concurrent_assert.ys
    various/defaults.ys
    various/delays.ys
    various/dualedge.ys
    various/expr.ys
    various/flop_naming.ys
    various/formal_stmts.ys
    various/hierref_error.ys
    various/huge_reg.ys
    various/ignore_asserts.ys
    various/init.ys
    various/intf_array_naming.ys
    various/intf_w_hierarchy.ys
    various/issue142.ys
    various/issue208.ys
    various/issue227.ys
    various/issue240.ys
    various/issue266.ys
    various/issue300.ys
    various/issue50.ys
    various/mem_inference.ys
    various/meminit.ys
    various/module_uniquify.ys
    various/pragmas.ys
    various/readmem.ys
    various/readmem_diag.ys
    various/regress.ys
    various/stringattrs.ys
    various/stringparams.ys
    various/symbolic_updates.ys
    various/timescale.ys
    various/top_attr.ys
    various/unknown_cells.ys
    various/toplevel_intf_unsupported.ys
    various/update_map.ys
    various/update_map_automatic_local.ys
    various/update_map_control_flow_round2.ys
    various/update_map_compound_assignment.ys
    various/update_map_datatypes_round2.ys
    various/update_map_enum_struct_case.ys
    various/update_map_function_multi_output_args.ys
    various/update_map_function_nested_calls.ys
    various/update_map_function_output_arg.ys
    various/update_map_function_static_local.ys
    various/update_map_function_static_expression_round2.ys
    various/update_map_function_struct_return.ys
    various/update_map_function_upward_ref.ys
    various/update_map_function_upward_ref_for_init.ys
    various/update_map_function_upward_ref_for_step.ys
    various/update_map_function_upward_ref_local_init.ys
    various/update_map_fallback_automatic_local.ys
    various/update_map_generic_interface.ys
    various/update_map_instance_array.ys
    various/update_map_interface_array_generate.ys
    various/update_map_interface_modport.ys
    various/update_map_masked_view.ys
    various/update_map_lowering.ys
    various/update_map_multidim_unpacked_struct_array.ys
    various/update_map_multi_block_round2.ys
    various/update_map_nested_automatic_locals.ys
    various/update_map_packed_struct_dynamic_lhs.ys
    various/update_map_packed_union_members.ys
    various/update_map_pending_rhs_read.ys
    various/update_map_read_after_write_round2.ys
    various/update_map_signed_typedef_partselect.ys
    various/update_map_single_bit_struct_bits.ys
    various/update_map_subroutines_round2.ys
    various/update_map_task_output_args.ys
    various/update_map_unpacked_array_of_struct.ys
    various/update_map_unpacked_struct.ys
    various/update_map_unpacked_union.ys
    various/wait_test.ys
    various/expect_test.ys
    various/program_test.ys
    various/synthesis_define.ys
)

foreach(test_script ${ALL_TESTS})
    set(test_fullpath ${CMAKE_CURRENT_SOURCE_DIR}/${test_script})
    get_filename_component(test_dir ${test_fullpath} DIRECTORY)
    add_test(
        NAME ${test_script}
        WORKING_DIRECTORY ${test_dir}
        COMMAND
            ${test_launcher}
            ${YOSYS_BINDIR}/yosys -m $<TARGET_FILE:yosys-slang> ${test_fullpath}
    )
endforeach()
