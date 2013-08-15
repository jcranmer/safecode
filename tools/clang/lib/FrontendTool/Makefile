##===- clang/lib/FrontendTool/Makefile ---------------------*- Makefile -*-===##
#
#                     The LLVM Compiler Infrastructure
#
# This file is distributed under the University of Illinois Open Source
# License. See LICENSE.TXT for details.
#
##===----------------------------------------------------------------------===##

CLANG_LEVEL := ../..
LIBRARYNAME := clangFrontendTool

include $(CLANG_LEVEL)/Makefile
#include $(CLANG_LEVEL)/../../Makefile.config

CXX.Flags += -DCLANG_ENABLE_ARCMT
CXX.Flags += -DCLANG_ENABLE_REWRITER
CXX.Flags += -DCLANG_ENABLE_STATIC_ANALYZER


