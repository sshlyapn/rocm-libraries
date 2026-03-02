# Copyright (c) Advanced Micro Devices, Inc., or its affiliates.
# SPDX-License-Identifier: MIT

# Keep this example intentionally self-contained: it does not include any headers from other
# examples/tests. It only depends on the public ck_tile headers under include/.

if(GPU_TARGETS MATCHES "gfx94|gfx95|gfx90a")
  add_executable(tile_example_gemm_universal_minimal universal_gemm_minimal.cpp)
  set_source_files_properties(universal_gemm_minimal.cpp PROPERTIES LANGUAGE HIP)

  add_executable(tile_example_gemm_universal_persistent universal_gemm_persistent.cpp)
  set_source_files_properties(universal_gemm_persistent.cpp PROPERTIES LANGUAGE HIP)

  add_executable(tile_example_gemm_universal_persistent_all_reduce universal_gemm_persistent_all_reduce.cpp)
  set_source_files_properties(universal_gemm_persistent_all_reduce.cpp PROPERTIES LANGUAGE HIP)

  add_executable(tile_example_gemm_universal_chuncked universal_gemm_chuncked.cpp)
  set_source_files_properties(universal_gemm_chuncked.cpp PROPERTIES LANGUAGE HIP)

  # Three flavours of the _ref executable from the same source file:
  #   _ref_default   – plain build (no rocshmem, no -fgpu-rdc)
  #   _ref_rocshmem  – linked with rocshmem
  #   _ref_rdc       – compiled/linked with -fgpu-rdc
  set(_ref_source universal_gemm_persistent_all_reduce_ref.cpp)
  set(_ref_variants default rocshmem rdc)
  foreach(_v IN LISTS _ref_variants)
    set(_tgt tile_example_gemm_universal_persistent_all_reduce_ref_${_v})
    add_executable(${_tgt} ${_ref_source})
    set_source_files_properties(${_ref_source} PROPERTIES LANGUAGE HIP)
    list(APPEND _ref_targets ${_tgt})
  endforeach()

  # Umbrella target: `make tile_example_gemm_universal_persistent_all_reduce_ref`
  # builds all three _ref variants in one go.
  add_custom_target(tile_example_gemm_universal_persistent_all_reduce_ref
    DEPENDS ${_ref_targets}
  )

  # RCCL (ROCm Collective Communications Library)
  # Prefer the ROCm-provided CMake package which defines the imported target `roc::rccl`.
  # Linking this target also propagates its include directories (so `<rccl/rccl.h>` is found)
  # and any required HIP host-side dependencies.
  find_package(rccl CONFIG REQUIRED)
  target_link_libraries(tile_example_gemm_universal_chuncked PRIVATE roc::rccl)
  target_link_libraries(tile_example_gemm_universal_persistent_all_reduce PRIVATE roc::rccl)
  foreach(_tgt IN LISTS _ref_targets)
    target_link_libraries(${_tgt} PRIVATE roc::rccl)
  endforeach()

  find_package(rocshmem REQUIRED PATHS)
  target_link_libraries(tile_example_gemm_universal_persistent_all_reduce PRIVATE roc::rocshmem)

  # Only the _rocshmem variant links rocshmem
  target_link_libraries(tile_example_gemm_universal_persistent_all_reduce_ref_rocshmem PRIVATE roc::rocshmem)

  # MPI is optional, but enables running `tile_example_gemm_universal_chuncked` across GPUs in a
  # single node via mpiexec (ncclUniqueId broadcast + ncclCommInitRank).
  find_package(MPI QUIET)
  if(MPI_CXX_FOUND)
    target_link_libraries(tile_example_gemm_universal_chuncked PRIVATE MPI::MPI_CXX)
    target_compile_definitions(tile_example_gemm_universal_chuncked PRIVATE CK_TILE_EXAMPLE_USE_MPI=1)

    target_link_libraries(tile_example_gemm_universal_persistent_all_reduce PRIVATE MPI::MPI_CXX)
    target_compile_definitions(tile_example_gemm_universal_persistent_all_reduce PRIVATE CK_TILE_EXAMPLE_USE_MPI=1)

    foreach(_tgt IN LISTS _ref_targets)
      target_link_libraries(${_tgt} PRIVATE MPI::MPI_CXX)
      target_compile_definitions(${_tgt} PRIVATE CK_TILE_EXAMPLE_USE_MPI=1)
    endforeach()

    message(STATUS "MPI found: tile_example_gemm_universal_chuncked, tile_example_gemm_universal_persistent_all_reduce & _ref variants will run with MPI support")
  else()
    message(STATUS "MPI not found: tile_example_gemm_universal_chuncked, tile_example_gemm_universal_persistent_all_reduce & _ref variants will run in single-process mode only")
  endif()

  # Keep consistent with other GEMM examples (helps avoid some compiler regressions).
  target_compile_options(tile_example_gemm_universal_minimal PRIVATE
    -mllvm -enable-noalias-to-md-conversion=0
  )
  target_compile_options(tile_example_gemm_universal_persistent PRIVATE
    -mllvm -enable-noalias-to-md-conversion=0
  )
  target_compile_options(tile_example_gemm_universal_persistent_all_reduce PRIVATE
    -mllvm -enable-noalias-to-md-conversion=0
  )
  # All _ref variants get the common noalias workaround.
  foreach(_tgt IN LISTS _ref_targets)
    target_compile_options(${_tgt} PRIVATE
      -mllvm -enable-noalias-to-md-conversion=0
    )
  endforeach()

  # Only the _rdc variant gets -fgpu-rdc (compile + link).
  target_compile_options(tile_example_gemm_universal_persistent_all_reduce_ref_rdc PRIVATE -fgpu-rdc)
  target_link_options(tile_example_gemm_universal_persistent_all_reduce_ref_rdc PRIVATE -fgpu-rdc)

  target_compile_options(tile_example_gemm_universal_chuncked PRIVATE
    -mllvm -enable-noalias-to-md-conversion=0
  )

  if(CK_USE_OCP_FP8)
    target_compile_definitions(tile_example_gemm_universal_minimal PRIVATE CK_TILE_USE_OCP_FP8)
    target_compile_definitions(tile_example_gemm_universal_persistent PRIVATE CK_TILE_USE_OCP_FP8)
    target_compile_definitions(tile_example_gemm_universal_persistent_all_reduce PRIVATE CK_TILE_USE_OCP_FP8)
    foreach(_tgt IN LISTS _ref_targets)
      target_compile_definitions(${_tgt} PRIVATE CK_TILE_USE_OCP_FP8)
    endforeach()
    target_compile_definitions(tile_example_gemm_universal_chuncked PRIVATE CK_TILE_USE_OCP_FP8)
  endif()
endif()


