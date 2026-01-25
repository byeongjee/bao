; ModuleID = 'test/intermittent/cuckoo_filter.ll'
source_filename = "test/intermittent/cuckoo_filter.c"
target datalayout = "e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-n32:64-S128-Fn32"
target triple = "arm64-apple-macosx26.0.0"

@filter = internal unnamed_addr global [256 x i16] zeroinitializer, align 2
@checkpoint_name = private unnamed_addr constant [4 x i8] c"bb3\00", align 1
@checkpoint_name.1 = private unnamed_addr constant [4 x i8] c"bb5\00", align 1
@checkpoint_name.2 = private unnamed_addr constant [4 x i8] c"bb7\00", align 1
@checkpoint_name.3 = private unnamed_addr constant [5 x i8] c"bb14\00", align 1
@checkpoint_name.4 = private unnamed_addr constant [5 x i8] c"bb19\00", align 1
@checkpoint_name.5 = private unnamed_addr constant [5 x i8] c"bb21\00", align 1
@checkpoint_name.6 = private unnamed_addr constant [5 x i8] c"bb23\00", align 1

; Function Attrs: mustprogress nofree norecurse nosync nounwind ssp willreturn memory(none) uwtable(sync)
define void @print_filter(ptr noundef readonly captures(none) %0) local_unnamed_addr #0 {
  ret void
}

; Function Attrs: nofree norecurse nounwind ssp memory(readwrite, argmem: none) uwtable(sync)
define noundef i32 @main() local_unnamed_addr #1 {
  %1 = alloca i16, align 2
  %2 = alloca i16, align 2
  %3 = alloca i16, align 2
  %4 = alloca i16, align 2
  %5 = alloca i16, align 2
  %6 = alloca i16, align 2
  %7 = alloca i16, align 2
  %8 = alloca i32, align 4
  tail call void @llvm.memset.p0.i64(ptr noundef nonnull align 2 dereferenceable(512) @filter, i8 0, i64 512, i1 false), !tbaa !6
  br label %10

9:                                                ; preds = %105
  call void @llvm.lifetime.start.p0(ptr %8)
  store volatile i32 0, ptr %8, align 4, !tbaa !10
  br label %110

10:                                               ; preds = %105, %0
  %11 = phi i16 [ 1, %0 ], [ %15, %105 ]
  %12 = phi i32 [ 0, %0 ], [ %107, %105 ]
  %13 = phi i16 [ -21279, %0 ], [ %106, %105 ]
  %14 = mul i16 %11, 17
  %15 = add i16 %14, 17
  call void @llvm.lifetime.start.p0(ptr %7)
  store i16 %15, ptr %7, align 2, !tbaa !6
  br label %16

16:                                               ; preds = %16, %10
  %17 = phi i32 [ 5381, %10 ], [ %23, %16 ]
  %18 = phi i64 [ 0, %10 ], [ %24, %16 ]
  call void @__checkpoint(ptr @checkpoint_name)
  %19 = getelementptr inbounds nuw i8, ptr %7, i64 %18
  %20 = mul i32 %17, 33
  %21 = load i8, ptr %19, align 1, !tbaa !12
  %22 = zext i8 %21 to i32
  %23 = add i32 %20, %22
  %24 = add nuw nsw i64 %18, 1
  %25 = icmp eq i64 %18, 1
  br i1 %25, label %26, label %16, !llvm.loop !13

26:                                               ; preds = %16
  call void @llvm.lifetime.end.p0(ptr %7)
  call void @llvm.lifetime.start.p0(ptr %6)
  store i16 %15, ptr %6, align 2, !tbaa !6
  br label %27

27:                                               ; preds = %27, %26
  %28 = phi i32 [ 5381, %26 ], [ %34, %27 ]
  %29 = phi i64 [ 0, %26 ], [ %35, %27 ]
  call void @__checkpoint(ptr @checkpoint_name.1)
  %30 = getelementptr inbounds nuw i8, ptr %6, i64 %29
  %31 = mul i32 %28, 33
  %32 = load i8, ptr %30, align 1, !tbaa !12
  %33 = zext i8 %32 to i32
  %34 = add i32 %31, %33
  %35 = add nuw nsw i64 %29, 1
  %36 = icmp eq i64 %29, 1
  br i1 %36, label %37, label %27, !llvm.loop !13

