# Linker information for the ASM_ARMASM language, so that Boost.Context can assemble its ARM64
# sources on Windows.
#
# CMake has no ASM_ARMASM dialect of its own -- its Microsoft ARM assembler language is called
# ASM_MARMASM. Boost 1.82 (the version openDAQ pins) therefore ships its own language definition
# for ASM_ARMASM in libs/context/cmake and calls enable_language(ASM_ARMASM) once the assembler
# is armasm/armasm64. Those modules cover compiler determination, testing and information.
#
# CMake 3.31 introduced a fourth, separately-loaded module per language,
# CMake<LANG>LinkerInformation.cmake, and unlike the other three it is not optional: a language
# without one aborts configuration with
#
#     CMake Error: Could not find cmake module file: CMakeASM_ARMASMLinkerInformation.cmake
#
# Boost 1.82 predates that requirement and so does not provide it. Supplying it here is enough,
# because the file is looked up on CMAKE_MODULE_PATH, to which the top-level CMakeLists.txt adds
# this directory. The body matches what CMake's own ASM dialects use -- see
# Modules/Internal/CMakeASM_MASMLinkerInformation.cmake in the CMake installation.
#
# This can be dropped once openDAQ moves to a Boost that knows about the 3.31 requirement.

set(ASM_DIALECT "_ARMASM")
include(Internal/CMakeASMLinkerInformation)
set(ASM_DIALECT)
