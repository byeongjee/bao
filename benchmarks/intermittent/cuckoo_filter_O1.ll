; ModuleID = 'test/intermittent/cuckoo_filter.c'
source_filename = "test/intermittent/cuckoo_filter.c"
target datalayout = "e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-n32:64-S128-Fn32"
target triple = "arm64-apple-macosx26.0.0"

@filter = internal unnamed_addr global [256 x i16] zeroinitializer, align 2

; Function Attrs: mustprogress nofree norecurse nosync nounwind ssp willreturn memory(none) uwtable(sync)
define void @print_filter(ptr noundef readonly captures(none) %0) local_unnamed_addr #0 {
  ret void
}

; Function Attrs: mustprogress nocallback nofree nosync nounwind willreturn memory(argmem: readwrite)
declare void @llvm.lifetime.start.p0(i64 immarg, ptr captures(none)) #1

; Function Attrs: mustprogress nocallback nofree nosync nounwind willreturn memory(argmem: readwrite)
declare void @llvm.lifetime.end.p0(i64 immarg, ptr captures(none)) #1

; Function Attrs: nofree norecurse nounwind ssp memory(readwrite, argmem: none) uwtable(sync)
define noundef i32 @main() local_unnamed_addr #2 {
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

9:                                                ; preds = %108
  call void @llvm.lifetime.start.p0(i64 4, ptr nonnull %8)
  store volatile i32 0, ptr %8, align 4, !tbaa !10
  br label %113

10:                                               ; preds = %0, %108
  %11 = phi i16 [ 1, %0 ], [ %15, %108 ]
  %12 = phi i32 [ 0, %0 ], [ %110, %108 ]
  %13 = phi i16 [ -21279, %0 ], [ %109, %108 ]
  %14 = mul i16 %11, 17
  %15 = add i16 %14, 17
  call void @llvm.lifetime.start.p0(i64 2, ptr nonnull %7)
  store i16 %15, ptr %7, align 2, !tbaa !6
  br label %16

16:                                               ; preds = %16, %10
  %17 = phi i32 [ 0, %10 ], [ %25, %16 ]
  %18 = phi i32 [ 5381, %10 ], [ %23, %16 ]
  %19 = phi ptr [ %7, %10 ], [ %24, %16 ]
  %20 = mul i32 %18, 33
  %21 = load i8, ptr %19, align 1, !tbaa !12
  %22 = zext i8 %21 to i32
  %23 = add i32 %20, %22
  %24 = getelementptr inbounds nuw i8, ptr %19, i64 1
  %25 = add nuw nsw i32 %17, 1
  %26 = icmp eq i32 %17, 0
  br i1 %26, label %16, label %27, !llvm.loop !13

27:                                               ; preds = %16
  call void @llvm.lifetime.end.p0(i64 2, ptr nonnull %7)
  call void @llvm.lifetime.start.p0(i64 2, ptr nonnull %6)
  store i16 %15, ptr %6, align 2, !tbaa !6
  br label %28

28:                                               ; preds = %28, %27
  %29 = phi i32 [ 0, %27 ], [ %37, %28 ]
  %30 = phi i32 [ 5381, %27 ], [ %35, %28 ]
  %31 = phi ptr [ %6, %27 ], [ %36, %28 ]
  %32 = mul i32 %30, 33
  %33 = load i8, ptr %31, align 1, !tbaa !12
  %34 = zext i8 %33 to i32
  %35 = add i32 %32, %34
  %36 = getelementptr inbounds nuw i8, ptr %31, i64 1
  %37 = add nuw nsw i32 %29, 1
  %38 = icmp eq i32 %29, 0
  br i1 %38, label %28, label %39, !llvm.loop !13

