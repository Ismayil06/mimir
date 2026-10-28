option(MIM_ASCEND_MLIR "Build ascend.emit_hivm against the MLIR C++ API." ON)

set(ASCEND_MLIR OFF)
if(MIM_ASCEND_MLIR)
    if(NOT MLIR_DIR)
        foreach(ver 19 18 17 16 15)
            if(EXISTS "/usr/lib/llvm-${ver}/lib/cmake/mlir/MLIRConfig.cmake")
                set(MLIR_DIR "/usr/lib/llvm-${ver}/lib/cmake/mlir")
                break()
            endif()
        endforeach()
    endif()
    find_package(MLIR CONFIG)
    if(MLIR_FOUND)
        set(ASCEND_MLIR ON)
    else()
        message(STATUS "MimIR ascend: MLIR not found; ascend.emit_hivm will reject programs")
    endif()
endif()

set(ASCEND_SOURCES
    ascend.cpp
    hfusion.cpp
)
if(ASCEND_MLIR)
    list(APPEND ASCEND_SOURCES
        hivm.cpp
        mlir/hivm_dialect.cpp
        mlir/hacc_dialect.cpp
    )
else()
    list(APPEND ASCEND_SOURCES hivm_stub.cpp)
endif()

add_mim_plugin(ascend
    SOURCES
        ${ASCEND_SOURCES}
)

if(ASCEND_MLIR)
    target_include_directories(mim_ascend SYSTEM PRIVATE ${MLIR_INCLUDE_DIRS})
    separate_arguments(ASCEND_LLVM_DEFS NATIVE_COMMAND "${LLVM_DEFINITIONS}")
    target_compile_options(mim_ascend PRIVATE ${ASCEND_LLVM_DEFS})
    target_link_libraries(mim_ascend PRIVATE
        MLIRFuncDialect
        MLIRMemRefDialect
        MLIRIR
        MLIRSupport
    )
    if(LLVM_LIBRARY_DIRS)
        set_target_properties(mim_ascend PROPERTIES
            BUILD_RPATH "${LLVM_LIBRARY_DIRS}"
            INSTALL_RPATH "${LLVM_LIBRARY_DIRS}"
        )
    endif()
endif()