37:                                               ; preds = %27
  %38 = trunc i32 %23 to i16
  %39 = tail call range(i16 1, 0) i16 @llvm.umax.i16(i16 %38, i16 1)
  call void @llvm.lifetime.end.p0(ptr %6)
  call void @llvm.lifetime.start.p0(ptr %5)
  store i16 %39, ptr %5, align 2, !tbaa !6
  br label %40

40:                                               ; preds = %40, %37
  %41 = phi i32 [ 5381, %37 ], [ %47, %40 ]
  %42 = phi i64 [ 0, %37 ], [ %48, %40 ]
  call void @__checkpoint(ptr @checkpoint_name.2)
  %43 = getelementptr inbounds nuw i8, ptr %5, i64 %42
  %44 = mul i32 %41, 33
  %45 = load i8, ptr %43, align 1, !tbaa !12
  %46 = zext i8 %45 to i32
  %47 = add i32 %44, %46
  %48 = add nuw nsw i64 %42, 1
  %49 = icmp eq i64 %42, 1
  br i1 %49, label %50, label %40, !llvm.loop !13

50:                                               ; preds = %40
  %51 = trunc i32 %34 to i16
  %52 = and i16 %51, 255
  call void @llvm.lifetime.end.p0(ptr %5)
  %53 = xor i32 %47, %34
  %54 = trunc i32 %53 to i16
  %55 = and i16 %54, 255
  %56 = zext nneg i16 %52 to i64
  %57 = getelementptr inbounds nuw i16, ptr @filter, i64 %56
  %58 = load i16, ptr %57, align 2, !tbaa !6
  %59 = icmp eq i16 %58, 0
  br i1 %59, label %60, label %61

60:                                               ; preds = %50
  store i16 %39, ptr %57, align 2, !tbaa !6
  br label %105

61:                                               ; preds = %50
  %62 = zext nneg i16 %55 to i64
  %63 = getelementptr inbounds nuw i16, ptr @filter, i64 %62
  %64 = load i16, ptr %63, align 2, !tbaa !6
  %65 = icmp eq i16 %64, 0
  br i1 %65, label %66, label %67

66:                                               ; preds = %61
  store i16 %39, ptr %63, align 2, !tbaa !6
  br label %105

67:                                               ; preds = %61
  %68 = and i16 %13, 1
  %69 = icmp eq i16 %68, 0
  %70 = lshr i16 %13, 1
  %71 = xor i16 %70, -19456
  %72 = select i1 %69, i16 %70, i16 %71
  %73 = and i16 %72, 128
  %74 = icmp eq i16 %73, 0
  %75 = select i1 %74, i16 %55, i16 %52
  %76 = zext nneg i16 %75 to i64
  %77 = getelementptr inbounds nuw i16, ptr @filter, i64 %76
  %78 = load i16, ptr %77, align 2, !tbaa !6
  store i16 %39, ptr %77, align 2, !tbaa !6
  br label %79

79:                                               ; preds = %93, %67
  %80 = phi i32 [ 0, %67 ], [ %101, %93 ]
  %81 = phi i16 [ %75, %67 ], [ %96, %93 ]
  %82 = phi i16 [ %78, %67 ], [ %100, %93 ]
  call void @llvm.lifetime.start.p0(ptr %4)
  store i16 %82, ptr %4, align 2, !tbaa !6
  br label %83

83:                                               ; preds = %83, %79
  %84 = phi i32 [ 5381, %79 ], [ %90, %83 ]
  %85 = phi i64 [ 0, %79 ], [ %91, %83 ]
  call void @__checkpoint(ptr @checkpoint_name.3)
  %86 = getelementptr inbounds nuw i8, ptr %4, i64 %85
  %87 = mul i32 %84, 33
  %88 = load i8, ptr %86, align 1, !tbaa !12
  %89 = zext i8 %88 to i32
  %90 = add i32 %87, %89
  %91 = add nuw nsw i64 %85, 1
  %92 = icmp eq i64 %85, 1
  br i1 %92, label %93, label %83, !llvm.loop !13

93:                                               ; preds = %83
  %94 = trunc i32 %90 to i16
  %95 = and i16 %94, 255
  call void @llvm.lifetime.end.p0(ptr %4)
  %96 = xor i16 %95, %81
  %97 = zext nneg i16 %96 to i64
  %98 = getelementptr inbounds nuw i16, ptr @filter, i64 %97
  %99 = load i16, ptr %98, align 2, !tbaa !6
  %100 = freeze i16 %99
  store i16 %82, ptr %98, align 2, !tbaa !6
  %101 = add nuw nsw i32 %80, 1
  %102 = icmp ne i16 %100, 0
  %103 = icmp samesign ult i32 %80, 7
  %104 = select i1 %102, i1 %103, i1 false
  br i1 %104, label %79, label %105, !llvm.loop !16

