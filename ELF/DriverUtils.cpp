//===- DriverUtils.cpp ----------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains utility functions for the driver. Because there
// are so many small functions, we created this separate file to make
// Driver.cpp less cluttered.
//
//===----------------------------------------------------------------------===//

#include "Driver.h"
#include "Error.h"
#include "Memory.h"
#include "lld/Config/Version.h"
#include "lld/Core/Reproduce.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/Triple.h"
#include "llvm/Option/Option.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"

using namespace llvm;
using namespace llvm::sys;

using namespace lld;
using namespace lld::elf;

// Create OptTable

// Create prefix string literals used in Options.td
#define PREFIX(NAME, VALUE) const char *const NAME[] = VALUE;
#include "Options.inc"
#undef PREFIX

// Create table mapping all options defined in Options.td
static const opt::OptTable::Info OptInfo[] = {
#define OPTION(X1, X2, ID, KIND, GROUP, ALIAS, X6, X7, X8, X9, X10)            \
  {X1, X2, X9,          X10,         OPT_##ID, opt::Option::KIND##Class,       \
   X8, X7, OPT_##GROUP, OPT_##ALIAS, X6},
#include "Options.inc"
#undef OPTION
};

ELFOptTable::ELFOptTable() : OptTable(OptInfo) {}

// Parse -color-diagnostics={auto,always,never} or -no-color-diagnostics.
static bool getColorDiagnostics(opt::InputArgList &Args) {
  bool Default = (ErrorOS == &errs() && Process::StandardErrHasColors());

  auto *Arg = Args.getLastArg(OPT_color_diagnostics, OPT_color_diagnostics_eq,
                              OPT_no_color_diagnostics);
  if (!Arg)
    return Default;
  if (Arg->getOption().getID() == OPT_color_diagnostics)
    return true;
  if (Arg->getOption().getID() == OPT_no_color_diagnostics)
    return false;

  StringRef S = Arg->getValue();
  if (S == "auto")
    return Default;
  if (S == "always")
    return true;
  if (S != "never")
    error("unknown option: -color-diagnostics=" + S);
  return false;
}

static cl::TokenizerCallback getQuotingStyle(opt::InputArgList &Args) {
  if (auto *Arg = Args.getLastArg(OPT_rsp_quoting)) {
    StringRef S = Arg->getValue();
    if (S != "windows" && S != "posix")
      error("invalid response file quoting: " + S);
    if (S == "windows")
      return cl::TokenizeWindowsCommandLine;
    return cl::TokenizeGNUCommandLine;
  }
  if (Triple(sys::getProcessTriple()).getOS() == Triple::Win32)
    return cl::TokenizeWindowsCommandLine;
  return cl::TokenizeGNUCommandLine;
}

// Parses a given list of options.
opt::InputArgList ELFOptTable::parse(ArrayRef<const char *> Argv) {
  // Make InputArgList from string vectors.
  unsigned MissingIndex;
  unsigned MissingCount;
  SmallVector<const char *, 256> Vec(Argv.data(), Argv.data() + Argv.size());

  // We need to get the quoting style for response files before parsing all
  // options so we parse here before and ignore all the options but
  // --rsp-quoting.
  opt::InputArgList Args = this->ParseArgs(Vec, MissingIndex, MissingCount);

  // Expand response files (arguments in the form of @<filename>)
  // and then parse the argument again.
  cl::ExpandResponseFiles(Saver, getQuotingStyle(Args), Vec);
  Args = this->ParseArgs(Vec, MissingIndex, MissingCount);

  // Interpret -color-diagnostics early so that error messages
  // for unknown flags are colored.
  Config->ColorDiagnostics = getColorDiagnostics(Args);
  if (MissingCount)
    error(Twine(Args.getArgString(MissingIndex)) + ": missing argument");

  for (auto *Arg : Args.filtered(OPT_UNKNOWN))
    error("unknown argument: " + Arg->getSpelling());
  return Args;
}

void elf::printHelp(const char *Argv0) {
  ELFOptTable Table;
  Table.PrintHelp(outs(), Argv0, "lld", false);
}

// Reconstructs command line arguments so that so that you can re-run
// the same command with the same inputs. This is for --reproduce.
std::string elf::createResponseFile(const opt::InputArgList &Args) {
  SmallString<0> Data;
  raw_svector_ostream OS(Data);

  // Copy the command line to the output while rewriting paths.
  for (auto *Arg : Args) {
    switch (Arg->getOption().getID()) {
    case OPT_reproduce:
      break;
    case OPT_INPUT:
      OS << quote(rewritePath(Arg->getValue())) << "\n";
      break;
    case OPT_L:
    case OPT_dynamic_list:
    case OPT_rpath:
    case OPT_alias_script_T:
    case OPT_script:
    case OPT_version_script:
      OS << Arg->getSpelling() << " " << quote(rewritePath(Arg->getValue()))
         << "\n";
      break;
    default:
      OS << toString(Arg) << "\n";
    }
  }
  return Data.str();
}

// Find a file by concatenating given paths. If a resulting path
// starts with "=", the character is replaced with a --sysroot value.
static Optional<std::string> findFile(StringRef Path1, const Twine &Path2) {
  SmallString<128> S;
  if (Path1.startswith("="))
    path::append(S, Config->Sysroot, Path1.substr(1), Path2);
  else
    path::append(S, Path1, Path2);

  if (fs::exists(S))
    return S.str().str();
  return None;
}

Optional<std::string> elf::findFromSearchPaths(StringRef Path) {
  for (StringRef Dir : Config->SearchPaths)
    if (Optional<std::string> S = findFile(Dir, Path))
      return S;
  return None;
}

// This is for -lfoo. We'll look for libfoo.so or libfoo.a from
// search paths.
Optional<std::string> elf::searchLibrary(StringRef Name) {
  if (Name.startswith(":"))
    return findFromSearchPaths(Name.substr(1));

  for (StringRef Dir : Config->SearchPaths) {
    if (!Config->Static)
      if (Optional<std::string> S = findFile(Dir, "lib" + Name + ".so"))
        return S;
    if (Optional<std::string> S = findFile(Dir, "lib" + Name + ".a"))
      return S;
  }
  return None;
}
