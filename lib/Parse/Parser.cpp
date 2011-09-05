//===-- Parser.cpp - Fortran Parser Interface -----------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// The Fortran parser interface.
//
//===----------------------------------------------------------------------===//

#include "flang/Parse/Parser.h"
#include "flang/AST/Decl.h"
#include "flang/AST/Expr.h"
#include "flang/AST/Stmt.h"
#include "flang/Basic/DeclSpec.h"
#include "flang/Basic/TokenKinds.h"
#include "flang/Sema/Sema.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallString.h"
using namespace flang;

static void VectorToString(const llvm::SmallVectorImpl<llvm::StringRef> &Vec,
                           llvm::SmallVectorImpl<char> &Str) {
  llvm::raw_svector_ostream OS(Str);
  for (llvm::SmallVectorImpl<llvm::StringRef>::const_iterator
         I = Vec.begin(), E = Vec.end(); I != E; ++I)
    OS << *I;
}

/// print - If a crash happens while the parser is active, print out a line
/// indicating what the current token is.
void PrettyStackTraceParserEntry::print(llvm::raw_ostream &OS) const {
  const Token &Tok = FP.getCurToken();
  if (Tok.is(tok::eof)) {
    OS << "<eof> parser at end of file\n";
    return;
  }

  if (!Tok.getLocation().isValid()) {
    OS << "<unknown> parser at unknown location\n";
    return;
  }

  llvm::SmallVector<llvm::StringRef, 2> Spelling;
  FP.getLexer().getSpelling(Tok, Spelling);
  llvm::SmallString<256> Name;
  VectorToString(Spelling, Name);
  FP.getLexer().getSourceManager()
    .PrintMessage(Tok.getLocation(),
                  "current parser token '" + Name.str() + "'", "error");
}

//===----------------------------------------------------------------------===//
//                            Fortran Parsing
//===----------------------------------------------------------------------===//

Parser::Parser(llvm::SourceMgr &SM, const LangOptions &Opts, Diagnostic  &D,
               Sema &actions)
  : TheLexer(SM, Opts, D), Features(Opts), CrashInfo(*this), SrcMgr(SM),
    CurBuffer(0), Context(actions.Context), Diag(D), Actions(actions),
    Identifiers(Opts) {
  getLexer().setBuffer(SrcMgr.getMemoryBuffer(CurBuffer));
  Tok.startToken();
  NextTok.startToken();
}

bool Parser::EnterIncludeFile(const std::string &Filename) {
  std::string IncludedFile;
  int NewBuf = SrcMgr.AddIncludeFile(Filename, getLexer().getLoc(),
                                     IncludedFile);
  if (NewBuf == -1)
    return true;

  CurBuffer = NewBuf;
  getLexer().setBuffer(SrcMgr.getMemoryBuffer(CurBuffer));
  return false;
}

