# Regression tests. These drive the real oclgdb binary against the real pocl CPU
# device under real ptrace -- there is nothing simulated here, which is the whole point
# of the project. Each test is a shell script that runs oclgdb --batch -x <script> and
# checks its output.
find_program(BASH_EXE bash)

function(oclgdb_test name)
  add_test(NAME ${name}
           COMMAND ${BASH_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/run_test.sh
                   $<TARGET_FILE:oclgdb> ${CMAKE_CURRENT_SOURCE_DIR}/${name}.txt
                   ${CMAKE_SOURCE_DIR})
  set_tests_properties(${name} PROPERTIES TIMEOUT 60)
endfunction()

oclgdb_test(phase1_single_workitem)
oclgdb_test(phase2_multi_workitem)
oclgdb_test(phase3_bug_diagnosis)
