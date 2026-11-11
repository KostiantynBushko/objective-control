# Runs the application with --auto-enroll against a synthetic camera and a
# file store, and checks that nothing was written to the database.
#
# A test pattern has no faces in it, so the application has nothing to
# store -- and the promise of the deferred mode is that a face that is not
# confirmed leaves nothing behind. The database file is not even created,
# because nothing was written to it.
#
# The run is short on purpose: fewer frames than the tracker collects views
# for, so the faces of the run cannot be confirmed and nothing can be
# stored -- whatever the detector makes of a test pattern.
#
# The test is driven from CMake, so that it does not depend on a shell.

if(NOT DEFINED APP OR NOT DEFINED DB)
    message(FATAL_ERROR "APP and DB have to be given")
endif()

if(NOT DEFINED DETECTOR OR NOT DEFINED RECOGNIZER)
    message(FATAL_ERROR "DETECTOR and RECOGNIZER have to be given")
endif()

file(REMOVE ${DB})

execute_process(
    COMMAND ${APP}
            --mode=synthetic --width=320 --height=240 --frames=2
            --auto-enroll --face-db=${DB}
            --face-model=${DETECTOR}
            --face-recognition-model=${RECOGNIZER}
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE errors)

if(NOT result EQUAL 0)
    message(FATAL_ERROR
        "the application failed (${result}):\n${output}\n${errors}")
endif()

# The report of the deferred mode has to be there: it says that the faces
# were watched and that none of them was stored.
if(NOT output MATCHES "Tracked [0-9]+ face")
    message(FATAL_ERROR "the report of the deferred mode is missing:\n${output}")
endif()

if(NOT output MATCHES "0 were stored")
    message(FATAL_ERROR "something was stored:\n${output}")
endif()

# Nothing was written, so the database was not even created.
if(EXISTS ${DB})
    message(FATAL_ERROR "the database was written:\n${output}")
endif()

message(STATUS "the deferred mode wrote nothing to the database")
