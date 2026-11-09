# Warnings.cmake
#
# Apply a consistent, strict-but-reasonable warning set to a target.
# Usage: warn_on(<target>)

function(warn_on target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
        return()
    endif()

    target_compile_options(${target} PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wshadow
        -Wold-style-cast
    )
endfunction()
