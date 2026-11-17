# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/home/claude/pico-sdk/tools/pioasm"
  "/tmp/claude-0/-home-claude/c0b49cca-c65c-5126-a29c-819947104f05/scratchpad/extsync/build_diag/pioasm"
  "/tmp/claude-0/-home-claude/c0b49cca-c65c-5126-a29c-819947104f05/scratchpad/extsync/build_diag/pioasm-install"
  "/tmp/claude-0/-home-claude/c0b49cca-c65c-5126-a29c-819947104f05/scratchpad/extsync/build_diag/pioasm/tmp"
  "/tmp/claude-0/-home-claude/c0b49cca-c65c-5126-a29c-819947104f05/scratchpad/extsync/build_diag/pioasm/src/pioasmBuild-stamp"
  "/tmp/claude-0/-home-claude/c0b49cca-c65c-5126-a29c-819947104f05/scratchpad/extsync/build_diag/pioasm/src"
  "/tmp/claude-0/-home-claude/c0b49cca-c65c-5126-a29c-819947104f05/scratchpad/extsync/build_diag/pioasm/src/pioasmBuild-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/tmp/claude-0/-home-claude/c0b49cca-c65c-5126-a29c-819947104f05/scratchpad/extsync/build_diag/pioasm/src/pioasmBuild-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/tmp/claude-0/-home-claude/c0b49cca-c65c-5126-a29c-819947104f05/scratchpad/extsync/build_diag/pioasm/src/pioasmBuild-stamp${cfgdir}") # cfgdir has leading slash
endif()
