; NOTE: Assertions have been autogenerated by utils/update_test_checks.py
; RUN: opt -instcombine -S < %s | FileCheck %s
; PR5438

; TODO: This should also optimize down.
define i32 @test1(i32 %a, i32 %b) nounwind readnone {
; CHECK-LABEL: @test1(
; CHECK-NEXT:    [[T0:%.*]] = icmp sgt i32 [[A:%.*]], -1
; CHECK-NEXT:    [[T1:%.*]] = icmp slt i32 [[B:%.*]], 0
; CHECK-NEXT:    [[T2:%.*]] = xor i1 [[T1]], [[T0]]
; CHECK-NEXT:    [[T3:%.*]] = zext i1 [[T2]] to i32
; CHECK-NEXT:    ret i32 [[T3]]
;
  %t0 = icmp sgt i32 %a, -1
  %t1 = icmp slt i32 %b, 0
  %t2 = xor i1 %t1, %t0
  %t3 = zext i1 %t2 to i32
  ret i32 %t3
}

; TODO: This optimizes partially but not all the way.
define i32 @test2(i32 %a, i32 %b) nounwind readnone {
; CHECK-LABEL: @test2(
; CHECK-NEXT:    [[TMP1:%.*]] = xor i32 [[A:%.*]], [[B:%.*]]
; CHECK-NEXT:    [[TMP2:%.*]] = lshr i32 [[TMP1]], 3
; CHECK-NEXT:    [[DOTLOBIT:%.*]] = and i32 [[TMP2]], 1
; CHECK-NEXT:    [[TMP3:%.*]] = xor i32 [[DOTLOBIT]], 1
; CHECK-NEXT:    ret i32 [[TMP3]]
;
  %t0 = and i32 %a, 8
  %t1 = and i32 %b, 8
  %t2 = icmp eq i32 %t0, %t1
  %t3 = zext i1 %t2 to i32
  ret i32 %t3
}

define i32 @test3(i32 %a, i32 %b) nounwind readnone {
; CHECK-LABEL: @test3(
; CHECK-NEXT:    [[T2_UNSHIFTED:%.*]] = xor i32 %a, %b
; CHECK-NEXT:    [[T2_UNSHIFTED_LOBIT:%.*]] = lshr i32 [[T2_UNSHIFTED]], 31
; CHECK-NEXT:    [[T2_UNSHIFTED_LOBIT_NOT:%.*]] = xor i32 [[T2_UNSHIFTED_LOBIT]], 1
; CHECK-NEXT:    ret i32 [[T2_UNSHIFTED_LOBIT_NOT]]
;
  %t0 = lshr i32 %a, 31
  %t1 = lshr i32 %b, 31
  %t2 = icmp eq i32 %t0, %t1
  %t3 = zext i1 %t2 to i32
  ret i32 %t3
}

; Variation on @test3: checking the 2nd bit in a situation where the 5th bit
; is one, not zero.
define i32 @test3i(i32 %a, i32 %b) nounwind readnone {
; CHECK-LABEL: @test3i(
; CHECK-NEXT:    [[T01:%.*]] = xor i32 %a, %b
; CHECK-NEXT:    [[TMP1:%.*]] = lshr i32 [[T01]], 31
; CHECK-NEXT:    [[T4:%.*]] = xor i32 [[TMP1]], 1
; CHECK-NEXT:    ret i32 [[T4]]
;
  %t0 = lshr i32 %a, 29
  %t1 = lshr i32 %b, 29
  %t2 = or i32 %t0, 35
  %t3 = or i32 %t1, 35
  %t4 = icmp eq i32 %t2, %t3
  %t5 = zext i1 %t4 to i32
  ret i32 %t5
}

define i1 @test4a(i32 %a) {
; CHECK-LABEL: @test4a(
; CHECK-NEXT:    [[C:%.*]] = icmp slt i32 %a, 1
; CHECK-NEXT:    ret i1 [[C]]
;
  %l = ashr i32 %a, 31
  %na = sub i32 0, %a
  %r = lshr i32 %na, 31
  %signum = or i32 %l, %r
  %c = icmp slt i32 %signum, 1
  ret i1 %c
}

define <2 x i1> @test4a_vec(<2 x i32> %a) {
; CHECK-LABEL: @test4a_vec(
; CHECK-NEXT:    [[C:%.*]] = icmp slt <2 x i32> %a, <i32 1, i32 1>
; CHECK-NEXT:    ret <2 x i1> [[C]]
;
  %l = ashr <2 x i32> %a, <i32 31, i32 31>
  %na = sub <2 x i32> zeroinitializer, %a
  %r = lshr <2 x i32> %na, <i32 31, i32 31>
  %signum = or <2 x i32> %l, %r
  %c = icmp slt <2 x i32> %signum, <i32 1, i32 1>
  ret <2 x i1> %c
}

define i1 @test4b(i64 %a) {
; CHECK-LABEL: @test4b(
; CHECK-NEXT:    [[C:%.*]] = icmp slt i64 %a, 1
; CHECK-NEXT:    ret i1 [[C]]
;
  %l = ashr i64 %a, 63
  %na = sub i64 0, %a
  %r = lshr i64 %na, 63
  %signum = or i64 %l, %r
  %c = icmp slt i64 %signum, 1
  ret i1 %c
}

define i1 @test4c(i64 %a) {
; CHECK-LABEL: @test4c(
; CHECK-NEXT:    [[C:%.*]] = icmp slt i64 %a, 1
; CHECK-NEXT:    ret i1 [[C]]
;
  %l = ashr i64 %a, 63
  %na = sub i64 0, %a
  %r = lshr i64 %na, 63
  %signum = or i64 %l, %r
  %signum.trunc = trunc i64 %signum to i32
  %c = icmp slt i32 %signum.trunc, 1
  ret i1 %c
}

define <2 x i1> @test4c_vec(<2 x i64> %a) {
; CHECK-LABEL: @test4c_vec(
; CHECK-NEXT:    [[C:%.*]] = icmp slt <2 x i64> %a, <i64 1, i64 1>
; CHECK-NEXT:    ret <2 x i1> [[C]]
;
  %l = ashr <2 x i64> %a, <i64 63, i64 63>
  %na = sub <2 x i64> zeroinitializer, %a
  %r = lshr <2 x i64> %na, <i64 63, i64 63>
  %signum = or <2 x i64> %l, %r
  %signum.trunc = trunc <2 x i64> %signum to <2 x i32>
  %c = icmp slt <2 x i32> %signum.trunc, <i32 1, i32 1>
  ret <2 x i1> %c
}

