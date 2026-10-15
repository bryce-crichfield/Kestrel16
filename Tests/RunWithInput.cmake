# Runs COMMAND (a ;-list) with INPUT on its stdin, each | a newline, and prints its output, so a
# test can type into a program that reads the terminal. stdout is printed whole before stderr,
# so a status line on stderr can't land in the middle of what the program printed.
string(REPLACE "|" "\n" text "${INPUT}")
file(MAKE_DIRECTORY ${OUTPUT_DIRECTORY})
file(WRITE ${OUTPUT_DIRECTORY}/input.txt "${text}")
execute_process(COMMAND ${COMMAND} INPUT_FILE ${OUTPUT_DIRECTORY}/input.txt RESULT_VARIABLE result
                OUTPUT_VARIABLE output ERROR_VARIABLE errors)
message("${output}")
message("${errors}")
if(NOT result EQUAL 0)
    message(FATAL_ERROR "exited with ${result}")
endif()
