; ModuleID = 'test/intermittent/cuckoo_filter.ll'
source_filename = "test/intermittent/cuckoo_filter.c"
target datalayout = "e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-n32:64-S128-Fn32"
target triple = "arm64-apple-macosx26.0.0"

@filter = internal unnamed_addr global [256 x i16] zeroinitializer, align 2
@checkpoint_name = private unnamed_addr constant [4 x i8] c"bb7\00", align 1
@checkpoint_name.1 = private unnamed_addr constant [5 x i8] c"bb10\00", align 1
@checkpoint_name.2 = private unnamed_addr constant [5 x i8] c"bb14\00", align 1
@checkpoint_name.3 = private unnamed_addr constant [5 x i8] c"bb19\00", align 1

; Function Attrs: mustprogress nofree norecurse nosync nounwind ssp willreturn memory(none) uwtable(sync)
define void @print_filter(ptr noundef readonly captures(none) %0) local_unnamed_addr #0 {
  ret void
}

; Function Attrs: nofree norecurse nounwind ssp memory(readwrite, argmem: none) uwtable(sync)
define noundef i32 @main() local_unnamed_addr #1 {
  %1 = alloca i32, align 4
  tail call void @llvm.memset.p0.i64(ptr noundef nonnull align 2 dereferenceable(512) @filter, i8 0, i64 512, i1 false), !tbaa !6
  br label %3

2:                                                ; preds = %139
  call void @llvm.lifetime.start.p0(ptr %1)
  store volatile i32 0, ptr %1, align 4, !tbaa !10
  br label %144

3:                                                ; preds = %139, %0
  %4 = phi i16 [ 1, %0 ], [ %8, %139 ]
  %5 = phi i32 [ 0, %0 ], [ %141, %139 ]
  %6 = phi i16 [ -21279, %0 ], [ %140, %139 ]
  %7 = mul i16 %4, 17
  %8 = add i16 %7, 17
  %9 = lshr i16 %8, 8
  %10 = and i16 %8, 255
  %11 = mul nuw nsw i16 %10, 33
  %12 = add nuw nsw i16 %9, 27205
  %13 = add nuw i16 %12, %11
  %14 = mul i16 %8, 33
  %15 = add i16 %14, 69
  %16 = add i16 %15, %9
  %17 = and i16 %16, 255
  %18 = lshr i16 %13, 8
  %19 = mul i16 %13, 33
  %20 = add i16 %19, 69
  %21 = add i16 %20, %18
  %22 = xor i16 %21, %16
  %23 = and i16 %22, 255
  %24 = zext nneg i16 %17 to i64
  %25 = getelementptr inbounds nuw i16, ptr @filter, i64 %24
  %26 = load i16, ptr %25, align 2, !tbaa !6
  %27 = icmp eq i16 %26, 0
  br i1 %27, label %28, label %29

28:                                               ; preds = %3
  store i16 %13, ptr %25, align 2, !tbaa !6
  br label %139

29:                                               ; preds = %3
  %30 = zext nneg i16 %23 to i64
  %31 = getelementptr inbounds nuw i16, ptr @filter, i64 %30
  %32 = load i16, ptr %31, align 2, !tbaa !6
  %33 = icmp eq i16 %32, 0
  br i1 %33, label %34, label %35

34:                                               ; preds = %29
  store i16 %13, ptr %31, align 2, !tbaa !6
  br label %139

35:                                               ; preds = %29
  %36 = and i16 %6, 1
  %37 = icmp eq i16 %36, 0
  %38 = lshr i16 %6, 1
  %39 = xor i16 %38, -19456
  %40 = select i1 %37, i16 %38, i16 %39
  %41 = and i16 %40, 128
  %42 = icmp eq i16 %41, 0
  %43 = select i1 %42, i16 %23, i16 %17
  %44 = zext nneg i16 %43 to i64
  %45 = getelementptr inbounds nuw i16, ptr @filter, i64 %44
  %46 = load i16, ptr %45, align 2, !tbaa !6
  store i16 %13, ptr %45, align 2, !tbaa !6
  %47 = lshr i16 %46, 8
  %48 = mul i16 %46, 33
  %49 = add i16 %48, 69
  %50 = add i16 %49, %47
  %51 = and i16 %50, 255
  %52 = xor i16 %51, %43
  %53 = zext nneg i16 %52 to i64
  %54 = getelementptr inbounds nuw i16, ptr @filter, i64 %53
  %55 = load i16, ptr %54, align 2, !tbaa !6
  %56 = freeze i16 %55
  store i16 %46, ptr %54, align 2, !tbaa !6
  %57 = icmp eq i16 %56, 0
  br i1 %57, label %139, label %58, !llvm.loop !12

