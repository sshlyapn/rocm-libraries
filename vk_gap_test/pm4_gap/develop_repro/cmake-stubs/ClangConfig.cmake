# Minimal stub so ROCr's trap_handler / blit CMakeLists can import the device
# assembler tools. This image ships the clang/llvm BINARIES but not the upstream
# LLVM/Clang CMake dev packages, so find_package(Clang) would otherwise fail. We
# only need the imported executable targets `clang` (and friends); the device
# .s -> .hsaco custom commands invoke them directly. Plain ASCII only.
set(_rocm_llvm_bin "/opt/rocm/llvm/bin")
foreach(_t clang clang-offload-bundler)
  if(NOT TARGET ${_t})
    add_executable(${_t} IMPORTED GLOBAL)
    set_target_properties(${_t} PROPERTIES IMPORTED_LOCATION "${_rocm_llvm_bin}/${_t}")
  endif()
endforeach()
set(Clang_FOUND TRUE)
set(CLANG_FOUND TRUE)
