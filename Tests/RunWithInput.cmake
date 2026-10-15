# Runs COMMAND (a ;-list) with INPUT on its stdin, each | a newline, and prints its output, so a
# test can type into a program that reads the terminal.
string(REPLACE "|" "
" text "${INPUT}")
file(MAKE_DIRECTORY ${OUTPUT_DIRECTORY})
file(WRITE ${OUTPUT_DIRECTORY}/input.txt "${text}")
execute_process(COMMAND ${COMMAND} INPUT_FILE ${OUTPUT_DIRECTORY}/input.txt RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "exited with ${result}")
endif()
