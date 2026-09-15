# Copies the DLLs a MinGW executable needs next to it (or into DLL_SUBDIR with a
# private assembly manifest). Run post-build by CMakeLists.txt.
if(POLICY CMP0207)
    cmake_policy(SET CMP0207 NEW)
endif()

set(CMAKE_GET_RUNTIME_DEPENDENCIES_PLATFORM "windows+pe")
set(CMAKE_GET_RUNTIME_DEPENDENCIES_TOOL "objdump")
set(CMAKE_GET_RUNTIME_DEPENDENCIES_COMMAND "${OBJDUMP}")

file(GET_RUNTIME_DEPENDENCIES
    EXECUTABLES "${EXE}"
    RESOLVED_DEPENDENCIES_VAR deps
    UNRESOLVED_DEPENDENCIES_VAR missing
    # Copies from an earlier build conflict with the originals.
    CONFLICTING_DEPENDENCIES_PREFIX conflict
    DIRECTORIES ${SEARCH_DIRS}
    PRE_EXCLUDE_REGEXES "^[Aa][Pp][Ii]-[Mm][Ss]-" "^[Ee][Xx][Tt]-[Mm][Ss]-"
    POST_EXCLUDE_REGEXES "[/\\\\][Ww][Ii][Nn][Dd][Oo][Ww][Ss][/\\\\]"
)

get_filename_component(exe_dir "${EXE}" DIRECTORY)
file(REAL_PATH "${exe_dir}" exe_dir)
foreach(name IN LISTS conflict_FILENAMES)
    foreach(path IN LISTS conflict_${name})
        get_filename_component(dir "${path}" DIRECTORY)
        file(REAL_PATH "${dir}" dir)
        if(NOT dir STREQUAL exe_dir)
            list(APPEND deps "${path}")
        endif()
    endforeach()
endforeach()

set(dest "${exe_dir}")
if(DLL_SUBDIR)
    set(dest "${exe_dir}/${DLL_SUBDIR}")
endif()
foreach(dll IN LISTS deps)
    file(COPY "${dll}" DESTINATION "${dest}")
endforeach()

if(DLL_SUBDIR)
    set(names "")
    foreach(dll IN LISTS deps)
        get_filename_component(n "${dll}" NAME)
        list(APPEND names "${n}")
    endforeach()
    list(REMOVE_DUPLICATES names)
    list(SORT names CASE INSENSITIVE)

    set(manifest "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n")
    string(APPEND manifest "<assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\" manifestVersion=\"1.0\">\n")
    string(APPEND manifest "  <assemblyIdentity type=\"win32\" name=\"${DLL_SUBDIR}\" version=\"1.0.0.0\" processorArchitecture=\"${ARCH}\"/>\n")
    foreach(n IN LISTS names)
        string(APPEND manifest "  <file name=\"${n}\"/>\n")
    endforeach()
    string(APPEND manifest "</assembly>\n")

    set(path "${dest}/${DLL_SUBDIR}.manifest")
    set(old "")
    if(EXISTS "${path}")
        file(READ "${path}" old)
    endif()
    if(NOT old STREQUAL manifest)
        file(WRITE "${path}" "${manifest}")
    endif()
endif()

if(missing)
    message(WARNING "Could not find DLLs needed by ${EXE}: ${missing}")
endif()
