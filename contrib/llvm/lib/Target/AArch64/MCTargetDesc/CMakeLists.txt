add_llvm_library(LLVMAArch64Desc
  AArch64AsmBackend.cpp
  AArch64ELFObjectWriter.cpp
  AArch64ELFStreamer.cpp
  AArch64MCAsmInfo.cpp
  AArch64MCCodeEmitter.cpp
  AArch64MCExpr.cpp
  AArch64MCTargetDesc.cpp
  )
add_dependencies(LLVMAArch64Desc AArch64CommonTableGen)

# Hack: we need to include 'main' target directory to grab private headers
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/.. ${CMAKE_CURRENT_BINARY_DIR}/..)
