# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/tmp/claude-0/-home-claude/c0b49cca-c65c-5126-a29c-819947104f05/scratchpad/extsync/build_diag/_deps/picotool-src/enc_bootloader"
  "/tmp/claude-0/-home-claude/c0b49cca-c65c-5126-a29c-819947104f05/scratchpad/extsync/build_diag/_deps/picotool-build/enc_bootloader"
  "/tmp/claude-0/-home-claude/c0b49cca-c65c-5126-a29c-819947104f05/scratchpad/extsync/build_diag/_deps/picotool-build/enc_bootloader"
  "/tmp/claude-0/-home-claude/c0b49cca-c65c-5126-a29c-819947104f05/scratchpad/extsync/build_diag/_deps/picotool-build/enc_bootloader/tmp"
  "/tmp/claude-0/-home-claude/c0b49cca-c65c-5126-a29c-819947104f05/scratchpad/extsync/build_diag/_deps/picotool-build/enc_bootloader/src/enc_bootloader-stamp"
  "/tmp/claude-0/-home-claude/c0b49cca-c65c-5126-a29c-819947104f05/scratchpad/extsync/build_diag/_deps/picotool-build/enc_bootloader/src"
  "/tmp/claude-0/-home-claude/c0b49cca-c65c-5126-a29c-819947104f05/scratchpad/extsync/build_diag/_deps/picotool-build/enc_bootloader/src/enc_bootloader-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/tmp/claude-0/-home-claude/c0b49cca-c65c-5126-a29c-819947104f05/scratchpad/extsync/build_diag/_deps/picotool-build/enc_bootloader/src/enc_bootloader-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/tmp/claude-0/-home-claude/c0b49cca-c65c-5126-a29c-819947104f05/scratchpad/extsync/build_diag/_deps/picotool-build/enc_bootloader/src/enc_bootloader-stamp${cfgdir}") # cfgdir has leading slash
endif()
