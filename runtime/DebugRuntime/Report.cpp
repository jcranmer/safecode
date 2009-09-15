//===- Report.cpp -------------------------------------------*- C++ -*-----===//
// 
//                       The SAFECode Compiler Project
//
// This file was developed by the LLVM research group and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
// 
//===----------------------------------------------------------------------===//
//
// This file implements functions for creating reports for the SAFECode
// run-time.
//
//===----------------------------------------------------------------------===//

#include "safecode/Runtime/Report.h"
#include "safecode/Config/config.h"

#include <iostream>
#include <cstdlib>

NAMESPACE_SC_BEGIN

ViolationInfo::~ViolationInfo() {}

void
ViolationInfo::print(std::ostream & OS) const {
  //
  // Print a single line report describing the error.  This is used, I believe,
  // by the automatic testing infrastructure scripts to determine if a safety
  // violation was correctly detected.
  //
  OS << std::showbase << std::hex 
     << "SAFECode:Violation Type " << this->type << " "
     << "when accessing  " << this->faultPtr << " "
     << "at IP=" << this->faultPC << "\n";

  //
  // Determine which descriptive string to use to describe the error.
  //
  const char * typestring;
  switch (type) {
    case FAULT_DANGLING_PTR:
      typestring = "Dangling Pointer Error";
      break;

    case FAULT_DOUBLE_FREE:
      typestring = "Double Free Error";
      break;

    case FAULT_OUT_OF_BOUNDS:
      typestring = "Out of Bounds Error";
      break;

    case FAULT_LOAD_STORE:
      typestring = "Load/Store Error";
      break;

    case FAULT_ALIGN:
      typestring = "Alignment Error";
      break;

    default:
      typestring = "Unknown Error";
      break;
  }

  //
  // Now print a more human readable version of the error.
  //
  OS << "\n";
  OS << "=======+++++++    SAFECODE RUNTIME ALERT +++++++=======\n";
  OS << "= Error type                            :\t" << typestring << "\n";
  OS << "= Program counter                       :\t" << this->faultPC << "\n";
  OS << "= Faulting pointer                      :\t" << this->faultPtr << "\n";
}

void
ReportMemoryViolation(const ViolationInfo *v) {
	v->print(std::cerr);
	abort();
}

NAMESPACE_SC_END
