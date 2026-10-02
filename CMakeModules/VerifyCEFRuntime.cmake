# Verify normal loader resolution without the environment overrides used by some
# capture tools. CEF locates ICU beside libcef, so selecting the SDK copy crashes.
find_program(LDD_EXECUTABLE ldd)
if(NOT LDD_EXECUTABLE)
    message(FATAL_ERROR "ldd is required to verify the packaged CEF runtime")
endif()

get_filename_component(runtime_directory "${APPLICATION}" DIRECTORY)
get_filename_component(expected_cef "${runtime_directory}/libcef.so" ABSOLUTE)
get_filename_component(expected_library "${APPLICATION_LIBRARY}" ABSOLUTE)
foreach(binary "${APPLICATION}" "${APPLICATION_LIBRARY}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env --unset=LD_LIBRARY_PATH --unset=LD_PRELOAD
            "${LDD_EXECUTABLE}" "${binary}"
        RESULT_VARIABLE result OUTPUT_VARIABLE dependencies ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Cannot inspect ${binary}: ${error}")
    endif()
    foreach(dependency libcef.so liblinux-wallpaperengine-lib.so)
        if(dependency STREQUAL "liblinux-wallpaperengine-lib.so" AND binary STREQUAL APPLICATION_LIBRARY)
            continue()
        endif()
        string(REGEX MATCH "${dependency} => ([^\n]+) \\(0x[0-9a-f]+\\)" match "${dependencies}")
        if(NOT match)
            message(FATAL_ERROR "${binary} does not resolve ${dependency}: ${dependencies}")
        endif()
        # Preserve the loader's selected pathname: an output symlink and the SDK
        # file can have the same realpath but different CEF resource directories.
        get_filename_component(resolved "${CMAKE_MATCH_1}" ABSOLUTE)
        if(dependency STREQUAL "libcef.so")
            set(expected "${expected_cef}")
        else()
            set(expected "${expected_library}")
        endif()
        if(NOT resolved STREQUAL expected)
            message(FATAL_ERROR "${binary} resolves ${dependency} to ${resolved}, expected ${expected}")
        endif()
    endforeach()
endforeach()

foreach(resource icudtl.dat resources.pak chrome_100_percent.pak chrome_200_percent.pak
                 v8_context_snapshot.bin locales/en-US.pak)
    if(NOT EXISTS "${runtime_directory}/${resource}")
        message(FATAL_ERROR "Missing packaged CEF resource: ${runtime_directory}/${resource}")
    endif()
endforeach()