58:                                               ; preds = %35
  call void @__checkpoint(ptr @checkpoint_name)
  %59 = lshr i16 %56, 8
  %60 = mul i16 %56, 33
  %61 = add i16 %60, 69
  %62 = add i16 %61, %59
  %63 = and i16 %62, 255
  %64 = xor i16 %63, %52
  %65 = zext nneg i16 %64 to i64
  %66 = getelementptr inbounds nuw i16, ptr @filter, i64 %65
  %67 = load i16, ptr %66, align 2, !tbaa !6
  %68 = freeze i16 %67
  store i16 %56, ptr %66, align 2, !tbaa !6
  %69 = icmp eq i16 %68, 0
  br i1 %69, label %139, label %70, !llvm.loop !12

70:                                               ; preds = %58
  %71 = lshr i16 %68, 8
  %72 = mul i16 %68, 33
  %73 = add i16 %72, 69
  %74 = add i16 %73, %71
  %75 = and i16 %74, 255
  %76 = xor i16 %75, %64
  %77 = zext nneg i16 %76 to i64
  %78 = getelementptr inbounds nuw i16, ptr @filter, i64 %77
  %79 = load i16, ptr %78, align 2, !tbaa !6
  %80 = freeze i16 %79
  store i16 %68, ptr %78, align 2, !tbaa !6
  %81 = icmp eq i16 %80, 0
  br i1 %81, label %139, label %82, !llvm.loop !12

82:                                               ; preds = %70
  %83 = lshr i16 %80, 8
  %84 = mul i16 %80, 33
  %85 = add i16 %84, 69
  %86 = add i16 %85, %83
  %87 = and i16 %86, 255
  %88 = xor i16 %87, %76
  %89 = zext nneg i16 %88 to i64
  %90 = getelementptr inbounds nuw i16, ptr @filter, i64 %89
  %91 = load i16, ptr %90, align 2, !tbaa !6
  %92 = freeze i16 %91
  store i16 %80, ptr %90, align 2, !tbaa !6
  %93 = icmp eq i16 %92, 0
  br i1 %93, label %139, label %94, !llvm.loop !12

94:                                               ; preds = %82
  call void @__checkpoint(ptr @checkpoint_name.1)
  %95 = lshr i16 %92, 8
  %96 = mul i16 %92, 33
  %97 = add i16 %96, 69
  %98 = add i16 %97, %95
  %99 = and i16 %98, 255
  %100 = xor i16 %99, %88
  %101 = zext nneg i16 %100 to i64
  %102 = getelementptr inbounds nuw i16, ptr @filter, i64 %101
  %103 = load i16, ptr %102, align 2, !tbaa !6
  %104 = freeze i16 %103
  store i16 %92, ptr %102, align 2, !tbaa !6
  %105 = icmp eq i16 %104, 0
  br i1 %105, label %139, label %106, !llvm.loop !12

106:                                              ; preds = %94
  %107 = lshr i16 %104, 8
  %108 = mul i16 %104, 33
  %109 = add i16 %108, 69
  %110 = add i16 %109, %107
  %111 = and i16 %110, 255
  %112 = xor i16 %111, %100
  %113 = zext nneg i16 %112 to i64
  %114 = getelementptr inbounds nuw i16, ptr @filter, i64 %113
  %115 = load i16, ptr %114, align 2, !tbaa !6
  %116 = freeze i16 %115
  store i16 %104, ptr %114, align 2, !tbaa !6
  %117 = icmp eq i16 %116, 0
  br i1 %117, label %139, label %118, !llvm.loop !12

118:                                              ; preds = %106
  %119 = lshr i16 %116, 8
  %120 = mul i16 %116, 33
  %121 = add i16 %120, 69
  %122 = add i16 %121, %119
  %123 = and i16 %122, 255
  %124 = xor i16 %123, %112
  %125 = zext nneg i16 %124 to i64
  %126 = getelementptr inbounds nuw i16, ptr @filter, i64 %125
  %127 = load i16, ptr %126, align 2, !tbaa !6
  %128 = freeze i16 %127
  store i16 %116, ptr %126, align 2, !tbaa !6
  %129 = icmp eq i16 %128, 0
  br i1 %129, label %139, label %130, !llvm.loop !12

130:                                              ; preds = %118
  %131 = lshr i16 %128, 8
  %132 = mul i16 %128, 33
  %133 = add i16 %132, 69
  %134 = add i16 %133, %131
  %135 = and i16 %134, 255
  %136 = xor i16 %135, %124
  %137 = zext nneg i16 %136 to i64
  %138 = getelementptr inbounds nuw i16, ptr @filter, i64 %137
  store i16 %128, ptr %138, align 2, !tbaa !6
  br label %139

