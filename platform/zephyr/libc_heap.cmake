# SPDX-License-Identifier: Apache-2.0
# Route the C library's allocation entry points to platform/zephyr/libc_heap.c.
#
# An app opts in by calling the function below after find_package(Zephyr).
# The call sites are in Zephyr's libc glue and inside the C library, so the
# app cannot define the entry points itself: Zephyr's first link pass puts
# every Zephyr library under --whole-archive. The calls are redirected with
# the linker's --wrap instead (GNU ld and LLVM lld). Every entry point the
# C library's allocator has must be listed: a call that escapes the list
# would use Zephyr's allocator behind the pools' back.
function(entelea_libc_heap_enable)
  target_sources(app PRIVATE ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/libc_heap.c)
  foreach(entry malloc realloc free aligned_alloc)
    target_link_libraries(app PRIVATE -Wl,--wrap=${entry})
  endforeach()
endfunction()