105:                                              ; preds = %93, %66, %60
  %106 = phi i16 [ %13, %60 ], [ %13, %66 ], [ %72, %93 ]
  %107 = add nuw nsw i32 %12, 1
  %108 = icmp eq i32 %107, 128
  br i1 %108, label %9, label %10, !llvm.loop !17

109:                                              ; preds = %165
  call void @llvm.lifetime.end.p0(ptr %8)
  ret i32 0

110:                                              ; preds = %165, %9
  %111 = phi i32 [ 0, %9 ], [ %166, %165 ]
  %112 = phi i16 [ 1, %9 ], [ %114, %165 ]
  %113 = mul i16 %112, 17
  %114 = add i16 %113, 17
  call void @llvm.lifetime.start.p0(ptr %3)
  store i16 %114, ptr %3, align 2, !tbaa !6
  br label %115

115:                                              ; preds = %115, %110
  %116 = phi i32 [ 5381, %110 ], [ %122, %115 ]
  %117 = phi i64 [ 0, %110 ], [ %123, %115 ]
  call void @__checkpoint(ptr @checkpoint_name.4)
  %118 = getelementptr inbounds nuw i8, ptr %3, i64 %117
  %119 = mul i32 %116, 33
  %120 = load i8, ptr %118, align 1, !tbaa !12
  %121 = zext i8 %120 to i32
  %122 = add i32 %119, %121
  %123 = add nuw nsw i64 %117, 1
  %124 = icmp eq i64 %117, 1
  br i1 %124, label %125, label %115, !llvm.loop !13

125:                                              ; preds = %115
  call void @llvm.lifetime.end.p0(ptr %3)
  call void @llvm.lifetime.start.p0(ptr %2)
  store i16 %114, ptr %2, align 2, !tbaa !6
  br label %126

126:                                              ; preds = %126, %125
  %127 = phi i32 [ 5381, %125 ], [ %133, %126 ]
  %128 = phi i64 [ 0, %125 ], [ %134, %126 ]
  call void @__checkpoint(ptr @checkpoint_name.5)
  %129 = getelementptr inbounds nuw i8, ptr %2, i64 %128
  %130 = mul i32 %127, 33
  %131 = load i8, ptr %129, align 1, !tbaa !12
  %132 = zext i8 %131 to i32
  %133 = add i32 %130, %132
  %134 = add nuw nsw i64 %128, 1
  %135 = icmp eq i64 %128, 1
  br i1 %135, label %136, label %126, !llvm.loop !13

136:                                              ; preds = %126
  %137 = trunc i32 %122 to i16
  %138 = tail call range(i16 1, 0) i16 @llvm.umax.i16(i16 %137, i16 1)
  call void @llvm.lifetime.end.p0(ptr %2)
  call void @llvm.lifetime.start.p0(ptr %1)
  store i16 %138, ptr %1, align 2, !tbaa !6
  br label %139

139:                                              ; preds = %139, %136
  %140 = phi i32 [ 5381, %136 ], [ %146, %139 ]
  %141 = phi i64 [ 0, %136 ], [ %147, %139 ]
  call void @__checkpoint(ptr @checkpoint_name.6)
  %142 = getelementptr inbounds nuw i8, ptr %1, i64 %141
  %143 = mul i32 %140, 33
  %144 = load i8, ptr %142, align 1, !tbaa !12
  %145 = zext i8 %144 to i32
  %146 = add i32 %143, %145
  %147 = add nuw nsw i64 %141, 1
  %148 = icmp eq i64 %141, 1
  br i1 %148, label %149, label %139, !llvm.loop !13

149:                                              ; preds = %139
  %150 = and i32 %133, 255
  %151 = zext nneg i32 %150 to i64
  call void @llvm.lifetime.end.p0(ptr %1)
  %152 = getelementptr inbounds nuw i16, ptr @filter, i64 %151
  %153 = load i16, ptr %152, align 2, !tbaa !6
  %154 = icmp eq i16 %153, %138
  br i1 %154, label %162, label %155

