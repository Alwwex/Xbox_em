# Runs xbem and checks its exit code and output.
#   -DXBEM=<path>  -DARGS=<a|b|c>  -DEXPECT_EXIT=<n>  -DEXPECT=<regex@@regex...>
string(REPLACE "|" ";" args "${ARGS}")
execute_process(
  COMMAND ${XBEM} ${args}
  RESULT_VARIABLE rc
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err
  TIMEOUT 60)
set(all "${out}\n${err}")

if(NOT "${rc}" STREQUAL "${EXPECT_EXIT}")
  message(FATAL_ERROR "xbem ${ARGS}: exit code ${rc}, expected ${EXPECT_EXIT}\n--- output ---\n${all}")
endif()

if(EXPECT)
  string(REPLACE "@@" ";" patterns "${EXPECT}")
  foreach(re IN LISTS patterns)
    if(NOT all MATCHES "${re}")
      message(FATAL_ERROR "xbem ${ARGS}: output does not match '${re}'\n--- output ---\n${all}")
    endif()
  endforeach()
endif()
