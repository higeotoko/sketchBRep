# ctest helper: run COMMAND (arguments separated by '|') with INPUT on stdin
# and check the output.
#   cmake -DCOMMAND=exe|arg... -DINPUT=file -DEXPECT=regex [-DREJECT=regex] -P RunWithInput.cmake
string(REPLACE "|" ";" command "${COMMAND}")
execute_process(COMMAND ${command} INPUT_FILE ${INPUT} OUTPUT_VARIABLE out ERROR_VARIABLE err RESULT_VARIABLE rc)
message("${out}")
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "exit code ${rc}: ${err}")
endif()
if(NOT out MATCHES "${EXPECT}")
  message(FATAL_ERROR "output does not match '${EXPECT}'")
endif()
if(DEFINED REJECT AND out MATCHES "${REJECT}")
  message(FATAL_ERROR "output matches '${REJECT}'")
endif()
