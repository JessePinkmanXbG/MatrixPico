# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/tmp/claude-0/-home-claude/c0b49cca-c65c-5126-a29c-819947104f05/scratchpad/extsync/build_diag/_deps/picotool-src"
  "/tmp/claude-0/-home-claude/c0b49cca-c65c-5126-a29c-819947104f05/scratchpad/extsync/build_diag/_deps/picotool-build"
  "/tmp/claude-0/-home-claude/c0b49cca-c65c-5126-a29c-819947104f05/scratchpad/extsync/build_diag/_deps/picotool-subbuild/picotool-populate-prefix"
  "/tmp/claude-0/-home-claude/c0b49cca-c65c-5126-a29c-819947104f05/scratchpad/extsync/build_diag/_deps/picotool-subbuild/picotool-populate-prefix/tmp"
  "/tmp/claude-0/-home-claude/c0b49cca-c65c-5126-a29c-819947104f05/scratchpad/extsync/build_diag/_deps/picotool-subbuild/picotool-populate-prefix/src/picotool-populate-stamp"
  "/tmp/claude-0/-home-claude/c0b49cca-c65c-5126-a29c-819947104f05/scratchpad/extsync/build_diag/_deps/picotool-subbuild/picotool-populate-prefix/src"
  "/tmp/claude-0/-home-claude/c0b49cca-c65c-5126-a29c-819947104f05/scratchpad/extsync/build_diag/_deps/picotool-subbuild/picotool-populate-prefix/src/picotool-populate-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/tmp/claude-0/-home-claude/c0b49cca-c65c-5126-a29c-819947104f05/scratchpad/extsync/build_diag/_deps/picotool-subbuild/picotool-populate-prefix/src/picotool-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/tmp/claude-0/-home-claude/c0b49cca-c65c-5126-a29c-819947104f05/scratchpad/extsync/build_diag/_deps/picotool-subbuild/picotool-populate-prefix/src/picotool-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
