! RUN: %flang -verify < %s
PROGRAM imptest
  IMPLICIT NONE
  IMPLICIT REAL (Y) ! expected-error {{use of 'IMPLICIT' after 'IMPLICIT NONE'}}
  INTEGER X

  X = 1
  Y = X ! expected-error {{use of undeclared identifier 'Y'}}
  I = 0 ! expected-error {{use of undeclared identifier 'I'}}

END PROGRAM imptest