39:                                               ; preds = %28
  %40 = trunc i32 %23 to i16
  %41 = tail call range(i16 1, 0) i16 @llvm.umax.i16(i16 %40, i16 1)
  call void @llvm.lifetime.end.p0(i64 2, ptr nonnull %6)
  call void @llvm.lifetime.start.p0(i64 2, ptr nonnull %5)
  store i16 %41, ptr %5, align 2, !tbaa !6
  br label %42

42:                                               ; preds = %42, %39
  %43 = phi i32 [ 0, %39 ], [ %51, %42 ]
  %44 = phi i32 [ 5381, %39 ], [ %49, %42 ]
  %45 = phi ptr [ %5, %39 ], [ %50, %42 ]
  %46 = mul i32 %44, 33
  %47 = load i8, ptr %45, align 1, !tbaa !12
  %48 = zext i8 %47 to i32
  %49 = add i32 %46, %48
  %50 = getelementptr inbounds nuw i8, ptr %45, i64 1
  %51 = add nuw nsw i32 %43, 1
  %52 = icmp eq i32 %43, 0
  br i1 %52, label %42, label %53, !llvm.loop !13

53:                                               ; preds = %42
  %54 = trunc i32 %35 to i16
  %55 = and i16 %54, 255
  call void @llvm.lifetime.end.p0(i64 2, ptr nonnull %5)
  %56 = xor i32 %49, %35
  %57 = trunc i32 %56 to i16
  %58 = and i16 %57, 255
  %59 = zext nneg i16 %55 to i64
  %60 = getelementptr inbounds nuw i16, ptr @filter, i64 %59
  %61 = load i16, ptr %60, align 2, !tbaa !6
  %62 = icmp eq i16 %61, 0
  br i1 %62, label %63, label %64

63:                                               ; preds = %53
  store i16 %41, ptr %60, align 2, !tbaa !6
  br label %108

64:                                               ; preds = %53
  %65 = zext nneg i16 %58 to i64
  %66 = getelementptr inbounds nuw i16, ptr @filter, i64 %65
  %67 = load i16, ptr %66, align 2, !tbaa !6
  %68 = icmp eq i16 %67, 0
  br i1 %68, label %69, label %70

69:                                               ; preds = %64
  store i16 %41, ptr %66, align 2, !tbaa !6
  br label %108

70:                                               ; preds = %64
  %71 = and i16 %13, 1
  %72 = icmp eq i16 %71, 0
  %73 = lshr i16 %13, 1
  %74 = xor i16 %73, -19456
  %75 = select i1 %72, i16 %73, i16 %74
  %76 = and i16 %75, 128
  %77 = icmp eq i16 %76, 0
  %78 = select i1 %77, i16 %58, i16 %55
  %79 = zext nneg i16 %78 to i64
  %80 = getelementptr inbounds nuw i16, ptr @filter, i64 %79
  %81 = load i16, ptr %80, align 2, !tbaa !6
  store i16 %41, ptr %80, align 2, !tbaa !6
  br label %82

82:                                               ; preds = %97, %70
  %83 = phi i32 [ 0, %70 ], [ %104, %97 ]
  %84 = phi i16 [ %78, %70 ], [ %100, %97 ]
  %85 = phi i16 [ %81, %70 ], [ %103, %97 ]
  call void @llvm.lifetime.start.p0(i64 2, ptr nonnull %4)
  store i16 %85, ptr %4, align 2, !tbaa !6
  br label %86

86:                                               ; preds = %86, %82
  %87 = phi i32 [ 0, %82 ], [ %95, %86 ]
  %88 = phi i32 [ 5381, %82 ], [ %93, %86 ]
  %89 = phi ptr [ %4, %82 ], [ %94, %86 ]
  %90 = mul i32 %88, 33
  %91 = load i8, ptr %89, align 1, !tbaa !12
  %92 = zext i8 %91 to i32
  %93 = add i32 %90, %92
  %94 = getelementptr inbounds nuw i8, ptr %89, i64 1
  %95 = add nuw nsw i32 %87, 1
  %96 = icmp eq i32 %87, 0
  br i1 %96, label %86, label %97, !llvm.loop !13

