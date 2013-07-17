//===-- Main.cpp - LLVM-based Fortran Compiler ----------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// The Fortran Compiler.
//
//===----------------------------------------------------------------------===//

#include "flang/Frontend/TextDiagnosticPrinter.h"
#include "flang/Frontend/VerifyDiagnosticConsumer.h"
#include "flang/AST/ASTConsumer.h"
#include "flang/Frontend/ASTConsumers.h"
#include "flang/Parse/Parser.h"
#include "flang/Sema/Sema.h"
#include "flang/CodeGen/ModuleBuilder.h"
#include "flang/CodeGen/BackendUtil.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/ExecutionEngine/ExecutionEngine.h"
#include "llvm/PassManager.h"
#include "llvm/Analysis/Verifier.h"
#include "llvm/Analysis/Passes.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/Support/Host.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/FormattedStream.h"
#include "llvm/Bitcode/ReaderWriter.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/Timer.h"
#include "llvm/Support/system_error.h"
#include "llvm/ADT/OwningPtr.h"
using namespace llvm;
using namespace flang;

llvm::sys::Path GetExecutablePath(const char *Argv0, bool CanonicalPrefixes) {
  if (!CanonicalPrefixes)
    return llvm::sys::Path(Argv0);

  // This just needs to be some symbol in the binary; C++ doesn't
  // allow taking the address of ::main however.
  void *P = (void*) (intptr_t) GetExecutablePath;
  return llvm::sys::Path::GetMainExecutable(Argv0, P);
}

//===----------------------------------------------------------------------===//
// Command line options.
//===----------------------------------------------------------------------===//

namespace {

  cl::opt<std::string>
  InputFiles(cl::Positional, cl::desc("<input file>"), cl::init("-"));

  cl::list<std::string>
  IncludeDirs("I", cl::desc("Directory of include files"),
              cl::value_desc("directory"), cl::Prefix);

  cl::opt<bool>
  ReturnComments("C", cl::desc("Do not discard comments"), cl::init(false));

  cl::opt<bool>
  RunVerifier("verify", cl::desc("Run the verifier"), cl::init(false));

  cl::opt<bool>
  SyntaxOnly("fsyntax-only", cl::desc("Do not compile code"), cl::init(false));

  cl::opt<bool>
  PrintAST("ast-print", cl::desc("Prints AST"), cl::init(false));

  cl::opt<bool>
  DumpAST("ast-dump", cl::desc("Dumps AST"), cl::init(false));

  cl::opt<bool>
  EmitLLVM("emit-llvm", cl::desc("Emit llvm"), cl::init(false));

  cl::opt<bool>
  EmitASM("S", cl::desc("Emit assembly"), cl::init(false));

  cl::opt<std::string>
  OutputFile("o", cl::desc("<output file>"), cl::init(""));

  cl::opt<bool>
  EmitDebugInfo("g", cl::desc("Emit debugging info"), cl::init(false));

  cl::list<std::string>
  LinkDirectories("L", cl::desc("Additional directories for library files"));

  cl::list<std::string>
  LinkLibraries("l", cl::desc("Additional libraries"));

  cl::opt<bool>
  CompileOnly("c", cl::desc("compile only, do not link"), cl::init(false));

} // end anonymous namespace


std::string GetOutputName(StringRef Filename,
                          BackendAction Action) {
  llvm::SmallString<256> Path(Filename.begin(), Filename.end());
  switch(Action) {
  case Backend_EmitObj:
    llvm::sys::path::replace_extension(Path, ".o");
    break;
  case Backend_EmitAssembly:
    llvm::sys::path::replace_extension(Path, ".s");
    break;
  case Backend_EmitBC:
    llvm::sys::path::replace_extension(Path, ".bc");
    break;
  case Backend_EmitLL:
    llvm::sys::path::replace_extension(Path, ".ll");
    break;
  }
  return std::string(Path.begin(), Path.size());
}

static bool EmitFile(llvm::raw_ostream &Out,
                     llvm::Module *Module,
                     llvm::TargetMachine* TM,
                     BackendAction Action) {
  //write instructions to file
  if(Action == Backend_EmitObj || Action == Backend_EmitAssembly){
    llvm::Module &Mod = *Module;
    llvm::TargetMachine &Target = *TM;
    llvm::TargetMachine::CodeGenFileType FileType =
      Action == Backend_EmitObj ? llvm::TargetMachine::CGFT_ObjectFile :
                                  llvm::TargetMachine::CGFT_AssemblyFile;

    llvm::PassManager PM;

    Target.setAsmVerbosityDefault(true);
    Target.setMCRelaxAll(true);
    llvm::formatted_raw_ostream FOS(Out);

    // Ask the target to add backend passes as necessary.
    if (Target.addPassesToEmitFile(PM, FOS, FileType, true)) {
      return true;
    }

    PM.run(Mod);
  }
  else if(Action == Backend_EmitBC ){
    llvm::WriteBitcodeToFile(Module, Out);
  } else if(Action == Backend_EmitLL ) {
    Module->print(Out, nullptr);
  }
  return false;
}

static bool EmitOutputFile(const std::string &Input,
                           llvm::Module *Module,
                           llvm::TargetMachine* TM,
                           BackendAction Action) {
  std::string err;
  llvm::raw_fd_ostream Out(Input.c_str(), err, llvm::raw_fd_ostream::F_Binary);
  if (!err.empty()){
    llvm::errs() << "Could not open output file '" << Input << "': "
                 << err <<"\n";
    return true;
  }
  return EmitFile(Out, Module, TM, Action);
}