139:                                              ; preds = %130, %118, %106, %94, %82, %70, %58, %35, %34, %28
  %140 = phi i16 [ %6, %28 ], [ %6, %34 ], [ %40, %130 ], [ %40, %118 ], [ %40, %106 ], [ %40, %94 ], [ %40, %82 ], [ %40, %70 ], [ %40, %58 ], [ %40, %35 ]
  call void @__checkpoint(ptr @checkpoint_name.2)
  %141 = add nuw nsw i32 %5, 1
  %142 = icmp eq i32 %141, 128
  br i1 %142, label %2, label %3, !llvm.loop !14

143:                                              ; preds = %176
  call void @llvm.lifetime.end.p0(ptr %1)
  ret i32 0

144:                                              ; preds = %176, %2
  %145 = phi i32 [ 0, %2 ], [ %177, %176 ]
  %146 = phi i16 [ 1, %2 ], [ %148, %176 ]
  %147 = mul i16 %146, 17
  %148 = add i16 %147, 17
  %149 = lshr i16 %148, 8
  %150 = and i16 %148, 255
  %151 = mul nuw nsw i16 %150, 33
  %152 = add nuw nsw i16 %149, 27205
  %153 = add nuw i16 %152, %151
  %154 = mul i16 %148, 33
  %155 = add i16 %154, 69
  %156 = add i16 %155, %149
  %157 = and i16 %156, 255
  %158 = zext nneg i16 %157 to i64
  %159 = getelementptr inbounds nuw i16, ptr @filter, i64 %158
  %160 = load i16, ptr %159, align 2, !tbaa !6
  %161 = icmp eq i16 %160, %153
  br i1 %161, label %173, label %162

162:                                              ; preds = %144
  %163 = mul i16 %153, 33
  %164 = add i16 %163, 69
  %165 = lshr i16 %153, 8
  %166 = add i16 %164, %165
  %167 = xor i16 %166, %156
  %168 = and i16 %167, 255
  %169 = zext nneg i16 %168 to i64
  %170 = getelementptr inbounds nuw i16, ptr @filter, i64 %169
  %171 = load i16, ptr %170, align 2, !tbaa !6
  %172 = icmp eq i16 %171, %153
  br i1 %172, label %173, label %176

173:                                              ; preds = %162, %144
  %174 = load volatile i32, ptr %1, align 4, !tbaa !10
  %175 = add i32 %174, 1
  store volatile i32 %175, ptr %1, align 4, !tbaa !10
  br label %176

176:                                              ; preds = %173, %162
  call void @__checkpoint(ptr @checkpoint_name.3)
  %177 = add nuw nsw i32 %145, 1
  %178 = icmp eq i32 %177, 128
  br i1 %178, label %143, label %144, !llvm.loop !15
}

; Function Attrs: nocallback nofree nounwind willreturn memory(argmem: write)
declare void @llvm.memset.p0.i64(ptr writeonly captures(none), i8, i64, i1 immarg) #2

declare void @llvm.lifetime.start.i64(i64)

; Function Attrs: nocallback nofree nosync nounwind willreturn memory(argmem: readwrite)
declare void @llvm.lifetime.start.p0(ptr captures(none)) #3

declare void @llvm.lifetime.end.i64(i64)

; Function Attrs: nocallback nofree nosync nounwind willreturn memory(argmem: readwrite)
declare void @llvm.lifetime.end.p0(ptr captures(none)) #3

declare void @__checkpoint(ptr)

attributes #0 = { mustprogress nofree norecurse nosync nounwind ssp willreturn memory(none) uwtable(sync) "frame-pointer"="non-leaf" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="apple-m1" "target-features"="+aes,+altnzcv,+ccdp,+ccidx,+ccpp,+complxnum,+crc,+dit,+dotprod,+flagm,+fp-armv8,+fp16fml,+fptoint,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+predres,+ras,+rcpc,+rdm,+sb,+sha2,+sha3,+specrestrict,+ssbs,+v8.1a,+v8.2a,+v8.3a,+v8.4a,+v8a" }
attributes #1 = { nofree norecurse nounwind ssp memory(readwrite, argmem: none) uwtable(sync) "frame-pointer"="non-leaf" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="apple-m1" "target-features"="+aes,+altnzcv,+ccdp,+ccidx,+ccpp,+complxnum,+crc,+dit,+dotprod,+flagm,+fp-armv8,+fp16fml,+fptoint,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+predres,+ras,+rcpc,+rdm,+sb,+sha2,+sha3,+specrestrict,+ssbs,+v8.1a,+v8.2a,+v8.3a,+v8.4a,+v8a" }
attributes #2 = { nocallback nofree nounwind willreturn memory(argmem: write) }
attributes #3 = { nocallback nofree nosync nounwind willreturn memory(argmem: readwrite) }

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
!12 = distinct !{!12, !13}
!13 = !{!"llvm.loop.mustprogress"}
!14 = distinct !{!14, !13}
!15 = distinct !{!15, !13}