97:                                               ; preds = %86
  %98 = trunc i32 %93 to i16
  %99 = and i16 %98, 255
  call void @llvm.lifetime.end.p0(i64 2, ptr nonnull %4)
  %100 = xor i16 %99, %84
  %101 = zext nneg i16 %100 to i64
  %102 = getelementptr inbounds nuw i16, ptr @filter, i64 %101
  %103 = load i16, ptr %102, align 2, !tbaa !6
  store i16 %85, ptr %102, align 2, !tbaa !6
  %104 = add nuw nsw i32 %83, 1
  %105 = icmp ne i16 %103, 0
  %106 = icmp samesign ult i32 %83, 7
  %107 = select i1 %105, i1 %106, i1 false
  br i1 %107, label %82, label %108, !llvm.loop !16

108:                                              ; preds = %97, %63, %69
  %109 = phi i16 [ %13, %63 ], [ %13, %69 ], [ %75, %97 ]
  %110 = add nuw nsw i32 %12, 1
  %111 = icmp eq i32 %110, 128
  br i1 %111, label %9, label %10, !llvm.loop !17

112:                                              ; preds = %171
  call void @llvm.lifetime.end.p0(i64 4, ptr nonnull %8)
  ret i32 0

113:                                              ; preds = %9, %171
  %114 = phi i32 [ 0, %9 ], [ %172, %171 ]
  %115 = phi i16 [ 1, %9 ], [ %117, %171 ]
  %116 = mul i16 %115, 17
  %117 = add i16 %116, 17
  call void @llvm.lifetime.start.p0(i64 2, ptr nonnull %3)
  store i16 %117, ptr %3, align 2, !tbaa !6
  br label %118

118:                                              ; preds = %118, %113
  %119 = phi i32 [ 0, %113 ], [ %127, %118 ]
  %120 = phi i32 [ 5381, %113 ], [ %125, %118 ]
  %121 = phi ptr [ %3, %113 ], [ %126, %118 ]
  %122 = mul i32 %120, 33
  %123 = load i8, ptr %121, align 1, !tbaa !12
  %124 = zext i8 %123 to i32
  %125 = add i32 %122, %124
  %126 = getelementptr inbounds nuw i8, ptr %121, i64 1
  %127 = add nuw nsw i32 %119, 1
  %128 = icmp eq i32 %119, 0
  br i1 %128, label %118, label %129, !llvm.loop !13

129:                                              ; preds = %118
  call void @llvm.lifetime.end.p0(i64 2, ptr nonnull %3)
  call void @llvm.lifetime.start.p0(i64 2, ptr nonnull %2)
  store i16 %117, ptr %2, align 2, !tbaa !6
  br label %130

130:                                              ; preds = %130, %129
  %131 = phi i32 [ 0, %129 ], [ %139, %130 ]
  %132 = phi i32 [ 5381, %129 ], [ %137, %130 ]
  %133 = phi ptr [ %2, %129 ], [ %138, %130 ]
  %134 = mul i32 %132, 33
  %135 = load i8, ptr %133, align 1, !tbaa !12
  %136 = zext i8 %135 to i32
  %137 = add i32 %134, %136
  %138 = getelementptr inbounds nuw i8, ptr %133, i64 1
  %139 = add nuw nsw i32 %131, 1
  %140 = icmp eq i32 %131, 0
  br i1 %140, label %130, label %141, !llvm.loop !13

141:                                              ; preds = %130
  %142 = trunc i32 %125 to i16
  %143 = tail call range(i16 1, 0) i16 @llvm.umax.i16(i16 %142, i16 1)
  call void @llvm.lifetime.end.p0(i64 2, ptr nonnull %2)
  call void @llvm.lifetime.start.p0(i64 2, ptr nonnull %1)
  store i16 %143, ptr %1, align 2, !tbaa !6
  br label %144