/// Lex - Get the next token.
void Parser::Lex() {
  if (NextTok.isNot(tok::unknown)) {
    Tok = NextTok;
  } else {
    TheLexer.Lex(Tok);
    ClassifyToken(Tok);
  }

  if (Tok.is(tok::eof)) return;

  TheLexer.Lex(NextTok);
  ClassifyToken(NextTok);

#define MERGE_TOKENS(A, B)                      \
  if (NextTok.is(tok::kw_ ## B)) {              \
    Tok.setKind(tok::kw_ ## A ## B);            \
    break;                                      \
  }                                             \

  // [3.3.1]p4
  switch (Tok.getKind()) {
  default: return;
  case tok::kw_BLOCK:
    MERGE_TOKENS(BLOCK, DATA);
    return;
  case tok::kw_ELSE:
    MERGE_TOKENS(ELSE, IF);
    MERGE_TOKENS(ELSE, WHERE);
    return;
  case tok::kw_END:
    MERGE_TOKENS(END, IF);
    MERGE_TOKENS(END, DO);
    MERGE_TOKENS(END, FUNCTION);
    MERGE_TOKENS(END, FORALL);
    MERGE_TOKENS(END, WHERE);
    MERGE_TOKENS(END, ENUM);
    MERGE_TOKENS(END, SELECT);
    MERGE_TOKENS(END, TYPE);
    MERGE_TOKENS(END, MODULE);
    MERGE_TOKENS(END, PROGRAM);
    MERGE_TOKENS(END, ASSOCIATE);
    MERGE_TOKENS(END, FILE);
    MERGE_TOKENS(END, INTERFACE);
    MERGE_TOKENS(END, BLOCKDATA);

    if (NextTok.is(tok::kw_BLOCK)) {
      Tok = NextTok;
      TheLexer.Lex(NextTok);
      ClassifyToken(NextTok);

      if (!NextTok.is(tok::kw_DATA)) {
        Diag.ReportError(NextTok.getLocation(),
                         "expected 'DATA' after 'BLOCK' keyword");
        return;
      }

      Tok.setKind(tok::kw_ENDBLOCKDATA);
      break;
    }

    return;
  case tok::kw_ENDBLOCK:
    MERGE_TOKENS(ENDBLOCK, DATA);
    return;
  case tok::kw_GO:
    MERGE_TOKENS(GO, TO);
    return;
  case tok::kw_SELECT:
    MERGE_TOKENS(SELECT, CASE);
    MERGE_TOKENS(SELECT, TYPE);
    return;
  case tok::kw_IN:
    MERGE_TOKENS(IN, OUT);
    return;
  case tok::kw_DOUBLE:
    MERGE_TOKENS(DOUBLE, PRECISION);
    return;
  }

  if (NextTok.is(tok::eof)) return;

  TheLexer.Lex(NextTok);
  ClassifyToken(NextTok);
}

void Parser::ClassifyToken(Token &T) {
  if (T.isNot(tok::identifier))
    return;

  // Set the identifier info for this token.
  llvm::SmallVector<llvm::StringRef, 2> Spelling;
  TheLexer.getSpelling(T, Spelling);
  llvm::SmallString<256> Name;
  VectorToString(Spelling, Name);
  std::string NameStr = Name.str();

  // We assume that the "common case" is that if an identifier is also a
  // keyword, it will most likely be used as a keyword. I.e., most programs are
  // sane, and won't use keywords for variable names. We mark it as a keyword
  // for ease in parsing. But it's weak and can change into an identifier or
  // builtin depending upon the context.
  if (IdentifierInfo *KW = Identifiers.lookupKeyword(NameStr)) {
    T.setIdentifierInfo(KW);
    T.setKind(KW->getTokenID());
  } else if (IdentifierInfo *BI = Identifiers.lookupBuiltin(NameStr)) {
    T.setIdentifierInfo(BI);
    T.setKind(BI->getTokenID());
  } else {
    IdentifierInfo *II = getIdentifierInfo(NameStr);
    T.setIdentifierInfo(II);
    T.setKind(II->getTokenID());
  }
}

/// EatIfPresent - Eat the token if it's present. Return 'true' if it was
/// delicious.
bool Parser::EatIfPresent(tok::TokenKind Kind) {
  if (Tok.is(Kind)) {
    Lex();
    return true;
  }

  return false;
}

/// LexToEndOfStatement - Lext to the end of a statement. Done in an
/// unrecoverable error situation.
void Parser::LexToEndOfStatement() {
  // Eat the rest of the line.
  while (!Tok.isAtStartOfStatement())
    Lex();
}

/// ParseStatementLabel - Parse the statement label token. If the current token
/// isn't a statement label, then set the StmtLabelTok's kind to "unknown".
void Parser::ParseStatementLabel() {
  if (Tok.isNot(tok::statement_label)) {
    StmtLabelTok.setKind(tok::unknown);
    return;
  }
  StmtLabelTok = Tok;
  Lex();
}

// Assumed syntax rules
//
//   R101 xyz-list        :=  xyz [, xyz] ...
//   R102 xyz-name        :=  name
//   R103 scalar-xyz      :=  xyz
//
//   C101 (R103) scalar-xyz shall be scalar.

/// ParseProgramUnits - Main entry point to the parser. Parses the current
/// source.
bool Parser::ParseProgramUnits() {
  Actions.ActOnTranslationUnit();

  // Prime the lexer.
  Lex();
  Tok.setFlag(Token::StartOfStatement);

  while (!ParseProgramUnit())
    /* Parse them all */;

  return Diag.hadErrors() || Diag.hadWarnings();
}

/// ParseProgramUnit - Parse a program unit.
///
///   R202:
///     program-unit :=
///         main-program
///      or external-subprogram
///      or module
///      or block-data
bool Parser::ParseProgramUnit() {
  if (Tok.is(tok::eof))
    return true;

  ParseStatementLabel();

  // FIXME: These calls should return something proper.
  switch (Tok.getKind()) {
  default:
    ParseMainProgram();
    break;

  case tok::kw_FUNCTION:
  case tok::kw_SUBPROGRAM:
    ParseExternalSubprogram();
    break;

  case tok::kw_MODULE:
    ParseModule();
    break;

  case tok::kw_BLOCKDATA:
    ParseBlockData();
    break;
  }

  return false;
}

/// ParseMainProgram - Parse the main program.
///
///   R1101:
///     main-program :=
///         [program-stmt]
///           [specification-part]
///           [execution-part]
///           [internal-subprogram-part]
///           end-program-stmt
bool Parser::ParseMainProgram() {
  // If the PROGRAM statement didn't have an identifier, pretend like it did for
  // the time being.
  StmtResult ProgStmt;
  if (Tok.is(tok::kw_PROGRAM)) {
    ProgStmt = ParsePROGRAMStmt();
    ParseStatementLabel();
  }

  // If the PROGRAM statement has an identifier, create a DeclarationNameInfo
  // object for the main-program action.
  const IdentifierInfo *IDInfo = 0;
  llvm::SMLoc NameLoc;
  if (ProgStmt.isUsable()) {
    ProgramStmt *PS = ProgStmt.takeAs<ProgramStmt>();
    IDInfo = PS->getProgramName();
    NameLoc = PS->getNameLocation();
  }

  DeclarationName DN(IDInfo);
  DeclarationNameInfo DNI(DN, NameLoc);
  Actions.ActOnMainProgram(DNI);

  if (Tok.isNot(tok::kw_END) && Tok.isNot(tok::kw_ENDPROGRAM)) {
    ParseSpecificationPart();
    ParseStatementLabel();
  }

  if (Tok.isNot(tok::kw_END) && Tok.isNot(tok::kw_ENDPROGRAM)) {
    ParseExecutionPart();
    ParseStatementLabel();
  }

  StmtResult EndProgStmt = ParseEND_PROGRAMStmt();

  Actions.ActOnEndProgramUnit();
  return false;
}

/// ParseSpecificationPart - Parse the specification part.
///
///   R204:
///     specification-part :=
///        [use-stmt] ...
///          [import-stmt] ...
///          [implicit-part] ...
///          [declaration-construct] ...
bool Parser::ParseSpecificationPart() {
  bool HasErrors = false;
  while (Tok.is(tok::kw_USE)) {
    StmtResult S = ParseUSEStmt();
    if (S.isInvalid()) {
      LexToEndOfStatement();
      HasErrors = true;
    }

    ParseStatementLabel();
  }

  while (Tok.is(tok::kw_IMPORT)) {
    StmtResult S = ParseIMPORTStmt();
    if (S.isInvalid()) {
      LexToEndOfStatement();
      HasErrors = true;
    }

    ParseStatementLabel();
  }

  while (Tok.is(tok::kw_IMPLICIT)) {
    if (ParseImplicitPartList()) {
      LexToEndOfStatement();
      HasErrors = true;
    }

    ParseStatementLabel();
  }

  if (ParseDeclarationConstructList()) {
    LexToEndOfStatement();
    HasErrors = true;
  }

  return HasErrors;
}

/// ParseExternalSubprogram - Parse an external subprogram.
///
///   R203:
///     external-subprogram :=
///         function-subprogram
///      or subroutine-subprogram
bool Parser::ParseExternalSubprogram() {
  return false;
}

/// ParseFunctionSubprogram - Parse a function subprogram.
///
///   R1223:
///     function-subprogram :=
///         function-stmt
///           [specification-part]
///           [execution-part]
///           [internal-subprogram-part]
///           end-function-stmt
bool Parser::ParseFunctionSubprogram() {
  return false;
}

/// ParseSubroutineSubprogram - Parse a subroutine subprogram.
///
///   R1231:
///     subroutine-subprogram :=
///         subroutine-stmt
///           [specification-part]
///           [execution-part]
///           [internal-subprogram-part]
///           end-subroutine-stmt
bool Parser::ParseSubroutineSubprogram() {
  return false;
}

/// ParseModule - Parse a module.
///
///   R1104:
///     module :=
///         module-stmt
///           [specification-part]
///           [module-subprogram-part]
///           end-module-stmt
bool Parser::ParseModule() {
  return false;
}

/// ParseBlockData - Parse block data.
///
///   R1116:
///     block-data :=
///         block-data-stmt
///           [specification-part]
///           end-block-data-stmt
bool Parser::ParseBlockData() {
  if (Tok.isNot(tok::kw_BLOCKDATA))
    return true;

  return false;
}

/// ParseImplicitPartList - Parse a (possibly empty) list of implicit part
/// statements.
bool Parser::ParseImplicitPartList() {
  return false;
}

/// ParseImplicitPart - Parse the implicit part.
///
///   R205:
///     implicit-part :=
///         [implicit-part-stmt] ...
///           implicit-stmt
bool Parser::ParseImplicitPart() {
  // R206:
  //   implicit-part-stmt :=
  //       implicit-stmt
  //    or parameter-stmt
  //    or format-stmt
  //    or entry-stmt
  return false;
}

/// ParseExecutionPart - Parse the execution part.
///
///   R208:
///     execution-part :=
///         executable-construct
///           [ execution-part-construct ] ...
bool Parser::ParseExecutionPart() {
  ParseExecutableConstruct();
  return false;
}

/// ParseDeclarationConstructList - Parse a (possibly empty) list of declaration
/// construct statements.
bool Parser::ParseDeclarationConstructList() {
  while (!ParseDeclarationConstruct()) // FIXME: Make into a list.
    /* Parse them all */ ;

  return false;
}

/// ParseDeclarationConstruct - Parse a declaration construct.
///
///   [2.1] R207:
///     declaration-construct :=
///         derived-type-def
///      or entry-stmt
///      or enum-def
///      or format-stmt
///      or interface-block
///      or parameter-stmt
///      or procedure-declaration-stmt
///      or specification-stmt
///      or type-declaration-stmt
///      or stmt-function-stmt
bool Parser::ParseDeclarationConstruct() {
  ParseStatementLabel();

  switch (Tok.getKind()) {
  default:
    return true;
  case tok::kw_INTEGER:
  case tok::kw_REAL:
  case tok::kw_COMPLEX:
  case tok::kw_CHARACTER:
  case tok::kw_LOGICAL:
  case tok::kw_DOUBLEPRECISION:
  case tok::kw_TYPE:
  case tok::kw_CLASS: {
    if (ParseTypeDeclarationStmt()) {
      LexToEndOfStatement();
      // FIXME:
    }
    break;
  }
    // FIXME: And the rest?
  }

  return false;
}

/// ParseForAllConstruct - Parse a forall construct.
///
///   [7.4.4.1] R752:
///     forall-construct :=
///         forall-construct-stmt
///           [forall-body-construct] ...
///           end-forall-stmt
bool Parser::ParseForAllConstruct() {
  return false;
}

/// ParseArraySpec - Parse an array specification.
///
///   [5.1.2.5] R510:
///     array-spec :=
///         explicit-shape-spec-list
///      or assumed-shape-spec-list
///      or deferred-shape-spec-list
///      or assumed-size-spec
bool Parser::ParseArraySpec(llvm::SmallVectorImpl<ExprResult> &Dims) {
  if (!EatIfPresent(tok::l_paren))
    return Diag.ReportError(Tok.getLocation(),
                            "expected '(' in array spec");

  // [5.1.2.5.1] R511, R512, R513
  //   explicit-shape-spec :=
  //       [ lower-bound : ] upper-bound
  //   lower-bound :=
  //       specification-expr
  //   upper-bound :=
  //       specification-expr
  //
  // [7.1.6] R729
  //   specification-expr :=
  //       scalar-int-expr
  //
  // [7.1.4] R727
  //   int-expr :=
  //       expr
  //
  //   C708: int-expr shall be of type integer.

  do {
    ExprResult E = ParseExpression();
    if (E.isInvalid()) return true;;
    Dims.push_back(E);
  } while (EatIfPresent(tok::comma));

  if (!EatIfPresent(tok::r_paren))
    return Diag.ReportError(Tok.getLocation(),
                            "expected ')' in array spec");

  return Actions.ActOnArraySpec(); // FIXME: Use the dims.
}

/// ParsePROGRAMStmt - If there is a PROGRAM statement, parse it.
/// 
///   [11.1] R1102:
///     program-stmt :=
///         PROGRAM program-name
Parser::StmtResult Parser::ParsePROGRAMStmt() {
  // Check to see if we start with a 'PROGRAM' statement.
  const IdentifierInfo *IDInfo = Tok.getIdentifierInfo();
  llvm::SMLoc ProgramLoc = Tok.getLocation();
  if (!isaKeyword(IDInfo->getName()) || Tok.isNot(tok::kw_PROGRAM))
    return Actions.ActOnPROGRAM(Context, 0, ProgramLoc, llvm::SMLoc(),
                                StmtLabelTok);

  // Parse the program name.
  Lex();
  if (Tok.isNot(tok::identifier) || Tok.isAtStartOfStatement()) {
    Diag.ReportError(ProgramLoc,
                     "'PROGRAM' keyword expects an identifier");
    return StmtResult();
  }

  llvm::SMLoc NameLoc = Tok.getLocation();
  IDInfo = Tok.getIdentifierInfo();
  Lex(); // Eat program name.
  return Actions.ActOnPROGRAM(Context, IDInfo, ProgramLoc, NameLoc,
                              StmtLabelTok);
}

/// ParseUSEStmt - Parse the 'USE' statement.
///
///   [11.2.2] R1109:
///     use-stmt :=
///         USE [ [ , module-nature ] :: ] module-name [ , rename-list ]
///      or USE [ [ , module-nature ] :: ] module-name , ONLY : [ only-list ]
Parser::StmtResult Parser::ParseUSEStmt() {
  Lex();

  // module-nature :=
  //     INTRINSIC
  //  or NON INTRINSIC
  UseStmt::ModuleNature MN = UseStmt::None;
  if (EatIfPresent(tok::comma)) {
    if (EatIfPresent(tok::kw_INTRINSIC)) {
      MN = UseStmt::Intrinsic;
    } else if (EatIfPresent(tok::kw_NONINTRINSIC)) {
      MN = UseStmt::NonIntrinsic;
    } else {
      Diag.ReportError(Tok.getLocation(),
                       "expected module nature keyword");
      return StmtResult();
    }

    if (!EatIfPresent(tok::coloncolon)) {
      Diag.ReportError(Tok.getLocation(),
                       "expected a '::' after the module nature");
      return StmtResult();
    }
  }

  EatIfPresent(tok::coloncolon);

  if (Tok.isNot(tok::identifier)) {
    Diag.ReportError(Tok.getLocation(),
                     "missing module name in USE statement");
    return StmtResult();
  }

  llvm::StringRef Name = Tok.getIdentifierInfo()->getName();
  Lex();

  if (!EatIfPresent(tok::comma)) {
    if (!Tok.isAtStartOfStatement()) {
      Diag.ReportError(Tok.getLocation(),
                       "expected a ',' in USE statement");
      return StmtResult();
    }

    return StmtResult();        // FIXME:
  }

  bool OnlyUse = false;
  llvm::SMLoc OnlyLoc = Tok.getLocation();
  IdentifierInfo *UseListFirstVar = 0;
  if (Tok.is(tok::kw_ONLY)) {
    UseListFirstVar = Tok.getIdentifierInfo();
    Lex(); // Eat 'ONLY'
    if (!EatIfPresent(tok::colon)) {
      if (Tok.isNot(tok::equalgreater)) {
        Diag.ReportError(Tok.getLocation(),
                         "expected a ':' after the ONLY keyword");
        return StmtResult();
      }

      OnlyUse = false;
    } else {
      OnlyUse = true;
    }
  }

  llvm::SmallVector<const VarDecl*, 8> LocalNames;
  llvm::SmallVector<const VarDecl*, 8> UseNames;

  if (!OnlyUse && Tok.is(tok::equalgreater)) {
    // They're using 'ONLY' as a non-keyword and renaming it.
    Lex(); // Eat '=>'
    if (Tok.isAtStartOfStatement() || Tok.isNot(tok::identifier)) {
      Diag.ReportError(Tok.getLocation(),
                       "missing rename of variable in USE statement");
      return StmtResult();
    }

    // FIXME:
    LocalNames.push_back(Context.getOrCreateVarDecl(llvm::SMLoc(), 0,
                                                    UseListFirstVar));
    UseNames.push_back(Context.getOrCreateVarDecl(llvm::SMLoc(), 0,
                                                  Tok.getIdentifierInfo()));
    Lex();
    EatIfPresent(tok::comma);
  }

  while (!Tok.isAtStartOfStatement() && Tok.is(tok::identifier)) {
    LocalNames.push_back(Context.getOrCreateVarDecl(llvm::SMLoc(), 0,
                                                    Tok.getIdentifierInfo()));
    Lex();

    if (OnlyUse) {
      if (Tok.is(tok::equalgreater)) {
        Diag.ReportError(Tok.getLocation(),
                         "performing a rename in an 'ONLY' list");
        return StmtResult();
      }

      if (!EatIfPresent(tok::comma))
        break;
      continue;
    }

    if (!EatIfPresent(tok::equalgreater)) {
      Diag.ReportError(Tok.getLocation(),
                       "expected a '=>' in the rename list");
      return StmtResult();
    }

    // FIXME: Check for identifier kind.
    llvm::SMLoc UseNameLoc = Tok.getLocation();
    // FIXME:
    UseNames.push_back(Context.getOrCreateVarDecl(UseNameLoc, 0,
                                                  Tok.getIdentifierInfo()));
    Lex();

    if (!EatIfPresent(tok::comma))
      break;
  }

  assert((UseNames.empty() || LocalNames.size() == UseNames.size()) &&
         "Unbalanced number of renames with USE ONLY names!");
  return Actions.ActOnUSE(MN, Name, OnlyUse, LocalNames, UseNames,
                          StmtLabelTok);
}

/// ParseIMPORTStmt - Parse the IMPORT statement.
///
///   [12.4.3.3] R1209:
///     import-stmt :=
///         IMPORT [[::] import-name-list]
Parser::StmtResult Parser::ParseIMPORTStmt() {
  Lex();
  EatIfPresent(tok::coloncolon);

  llvm::SmallVector<IdentifierInfo*, 4> ImportNameList;
  while (!Tok.isAtStartOfStatement() && Tok.is(tok::identifier)) {
    ImportNameList.push_back(Tok.getIdentifierInfo());
    Lex();
    EatIfPresent(tok::comma);
  }

  return Actions.ActOnIMPORT(ImportNameList, StmtLabelTok);
}

/// ParseIMPLICITStmt - Parse the IMPLICIT statement.
///
///   [5.3] R549:
///     implicit-stmt :=
///         IMPLICIT implicit-spec-list
///      or IMPLICIT NONE
Parser::StmtResult Parser::ParseIMPLICITStmt() {
  Lex();

  if (Tok.is(tok::kw_NONE))
    return Actions.ActOnIMPLICIT(StmtLabelTok);
  
  return StmtResult();
}

/// ParsePARAMETERStmt - Parse the PARAMETER statement.
///
///   [5.4.11] R548:
///     parameter-stmt :=
///         PARAMETER ( named-constant-def-list )
Parser::StmtResult Parser::ParsePARAMETERStmt() {
  Lex();
  if (!EatIfPresent(tok::l_paren)) {
    Diag.ReportError(Tok.getLocation(),
                     "expected '(' in PARAMETER statement");
    return StmtResult();
  }

  llvm::SmallVector<const IdentifierInfo *, 4> NamedConsts;
  llvm::SmallVector<ExprResult, 4> ConstExprs;
  while (Tok.is(tok::identifier)) {
    NamedConsts.push_back(Tok.getIdentifierInfo());
    Lex();

    if (!EatIfPresent(tok::equal)) {
      Diag.ReportError(Tok.getLocation(),
                       "expected '=' in PARAMETER statement");
      goto error;
    }

    ExprResult ConstExpr = ParseExpression();
    if (ConstExpr.isInvalid())
      goto error;
    ConstExprs.push_back(ConstExpr);

    EatIfPresent(tok::comma);
  }

  if (!EatIfPresent(tok::r_paren)) {
    Diag.ReportError(Tok.getLocation(),
                     "expected ')' in PARAMETER statement");
    goto error;
  }

  return Actions.ActOnPARAMETER(NamedConsts, ConstExprs, StmtLabelTok);

 error:
  // Clean up any expressions we may have created before the error.
  for (llvm::SmallVectorImpl<ExprResult>::iterator
         I = ConstExprs.begin(), E = ConstExprs.end(); I != E; ++I)
    delete I->take();
  
  return StmtResult();
}

/// ParseProcedureDeclStmt - Parse the procedure declaration statement.
///
///   [12.3.2.3] R1211:
///     procedure-declaration-stmt :=
///         PROCEDURE ([proc-interface]) [ [ , proc-attr-spec ]... :: ] #
///         # proc-decl-list
bool Parser::ParseProcedureDeclStmt() {
  return false;
}

/// ParseSpecificationStmt - Parse the specification statement.
///
///   [2.1] R212:
///     specification-stmt :=
///         access-stmt
///      or allocatable-stmt
///      or asynchronous-stmt
///      or bind-stmt
///      or common-stmt
///      or data-stmt
///      or dimension-stmt
///      or equivalence-stmt
///      or external-stmt
///      or intent-stmt
///      or intrinsic-stmt
///      or namelist-stmt
///      or optional-stmt
///      or pointer-stmt
///      or protected-stmt
///      or save-stmt
///      or target-stmt
///      or value-stmt
///      or volatile-stmt
bool Parser::ParseSpecificationStmt() {
  StmtResult Result;
  switch (Tok.getKind()) {
  default: break;
  case tok::kw_ASYNCHRONOUS:
    Result = ParseASYNCHRONOUSStmt();
    return true;
  }

  return false;
}

/// ParseACCESSStmt - Parse the ACCESS statement.
///
///   [5.2.1] R518:
///     access-stmt :=
///         access-spec [[::] access-id-list]
Parser::StmtResult Parser::ParseACCESSStmt() {
  return StmtResult();
}

/// ParseALLOCATABLEStmt - Parse the ALLOCATABLE statement.
///
///   [5.2.2] R520:
///     allocatable-stmt :=
///         ALLOCATABLE [::] object-name       #
///         # [ ( deferred-shape-spec-list ) ] #
///         # [ , object-name [ ( deferred-sape-spec-list ) ] ] ...
Parser::StmtResult Parser::ParseALLOCATABLEStmt() {
  return StmtResult();
}

/// ParseASYNCHRONOUSStmt - Parse the ASYNCHRONOUS statement.
///
///   [5.4.3] R528:
///     asynchronous-stmt :=
///         ASYNCHRONOUS [::] object-name-list
Parser::StmtResult Parser::ParseASYNCHRONOUSStmt() {
  Lex();
  EatIfPresent(tok::coloncolon);

  llvm::SmallVector<const IdentifierInfo*, 8> ObjNameList;
  while (!Tok.isAtStartOfStatement() && Tok.is(tok::identifier)) {
    ObjNameList.push_back(Tok.getIdentifierInfo());
    Lex();
    EatIfPresent(tok::comma);
  }

  return Actions.ActOnASYNCHRONOUS(ObjNameList, StmtLabelTok);
}

/// ParseBINDStmt - Parse the BIND statement.
///
///   [5.2.4] R522:
///     bind-stmt :=
///         language-binding-spec [::] bind-entity-list
Parser::StmtResult Parser::ParseBINDStmt() {
  return StmtResult();
}

/// ParseCOMMONStmt - Parse the COMMON statement.
///
///   [5.5.2] R557:
///     common-stmt :=
///         COMMON #
///         # [ / [common-block-name] / ] common-block-object-list #
///         # [ [,] / [common-block-name / #
///         #   common-block-object-list ] ...
Parser::StmtResult Parser::ParseCOMMONStmt() {
  return StmtResult();
}

/// ParseDATAStmt - Parse the DATA statement.
///
///   [5.2.5] R524:
///     data-stmt :=
///         DATA data-stmt-set [ [,] data-stmt-set ] ...
Parser::StmtResult Parser::ParseDATAStmt() {
  return StmtResult();
}

/// ParseDIMENSIONStmt - Parse the DIMENSION statement.
///
///   [5.2.6] R535:
///     dimension-stmt :=
///         DIMENSION [::] array-name ( array-spec ) #
///         # [ , array-name ( array-spec ) ] ...
Parser::StmtResult Parser::ParseDIMENSIONStmt() {
  return StmtResult();
}

/// ParseEQUIVALENCEStmt - Parse the EQUIVALENCE statement.
///
///   [5.5.1] R554:
///     equivalence-stmt :=
///         EQUIVALENCE equivalence-set-list
Parser::StmtResult Parser::ParseEQUIVALENCEStmt() {
  return StmtResult();
}

/// ParseEXTERNALStmt - Parse the EXTERNAL statement.
///
///   [12.3.2.2] R1210:
///     external-stmt :=
///         EXTERNAL [::] external-name-list
Parser::StmtResult Parser::ParseEXTERNALStmt() {
  return StmtResult();
}

/// ParseINTENTStmt - Parse the INTENT statement.
///
///   [5.2.7] R536:
///     intent-stmt :=
///         INTENT ( intent-spec ) [::] dummy-arg-name-list
Parser::StmtResult Parser::ParseINTENTStmt() {
  return StmtResult();
}

/// ParseINTRINSICStmt - Parse the INTRINSIC statement.
///
///   [12.3.2.4] R1216:
///     intrinsic-stmt :=
///         INTRINSIC [::] intrinsic-procedure-name-list
Parser::StmtResult Parser::ParseINTRINSICStmt() {
  return StmtResult();
}

/// ParseNAMELISTStmt - Parse the NAMELIST statement.
///
///   [5.4] R552:
///     namelist-stmt :=
///         NAMELIST #
///         # / namelist-group-name / namelist-group-object-list #
///         # [ [,] / namelist-group-name / #
///         #   namelist-group-object-list ] ...
Parser::StmtResult Parser::ParseNAMELISTStmt() {
  return StmtResult();
}

/// ParseOPTIONALStmt - Parse the OPTIONAL statement.
///
///   [5.2.8] R537:
///     optional-stmt :=
///         OPTIONAL [::] dummy-arg-name-list
Parser::StmtResult Parser::ParseOPTIONALStmt() {
  return StmtResult();
}

/// ParsePOINTERStmt - Parse the POINTER statement.
///
///   [5.2.10] R540:
///     pointer-stmt :=
///         POINTER [::] pointer-decl-list
Parser::StmtResult Parser::ParsePOINTERStmt() {
  return StmtResult();
}

/// ParsePROTECTEDStmt - Parse the PROTECTED statement.
///
///   [5.2.11] R542:
///     protected-stmt :=
///         PROTECTED [::] entity-name-list
Parser::StmtResult Parser::ParsePROTECTEDStmt() {
  return StmtResult();
}

/// ParseSAVEStmt - Parse the SAVE statement.
///
///   [5.2.12] R543:
///     save-stmt :=
///         SAVE [ [::] saved-entity-list ]
Parser::StmtResult Parser::ParseSAVEStmt() {
  return StmtResult();
}

/// ParseTARGETStmt - Parse the TARGET statement.
///
///   [5.2.13] R546:
///     target-stmt :=
///         TARGET [::] object-name [ ( array-spec ) ] #
///         # [ , object-name [ ( array-spec ) ] ] ...
Parser::StmtResult Parser::ParseTARGETStmt() {
  return StmtResult();
}

/// ParseVALUEStmt - Parse the VALUE statement.
///
///   [5.2.14] R547:
///     value-stmt :=
///         VALUE [::] dummy-arg-name-list
Parser::StmtResult Parser::ParseVALUEStmt() {
  return StmtResult();
}

/// ParseVOLATILEStmt - Parse the VOLATILE statement.
///
///   [5.2.15] R548:
///     volatile-stmt :=
///         VOLATILE [::] object-name-list
Parser::StmtResult Parser::ParseVOLATILEStmt() {
  return StmtResult();
}

/// ParseALLOCATEStmt - Parse the ALLOCATE statement.
///
///   [6.3.1] R623:
///     allocate-stmt :=
///         ALLOCATE ( [ type-spec :: ] alocation-list [ , alloc-opt-list ] )
Parser::StmtResult Parser::ParseALLOCATEStmt() {
  return StmtResult();
}

/// ParseNULLIFYStmt - Parse the NULLIFY statement.
///
///   [6.3.2] R633:
///     nullify-stmt :=
///         NULLIFY ( pointer-object-list )
Parser::StmtResult Parser::ParseNULLIFYStmt() {
  return StmtResult();
}

/// ParseDEALLOCATEStmt - Parse the DEALLOCATE statement.
///
///   [6.3.3] R635:
///     deallocate-stmt :=
///         DEALLOCATE ( allocate-object-list [ , dealloc-op-list ] )
Parser::StmtResult Parser::ParseDEALLOCATEStmt() {
  return StmtResult();
}

/// ParseWHEREStmt - Parse the WHERE statement.
///
///   [7.4.3.1] R743:
///     where-stmt :=
///         WHERE ( mask-expr ) where-assignment-stmt
Parser::StmtResult Parser::ParseWHEREStmt() {
  return StmtResult();
}

/// ParseFORALLStmt - Parse the FORALL construct statement.
///
///   [7.4.4.1] R753:
///     forall-construct-stmt :=
///         [forall-construct-name :] FORALL forall-header
Parser::StmtResult Parser::ParseFORALLStmt() {
  return StmtResult();
}

/// ParseENDFORALLStmt - Parse the END FORALL construct statement.
/// 
///   [7.4.4.1] R758:
///     end-forall-stmt :=
///         END FORALL [forall-construct-name]
Parser::StmtResult Parser::ParseEND_FORALLStmt() {
  return StmtResult();
}
