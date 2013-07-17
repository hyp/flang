! RUN: %flang -emit-llvm -o - %s | %file_check %s

SUBROUTINE FOO(STR) ! CHECK: define void @FOO({ i8*, i64 }
  CHARACTER*(*) STR
  STR = 'AGAIN'
END

CHARACTER*10 FUNCTION BAR(I) ! CHECK: define void @BAR
  INTEGER I
  BAR = 'STRING'
  BAR = BAR
END

PROGRAM test
  CHARACTER STR     ! CHECK: alloca [1 x i8]
  CHARACTER*20 STR2 ! CHECK: alloca [20 x i8]
  PARAMETER (Label = '...')
  LOGICAL L

  STR = 'HELLO' ! CHECK: call void @libflang_assignment_char1
  STR = STR
  STR = STR(1:1)

  STR = STR // ' WORLD' ! CHECK: call void @libflang_concat_char1

  L = STR .EQ. STR      ! CHECK: call i32 @libflang_compare_char1
  CONTINUE              ! CHECK: icmp eq i32

  L = STR .NE. STR      ! CHECK: call i32 @libflang_compare_char1
  CONTINUE              ! CHECK: icmp ne i32

  CALL FOO(STR)

  STR = BAR(2)

  STR2 = 'GREETINGS'
  STR2 = Label

  CALL FOO(BAR(1))

  STR2 = 'JK ' // BAR(10) // ' KG'

END PROGRAM