144:                                              ; preds = %144, %141
  %145 = phi i32 [ 0, %141 ], [ %153, %144 ]
  %146 = phi i32 [ 5381, %141 ], [ %151, %144 ]
  %147 = phi ptr [ %1, %141 ], [ %152, %144 ]
  %148 = mul i32 %146, 33
  %149 = load i8, ptr %147, align 1, !tbaa !12
  %150 = zext i8 %149 to i32
  %151 = add i32 %148, %150
  %152 = getelementptr inbounds nuw i8, ptr %147, i64 1
  %153 = add nuw nsw i32 %145, 1
  %154 = icmp eq i32 %145, 0
  br i1 %154, label %144, label %155, !llvm.loop !13

155:                                              ; preds = %144
  %156 = and i32 %137, 255
  %157 = zext nneg i32 %156 to i64
  call void @llvm.lifetime.end.p0(i64 2, ptr nonnull %1)
  %158 = getelementptr inbounds nuw i16, ptr @filter, i64 %157
  %159 = load i16, ptr %158, align 2, !tbaa !6
  %160 = icmp eq i16 %159, %143
  br i1 %160, label %168, label %161

161:                                              ; preds = %155
  %162 = xor i32 %151, %137
  %163 = and i32 %162, 255
  %164 = zext nneg i32 %163 to i64
  %165 = getelementptr inbounds nuw i16, ptr @filter, i64 %164
  %166 = load i16, ptr %165, align 2, !tbaa !6
  %167 = icmp eq i16 %166, %143
  br i1 %167, label %168, label %171

168:                                              ; preds = %155, %161
  %169 = load volatile i32, ptr %8, align 4, !tbaa !10
  %170 = add i32 %169, 1
  store volatile i32 %170, ptr %8, align 4, !tbaa !10
  br label %171

171:                                              ; preds = %161, %168
  %172 = add nuw nsw i32 %114, 1
  %173 = icmp eq i32 %172, 128
  br i1 %173, label %112, label %113, !llvm.loop !18
}

; Function Attrs: nocallback nofree nosync nounwind speculatable willreturn memory(none)
declare i16 @llvm.umax.i16(i16, i16) #3

; Function Attrs: nocallback nofree nounwind willreturn memory(argmem: write)
declare void @llvm.memset.p0.i64(ptr writeonly captures(none), i8, i64, i1 immarg) #4

attributes #0 = { mustprogress nofree norecurse nosync nounwind ssp willreturn memory(none) uwtable(sync) "frame-pointer"="non-leaf" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="apple-m1" "target-features"="+aes,+altnzcv,+ccdp,+ccidx,+ccpp,+complxnum,+crc,+dit,+dotprod,+flagm,+fp-armv8,+fp16fml,+fptoint,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+predres,+ras,+rcpc,+rdm,+sb,+sha2,+sha3,+specrestrict,+ssbs,+v8.1a,+v8.2a,+v8.3a,+v8.4a,+v8a" }
attributes #1 = { mustprogress nocallback nofree nosync nounwind willreturn memory(argmem: readwrite) }
attributes #2 = { nofree norecurse nounwind ssp memory(readwrite, argmem: none) uwtable(sync) "frame-pointer"="non-leaf" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="apple-m1" "target-features"="+aes,+altnzcv,+ccdp,+ccidx,+ccpp,+complxnum,+crc,+dit,+dotprod,+flagm,+fp-armv8,+fp16fml,+fptoint,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+predres,+ras,+rcpc,+rdm,+sb,+sha2,+sha3,+specrestrict,+ssbs,+v8.1a,+v8.2a,+v8.3a,+v8.4a,+v8a" }
attributes #3 = { nocallback nofree nosync nounwind speculatable willreturn memory(none) }
attributes #4 = { nocallback nofree nounwind willreturn memory(argmem: write) }

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