155:                                              ; preds = %149
  %156 = xor i32 %146, %133
  %157 = and i32 %156, 255
  %158 = zext nneg i32 %157 to i64
  %159 = getelementptr inbounds nuw i16, ptr @filter, i64 %158
  %160 = load i16, ptr %159, align 2, !tbaa !6
  %161 = icmp eq i16 %160, %138
  br i1 %161, label %162, label %165

162:                                              ; preds = %155, %149
  %163 = load volatile i32, ptr %8, align 4, !tbaa !10
  %164 = add i32 %163, 1
  store volatile i32 %164, ptr %8, align 4, !tbaa !10
  br label %165

165:                                              ; preds = %162, %155
  %166 = add nuw nsw i32 %111, 1
  %167 = icmp eq i32 %166, 128
  br i1 %167, label %109, label %110, !llvm.loop !18
}

; Function Attrs: nocallback nocreateundeforpoison nofree nosync nounwind speculatable willreturn memory(none)
declare i16 @llvm.umax.i16(i16, i16) #2

; Function Attrs: nocallback nofree nounwind willreturn memory(argmem: write)
declare void @llvm.memset.p0.i64(ptr writeonly captures(none), i8, i64, i1 immarg) #3

declare void @llvm.lifetime.start.i64(i64)

; Function Attrs: nocallback nofree nosync nounwind willreturn memory(argmem: readwrite)
declare void @llvm.lifetime.start.p0(ptr captures(none)) #4

declare void @llvm.lifetime.end.i64(i64)

; Function Attrs: nocallback nofree nosync nounwind willreturn memory(argmem: readwrite)
declare void @llvm.lifetime.end.p0(ptr captures(none)) #4

declare void @__checkpoint(ptr)

attributes #0 = { mustprogress nofree norecurse nosync nounwind ssp willreturn memory(none) uwtable(sync) "frame-pointer"="non-leaf" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="apple-m1" "target-features"="+aes,+altnzcv,+ccdp,+ccidx,+ccpp,+complxnum,+crc,+dit,+dotprod,+flagm,+fp-armv8,+fp16fml,+fptoint,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+predres,+ras,+rcpc,+rdm,+sb,+sha2,+sha3,+specrestrict,+ssbs,+v8.1a,+v8.2a,+v8.3a,+v8.4a,+v8a" }
attributes #1 = { nofree norecurse nounwind ssp memory(readwrite, argmem: none) uwtable(sync) "frame-pointer"="non-leaf" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="apple-m1" "target-features"="+aes,+altnzcv,+ccdp,+ccidx,+ccpp,+complxnum,+crc,+dit,+dotprod,+flagm,+fp-armv8,+fp16fml,+fptoint,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+predres,+ras,+rcpc,+rdm,+sb,+sha2,+sha3,+specrestrict,+ssbs,+v8.1a,+v8.2a,+v8.3a,+v8.4a,+v8a" }
attributes #2 = { nocallback nocreateundeforpoison nofree nosync nounwind speculatable willreturn memory(none) }
attributes #3 = { nocallback nofree nounwind willreturn memory(argmem: write) }
attributes #4 = { nocallback nofree nosync nounwind willreturn memory(argmem: readwrite) }

!llvm.module.flags = !{!0, !1, !2, !3, !4}
!llvm.ident = !{!5}

!0 = !{i32 2, !"SDK Version", [2 x i32] [i32 26, i32 2]}
!1 = !{i32 1, !"wchar_size", i32 4}
!2 = !{i32 8, !"PIC Level", i32 2}
!3 = !{i32 7, !"uwtable", i32 1}
!4 = !{i32 7, !"frame-pointer", i32 1}
!5 = !{!"Homebrew clang version 21.1.1"}
!6 = !{!7, !7, i64 0}
!7 = !{!"short", !8, i64 0}
!8 = !{!"omnipotent char", !9, i64 0}
!9 = !{!"Simple C/C++ TBAA"}
!10 = !{!11, !11, i64 0}
!11 = !{!"int", !8, i64 0}
!12 = !{!8, !8, i64 0}
!13 = distinct !{!13, !14, !15}
!14 = !{!"llvm.loop.mustprogress"}
!15 = !{!"llvm.loop.unroll.disable"}
!16 = distinct !{!16, !14, !15}
!17 = distinct !{!17, !14, !15}
!18 = distinct !{!18, !14, !15}
