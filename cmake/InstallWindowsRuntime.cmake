# Stage only the recursive PE import closure; targets retain their install owner.
function(atrinik_install_windows_runtime destination)
    find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
    find_program(ATRINIK_LLVM_READOBJ NAMES llvm-readobj REQUIRED)
    set(ATRINIK_WINDOWS_LLVM_LICENSE_DIR
        "${ATRINIK_WINDOWS_RUNTIME_DIR}/../share/licenses/llvm-mingw"
        CACHE PATH "LLVM-MinGW notices for compiler-rt, CRT and runtime DLLs")
    foreach (notice LICENSE.TXT COPYING COPYING.MinGW-w64-runtime.txt
            COPYING.MinGW-w64.txt COPYING.winpthreads.txt COPYING.winstorecompat.txt)
        if (NOT EXISTS "${ATRINIK_WINDOWS_LLVM_LICENSE_DIR}/${notice}" OR
                IS_DIRECTORY "${ATRINIK_WINDOWS_LLVM_LICENSE_DIR}/${notice}" OR
                IS_SYMLINK "${ATRINIK_WINDOWS_LLVM_LICENSE_DIR}/${notice}")
            message(FATAL_ERROR "LLVM-MinGW package notice is missing: ${notice}")
        endif ()
        install(FILES "${ATRINIK_WINDOWS_LLVM_LICENSE_DIR}/${notice}"
            DESTINATION "${destination}/licenses/llvm-mingw")
    endforeach ()
    set(ATRINIK_WINDOWS_DEPENDENCY_INVENTORY_DIR
        "${ATRINIK_WINDOWS_RUNTIME_DIR}/../share/atrinik/windows-dependencies"
        CACHE PATH "Producer inventory for rebuilt Windows dependencies")
    set(dependency_inventory "${ATRINIK_WINDOWS_DEPENDENCY_INVENTORY_DIR}/dependencies.json")
    set(pe_inventory "${ATRINIK_WINDOWS_DEPENDENCY_INVENTORY_DIR}/pe-closure.json")
    foreach (inventory "${dependency_inventory}" "${pe_inventory}")
        if (NOT EXISTS "${inventory}" OR IS_DIRECTORY "${inventory}" OR IS_SYMLINK "${inventory}")
            message(FATAL_ERROR "Windows dependency inventory is missing: ${inventory}")
        endif ()
    endforeach ()
    file(READ "${dependency_inventory}" dependency_manifest)
    string(JSON schema GET "${dependency_manifest}" schema_version)
    if (NOT schema EQUAL 1)
        message(FATAL_ERROR "Unsupported Windows dependency inventory schema")
    endif ()
    string(JSON dependency_count LENGTH "${dependency_manifest}" dependencies)
    if (dependency_count LESS 1 OR dependency_count GREATER 128)
        message(FATAL_ERROR "Windows dependency inventory has an invalid size")
    endif ()
    math(EXPR last_dependency "${dependency_count} - 1")
    set(dependency_names)
    foreach (index RANGE ${last_dependency})
        string(JSON name GET "${dependency_manifest}" dependencies ${index} name)
        if (NOT name MATCHES "^[A-Za-z0-9_+.-]+$" OR name STREQUAL "." OR name STREQUAL ".." OR
                name IN_LIST dependency_names)
            message(FATAL_ERROR "Invalid or duplicate Windows dependency name: ${name}")
        endif ()
        list(APPEND dependency_names "${name}")
        string(JSON notice_count LENGTH "${dependency_manifest}" dependencies ${index} licenses)
        if (notice_count LESS 1 OR notice_count GREATER 64)
            message(FATAL_ERROR "Windows dependency has an invalid notice count: ${name}")
        endif ()
        math(EXPR last_notice "${notice_count} - 1")
        foreach (notice_index RANGE ${last_notice})
            string(JSON source_notice GET "${dependency_manifest}" dependencies ${index} licenses ${notice_index})
            if (NOT source_notice MATCHES "^[A-Za-z0-9_+./-]+$" OR source_notice MATCHES "(^/|(^|/)\\.\\.(/|$))")
                message(FATAL_ERROR "Unsafe Windows dependency notice path: ${source_notice}")
            endif ()
            string(REPLACE "/" "_" notice "${source_notice}")
            set(notice_file "${ATRINIK_WINDOWS_RUNTIME_DIR}/../share/licenses/${name}/${notice}")
            if (NOT EXISTS "${notice_file}" OR IS_DIRECTORY "${notice_file}" OR IS_SYMLINK "${notice_file}")
                message(FATAL_ERROR "Windows dependency notice is missing: ${name}/${notice}")
            endif ()
            install(FILES "${notice_file}" DESTINATION "${destination}/licenses/${name}")
        endforeach ()
    endforeach ()
    install(FILES "${dependency_inventory}" DESTINATION "${destination}/licenses"
        RENAME windows-dependencies.json)
    install(FILES "${pe_inventory}" DESTINATION "${destination}/licenses"
        RENAME windows-pe-closure.json)
    if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/ci/stage_windows_runtime.py")
        set(runtime_helper "${CMAKE_CURRENT_SOURCE_DIR}/tools/ci/stage_windows_runtime.py")
    else ()
        set(runtime_helper "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../tools/ci/stage_windows_runtime.py")
    endif ()
    if (NOT EXISTS "${runtime_helper}")
        message(FATAL_ERROR "Windows runtime closure helper is missing: ${runtime_helper}")
    endif ()
    set(runtime_arguments "--runtime-dir \"${ATRINIK_WINDOWS_RUNTIME_DIR}\"")
    if (ATRINIK_WINDOWS_PYTHON_RUNTIME_DIR)
        string(APPEND runtime_arguments
            " --runtime-dir \"${ATRINIK_WINDOWS_PYTHON_RUNTIME_DIR}\"")
        # Preserve the externally built Python extension modules, and inspect
        # their dependencies without replacing their runtime ownership.
        file(GLOB python_modules "${ATRINIK_WINDOWS_PYTHON_RUNTIME_DIR}/*.pyd"
            "${ATRINIK_WINDOWS_PYTHON_RUNTIME_DIR}/*.dll")
        list(APPEND ARGN ${python_modules})
    endif ()
    foreach (binary IN LISTS ARGN)
        string(APPEND runtime_arguments " \"${binary}\"")
    endforeach ()
    set(stage "${CMAKE_CURRENT_BINARY_DIR}/windows-runtime-stage")
    set(script "${CMAKE_CURRENT_BINARY_DIR}/install-windows-runtime-$<CONFIG>.cmake")
    file(GENERATE OUTPUT "${script}" CONTENT "
file(LOCK \"${CMAKE_CURRENT_BINARY_DIR}/windows-runtime.lock\" GUARD PROCESS TIMEOUT 30)
file(REMOVE_RECURSE \"${stage}\")
execute_process(COMMAND \"${Python3_EXECUTABLE}\" \"${runtime_helper}\"
    --readobj \"${ATRINIK_LLVM_READOBJ}\" --dlls-only --output-dir \"${stage}\"
    ${runtime_arguments}
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if (NOT result EQUAL 0)
    message(FATAL_ERROR \"Windows runtime closure failed: \${error}\")
endif ()
file(INSTALL DESTINATION \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${destination}\"
    TYPE DIRECTORY FILES \"${stage}/\")
")
    install(SCRIPT "${script}")
endfunction()
