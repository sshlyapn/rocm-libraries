# Minimal stub (see ClangConfig.cmake). Imports the LLVM binutils targets that
# ROCr's device-code CMakeLists reference and provides the few LLVM_* variables
# they read. Plain ASCII only.
set(_rocm_llvm_bin "/opt/rocm/llvm/bin")
foreach(_t llvm-objcopy llvm-mc llvm-dis opt llvm-link)
  if(NOT TARGET ${_t})
    add_executable(${_t} IMPORTED GLOBAL)
    set_target_properties(${_t} PROPERTIES IMPORTED_LOCATION "${_rocm_llvm_bin}/${_t}")
  endif()
endforeach()
set(LLVM_FOUND TRUE)
set(LLVM_PACKAGE_VERSION "22.0.0")
set(LLVM_VERSION_MAJOR 22)
set(LLVM_VERSION_MINOR 0)
set(LLVM_VERSION_PATCH 0)
set(LLVM_INCLUDE_DIRS "/opt/rocm/llvm/include")
set(LLVM_LIBRARY_DIRS "/opt/rocm/llvm/lib")
set(LLVM_TOOLS_BINARY_DIR "${_rocm_llvm_bin}")