static bool LinkFiles(ArrayRef<std::string> OutputFiles) {
  const char *Driver = "gcc";
  std::string Cmd;
  llvm::raw_string_ostream OS(Cmd);
  OS << Driver;
  for(const std::string &I : OutputFiles)
    OS << " " << I;
  for(const std::string &I : LinkDirectories)
    OS << " -L " << I;
  for(const std::string &I : LinkLibraries)
    OS << " -l " << I;
  OS << " -l libflang";
  if(OutputFile.size())
    OS << " -o " << OutputFile;
  Cmd = OS.str();
  return system(Cmd.c_str());
}

static bool ParseFile(const std::string &Filename,
                      const std::vector<std::string> &IncludeDirs,
                      SmallVectorImpl<std::string> &OutputFiles) {
  llvm::OwningPtr<llvm::MemoryBuffer> MB;
  if (llvm::error_code ec = llvm::MemoryBuffer::getFileOrSTDIN(Filename.c_str(),
                                                               MB)) {
    llvm::errs() << "Could not open input file '" << Filename << "': " 
                 << ec.message() <<"\n";
    return true;
  }

  // Record the location of the include directory so that the lexer can find it
  // later.
  SourceMgr SrcMgr;
  SrcMgr.setIncludeDirs(IncludeDirs);

  // Tell SrcMgr about this buffer, which is what Parser will pick up.
  SrcMgr.AddNewSourceBuffer(MB.take(), llvm::SMLoc());

  LangOptions Opts;
  Opts.ReturnComments = ReturnComments;
  llvm::StringRef Ext = llvm::sys::path::extension(Filename);
  if(Ext.equals(".f") || Ext.equals(".F")) {
    Opts.FixedForm = 1;
    Opts.FreeForm = 0;
  }

  TextDiagnosticPrinter TDP(SrcMgr);
  DiagnosticsEngine Diag(new DiagnosticIDs,&SrcMgr, &TDP, false);
  // Chain in -verify checker, if requested.
  if(RunVerifier)
    Diag.setClient(new VerifyDiagnosticConsumer(Diag));

  ASTContext Context(SrcMgr, Opts);
  Sema SA(Context, Diag);
  Parser P(SrcMgr, Opts, Diag, SA);
  Diag.getClient()->BeginSourceFile(Opts, &P.getLexer());
  P.ParseProgramUnits();
  Diag.getClient()->EndSourceFile();

  // Dump
  if(PrintAST || DumpAST) {
    auto Dumper = CreateASTDumper("");
    Dumper->HandleTranslationUnit(Context);
    delete Dumper;
  }

  // Emit
  if(!SyntaxOnly && !Diag.hadErrors()) {
    auto CG = CreateLLVMCodeGen(Diag, Filename == ""? std::string("module") : Filename,
                                CodeGenOptions(), flang::TargetOptions(), llvm::getGlobalContext());
    CG->Initialize(Context);
    CG->HandleTranslationUnit(Context);

    BackendAction BA = Backend_EmitObj;
    if(EmitASM)   BA = Backend_EmitAssembly;
    if(EmitLLVM)  BA = Backend_EmitLL;

    llvm::Triple TheTriple(llvm::sys::getDefaultTargetTriple());
    const llvm::Target *TheTarget = 0;
    std::string Err;
    TheTarget = llvm::TargetRegistry::lookupTarget(TheTriple.str(), Err);
    auto TM = TheTarget->createTargetMachine(TheTriple.getTriple(), "", "", llvm::TargetOptions());
    if(OutputFile == "-")
      EmitFile(llvm::outs(), CG->GetModule(), TM, BA);
    else {
      OutputFiles.push_back(GetOutputName(Filename, BA));
      EmitOutputFile(OutputFiles.back(), CG->GetModule(), TM, BA);
    }
    delete CG;
  }

  return Diag.hadErrors();
}

int main(int argc, char **argv) {
  sys::PrintStackTraceOnErrorSignal();
  PrettyStackTraceProgram X(argc, argv);
  cl::ParseCommandLineOptions(argc, argv, "LLVM Fortran compiler");

  bool CanonicalPrefixes = true;
  for (int i = 1; i < argc; ++i)
    if (llvm::StringRef(argv[i]) == "-no-canonical-prefixes") {
      CanonicalPrefixes = false;
      break;
    }

  llvm::sys::Path Path = GetExecutablePath(argv[0], CanonicalPrefixes);

  llvm::InitializeAllTargets();
  llvm::InitializeAllTargetMCs();
  llvm::InitializeAllAsmPrinters();
  llvm::InitializeAllAsmParsers();

  // Parse the input file.
  bool HadErrors = false;
  SmallVector <std::string, 32> OutputFiles;
  OutputFiles.reserve(1);

  if(ParseFile(InputFiles, IncludeDirs, OutputFiles)) {
    HadErrors = true;
  }
  if(!HadErrors && !CompileOnly && !EmitLLVM && !EmitASM)
    LinkFiles(OutputFiles);

  // If any timers were active but haven't been destroyed yet, print their
  // results now. This happens in -disable-free mode.
  llvm::TimerGroup::printAll(llvm::errs());

  llvm::llvm_shutdown();
  return HadErrors;
}
