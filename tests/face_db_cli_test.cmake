# Runs the commands of the face database against a file store.
#
# The commands do not need a camera and no model: they work on the
# samples that are in the store. The test checks the parts that do not
# need a face: the errors of a bad value, the report of a database that
# holds nobody, and the list of the people.
#
# The test is driven from CMake, so that it does not depend on a shell.

if(NOT DEFINED APP OR NOT DEFINED DB)
    message(FATAL_ERROR "APP and DB have to be given")
endif()

file(REMOVE ${DB})

# --face-db-duplicates without --face-db is a usage error.
execute_process(
    COMMAND ${APP} --face-db-duplicates
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE errors)

if(NOT result EQUAL 2)
    message(FATAL_ERROR
        "--face-db-duplicates without --face-db should exit with 2, "
        "but it exited with ${result}:\n${output}\n${errors}")
endif()

# A database that does not exist holds nobody, and nobody can be a
# duplicate of somebody.
execute_process(
    COMMAND ${APP} --face-db=${DB} --face-db-duplicates
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE errors)

if(NOT result EQUAL 0)
    message(FATAL_ERROR
        "--face-db-duplicates failed (${result}):\n${output}\n${errors}")
endif()

if(NOT output MATCHES "No two people")
    message(FATAL_ERROR
        "the report of the duplicates is missing:\n${output}")
endif()

# A merge that names the same person twice is a usage error.
execute_process(
    COMMAND ${APP} --face-db=${DB} --face-db-merge=1:1
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE errors)

if(NOT result EQUAL 2)
    message(FATAL_ERROR
        "a merge of a person with itself should exit with 2, but it exited "
        "with ${result}:\n${output}\n${errors}")
endif()

# A merge whose value is not a pair of ids is a usage error.
execute_process(
    COMMAND ${APP} --face-db=${DB} --face-db-merge=nonsense
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE errors)

if(NOT result EQUAL 2)
    message(FATAL_ERROR
        "a merge with a bad value should exit with 2, but it exited with "
        "${result}:\n${output}\n${errors}")
endif()

# A merge of a person that is not in the database says so.
execute_process(
    COMMAND ${APP} --face-db=${DB} --face-db-merge=7:1
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE errors)

if(NOT result EQUAL 1)
    message(FATAL_ERROR
        "a merge of an unknown person should exit with 1, but it exited with "
        "${result}:\n${output}\n${errors}")
endif()

# The number of samples that are averaged has to be a number.
execute_process(
    COMMAND ${APP} --face-db=${DB} --face-match-samples=0 --face-db-duplicates
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE errors)

if(NOT result EQUAL 2)
    message(FATAL_ERROR
        "--face-match-samples=0 should exit with 2, but it exited with "
        "${result}:\n${output}\n${errors}")
endif()

message(STATUS "the commands of the face database behave as they should")
