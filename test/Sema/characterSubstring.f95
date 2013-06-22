! RUN: %flang -verify < %s
PROGRAM charsubstring
  CHARACTER (LEN=16) :: C

  C = 'HELLO'(1:3)
  C = 'HELLO'(1:'FALSE') ! expected-error {{expected an integer expression}}
  C = 'HELLO'(1:)
  C = 'HELLO'(:)
  C = C(4.0:3.0) ! expected-error {{expected an integer expression}}

ENDPROGRAM charsubstring