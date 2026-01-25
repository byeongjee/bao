; ModuleID = 'test/intermittent/rsa.ll'
source_filename = "test/intermittent/rsa.c"
target datalayout = "e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-n32:64-S128-Fn32"
target triple = "arm64-apple-macosx26.0.0"

%struct.pubkey_t = type { [16 x i8], i16 }

@g_cyphertext_len = local_unnamed_addr global i32 0, align 4
@P1DIR = internal global i8 0, align 1
@P4DIR = internal global i8 0, align 1
@PLAINTEXT = internal unnamed_addr constant [12 x i8] c".RRRSSSAAA.\00", align 1
@g_base = global [32 x i16] zeroinitializer, align 2
@g_block = global [32 x i16] zeroinitializer, align 2
@pubkey = internal unnamed_addr constant %struct.pubkey_t { [16 x i8] c"A\A1\BC\AC\A3.\A9\81\A9\B7]\D7e$R\EA", i16 3 }, align 2
@g_A = local_unnamed_addr global [16 x i16] zeroinitializer, align 2
@g_B = local_unnamed_addr global [16 x i16] zeroinitializer, align 2
@g_cyphertext = local_unnamed_addr global [16 x i16] zeroinitializer, align 2
@g_product = local_unnamed_addr global [32 x i16] zeroinitializer, align 2
@checkpoint_name = private unnamed_addr constant [4 x i8] c"bb3\00", align 1
@checkpoint_name.1 = private unnamed_addr constant [4 x i8] c"bb8\00", align 1
@checkpoint_name.2 = private unnamed_addr constant [5 x i8] c"bb12\00", align 1
@checkpoint_name.3 = private unnamed_addr constant [5 x i8] c"bb14\00", align 1
@checkpoint_name.4 = private unnamed_addr constant [5 x i8] c"bb18\00", align 1
@checkpoint_name.5 = private unnamed_addr constant [5 x i8] c"bb19\00", align 1
@checkpoint_name.6 = private unnamed_addr constant [4 x i8] c"bb4\00", align 1
@checkpoint_name.7 = private unnamed_addr constant [4 x i8] c"bb6\00", align 1
@checkpoint_name.8 = private unnamed_addr constant [5 x i8] c"bb16\00", align 1
@checkpoint_name.9 = private unnamed_addr constant [5 x i8] c"bb18\00", align 1
@checkpoint_name.10 = private unnamed_addr constant [5 x i8] c"bb23\00", align 1
@checkpoint_name.11 = private unnamed_addr constant [5 x i8] c"bb26\00", align 1
@checkpoint_name.12 = private unnamed_addr constant [5 x i8] c"bb31\00", align 1
@checkpoint_name.13 = private unnamed_addr constant [5 x i8] c"bb37\00", align 1
@checkpoint_name.14 = private unnamed_addr constant [5 x i8] c"bb39\00", align 1
@checkpoint_name.15 = private unnamed_addr constant [5 x i8] c"bb46\00", align 1
@checkpoint_name.16 = private unnamed_addr constant [5 x i8] c"bb47\00", align 1

; Function Attrs: nofree norecurse nounwind ssp uwtable(sync)
define noundef i32 @main() local_unnamed_addr #0 {
  %1 = alloca [8 x i8], align 1
  %2 = load volatile i8, ptr @P1DIR, align 1, !tbaa !6
  %3 = or i8 %2, 1
  store volatile i8 %3, ptr @P1DIR, align 1, !tbaa !6
  %4 = load volatile i8, ptr @P4DIR, align 1, !tbaa !6
  %5 = or i8 %4, 2
  store volatile i8 %5, ptr @P4DIR, align 1, !tbaa !6
  br label %6

6:                                                ; preds = %13, %0
  %7 = phi i64 [ 0, %0 ], [ %16, %13 ]
  %8 = icmp samesign ult i64 %7, 11
  br i1 %8, label %9, label %13

9:                                                ; preds = %6
  %10 = getelementptr inbounds nuw [12 x i8], ptr @PLAINTEXT, i64 0, i64 %7
  %11 = load i8, ptr %10, align 1, !tbaa !6
  %12 = zext i8 %11 to i16
  br label %13

13:                                               ; preds = %9, %6
  %14 = phi i16 [ %12, %9 ], [ 255, %6 ]
  call void @__checkpoint(ptr @checkpoint_name)
  %15 = getelementptr inbounds nuw [32 x i16], ptr @g_base, i64 0, i64 %7
  store i16 %14, ptr %15, align 2, !tbaa !9
  %16 = add nuw nsw i64 %7, 1
  %17 = icmp eq i64 %16, 15
  br i1 %17, label %18, label %6, !llvm.loop !11

18:                                               ; preds = %13
  store i16 1, ptr getelementptr inbounds nuw (i8, ptr @g_base, i64 30), align 2, !tbaa !9
  store i16 1, ptr @g_block, align 2, !tbaa !9
  tail call void @llvm.memset.p0.i64(ptr noundef nonnull align 2 dereferenceable(30) getelementptr inbounds nuw (i8, ptr @g_block, i64 2), i8 0, i64 30, i1 false), !tbaa !9
  br label %19

19:                                               ; preds = %27, %18
  %20 = phi i32 [ 3, %18 ], [ %23, %27 ]
  %21 = and i32 %20, 1
  %22 = icmp eq i32 %21, 0
  %23 = lshr i32 %20, 1
  br i1 %22, label %25, label %24

24:                                               ; preds = %19
  tail call void @llvm.memcpy.p0.p0.i64(ptr noundef nonnull align 2 dereferenceable(32) @g_A, ptr noundef nonnull align 2 dereferenceable(32) @g_base, i64 32, i1 false), !tbaa !9
  tail call void @llvm.memcpy.p0.p0.i64(ptr noundef nonnull align 2 dereferenceable(32) @g_B, ptr noundef nonnull align 2 dereferenceable(32) @g_block, i64 32, i1 false), !tbaa !9
  tail call fastcc void @mult_mod_operation(ptr noundef nonnull @g_block)
  br label %25

25:                                               ; preds = %24, %19
  %26 = icmp eq i32 %20, 1
  br i1 %26, label %29, label %27

27:                                               ; preds = %25
  call void @__checkpoint(ptr @checkpoint_name.1)
  tail call void @llvm.memcpy.p0.p0.i64(ptr noundef nonnull align 2 dereferenceable(32) @g_A, ptr noundef nonnull align 2 dereferenceable(32) @g_base, i64 32, i1 false), !tbaa !9
  tail call void @llvm.memcpy.p0.p0.i64(ptr noundef nonnull align 2 dereferenceable(32) @g_B, ptr noundef nonnull align 2 dereferenceable(32) @g_base, i64 32, i1 false), !tbaa !9
  tail call fastcc void @mult_mod_operation(ptr noundef nonnull @g_base)
  %28 = icmp eq i32 %20, 0
  br i1 %28, label %29, label %19, !llvm.loop !14

29:                                               ; preds = %27, %25
  %30 = load i32, ptr @g_cyphertext_len, align 4, !tbaa !15
  %31 = add i32 %30, 16
  %32 = icmp ult i32 %31, 17
  br i1 %32, label %33, label %47

33:                                               ; preds = %29
  %34 = icmp ugt i32 %30, -16
  br i1 %34, label %48, label %35

35:                                               ; preds = %33
  %36 = add i32 %30, 16
  br label %37

37:                                               ; preds = %37, %35
  %38 = phi i64 [ 0, %35 ], [ %45, %37 ]
  call void @__checkpoint(ptr @checkpoint_name.2)
  %39 = trunc i64 %38 to i32
  %40 = add i32 %30, %39
  %41 = getelementptr inbounds nuw [32 x i16], ptr @g_block, i64 0, i64 %38
  %42 = load <8 x i16>, ptr %41, align 2, !tbaa !9
  %43 = zext i32 %40 to i64
  %44 = getelementptr inbounds nuw [16 x i16], ptr @g_cyphertext, i64 0, i64 %43
  store <8 x i16> %42, ptr %44, align 2, !tbaa !9
  %45 = add nuw i64 %38, 8
  %46 = icmp eq i64 %45, 16
  br i1 %46, label %58, label %37, !llvm.loop !17

47:                                               ; preds = %29
  call void @llvm.lifetime.start.p0(ptr %1)
  br label %61

48:                                               ; preds = %48, %33
  %49 = phi i64 [ %56, %48 ], [ 0, %33 ]
  %50 = phi i32 [ %53, %48 ], [ %30, %33 ]
  call void @__checkpoint(ptr @checkpoint_name.3)
  %51 = getelementptr inbounds nuw [32 x i16], ptr @g_block, i64 0, i64 %49
  %52 = load i16, ptr %51, align 2, !tbaa !9
  %53 = add i32 %50, 1
  %54 = zext i32 %50 to i64
  %55 = getelementptr inbounds nuw [16 x i16], ptr @g_cyphertext, i64 0, i64 %54
  store i16 %52, ptr %55, align 2, !tbaa !9
  %56 = add nuw nsw i64 %49, 1
  %57 = icmp eq i64 %56, 16
  br i1 %57, label %58, label %48, !llvm.loop !20

58:                                               ; preds = %48, %37
  %59 = phi i32 [ %53, %48 ], [ %36, %37 ]
  store i32 %59, ptr @g_cyphertext_len, align 4, !tbaa !15
  call void @llvm.lifetime.start.p0(ptr %1)
  %60 = icmp eq i32 %59, 0
  br i1 %60, label %64, label %61

61:                                               ; preds = %58, %47
  %62 = phi i32 [ %30, %47 ], [ %59, %58 ]
  %63 = zext i32 %62 to i64
  br label %65

64:                                               ; preds = %83, %58
  call void @llvm.lifetime.end.p0(ptr %1)
  ret i32 0

65:                                               ; preds = %83, %61
  %66 = phi i64 [ 0, %61 ], [ %74, %83 ]
  %67 = phi i32 [ 0, %61 ], [ %84, %83 ]
  call void @__checkpoint(ptr @checkpoint_name.4)
  %68 = getelementptr inbounds nuw [16 x i16], ptr @g_cyphertext, i64 0, i64 %66
  %69 = load i16, ptr %68, align 2, !tbaa !9
  %70 = trunc i16 %69 to i8
  %71 = add nsw i32 %67, 1
  %72 = sext i32 %67 to i64
  %73 = getelementptr inbounds [8 x i8], ptr %1, i64 0, i64 %72
  store volatile i8 %70, ptr %73, align 1, !tbaa !6
  %74 = add nuw nsw i64 %66, 1
  %75 = and i64 %74, 7
  %76 = icmp eq i64 %75, 0
  br i1 %76, label %77, label %83

77:                                               ; preds = %77, %65
  %78 = phi i64 [ %81, %77 ], [ 0, %65 ]
  call void @__checkpoint(ptr @checkpoint_name.5)
  %79 = getelementptr inbounds nuw [8 x i8], ptr %1, i64 0, i64 %78
  %80 = load volatile i8, ptr %79, align 1, !tbaa !6
  %81 = add nuw nsw i64 %78, 1
  %82 = icmp eq i64 %81, 8
  br i1 %82, label %83, label %77, !llvm.loop !21

83:                                               ; preds = %77, %65
  %84 = phi i32 [ %71, %65 ], [ 0, %77 ]
  %85 = icmp eq i64 %74, %63
  br i1 %85, label %64, label %65, !llvm.loop !22
}

; Function Attrs: inlinehint nofree norecurse nosync nounwind ssp memory(readwrite, argmem: write, inaccessiblemem: none) uwtable(sync)
define internal fastcc void @mult_mod_operation(ptr noundef writeonly captures(none) %0) unnamed_addr #1 {
  %2 = ptrtoint ptr %0 to i64
  %3 = alloca [32 x i16], align 2
  tail call void @llvm.memset.p0.i64(ptr noundef nonnull align 2 dereferenceable(64) @g_product, i8 0, i64 64, i1 false), !tbaa !9
  br label %4

4:                                                ; preds = %32, %1
  %5 = phi i64 [ 0, %1 ], [ %37, %32 ]
  %6 = phi i16 [ 0, %1 ], [ %34, %32 ]
  br label %7

7:                                                ; preds = %27, %4
  %8 = phi i64 [ 0, %4 ], [ %30, %27 ]
  %9 = phi i16 [ 0, %4 ], [ %29, %27 ]
  %10 = phi i16 [ %6, %4 ], [ %28, %27 ]
  %11 = sub nsw i64 %5, %8
  %12 = icmp ult i64 %11, 16
  br i1 %12, label %13, label %27

13:                                               ; preds = %7
  %14 = getelementptr inbounds nuw i16, ptr @g_A, i64 %11
  %15 = load i16, ptr %14, align 2, !tbaa !9
  %16 = getelementptr inbounds nuw i16, ptr @g_B, i64 %8
  %17 = load i16, ptr %16, align 2, !tbaa !9
  %18 = zext i16 %15 to i32
  %19 = zext i16 %17 to i32
  %20 = mul nuw i32 %19, %18
  %21 = trunc i32 %20 to i16
  %22 = and i16 %21, 255
  %23 = add i16 %22, %10
  %24 = lshr i32 %20, 8
  %25 = trunc i32 %24 to i16
  %26 = add i16 %9, %25
  br label %27

27:                                               ; preds = %13, %7
  %28 = phi i16 [ %23, %13 ], [ %10, %7 ]
  %29 = phi i16 [ %26, %13 ], [ %9, %7 ]
  call void @__checkpoint(ptr @checkpoint_name.6)
  %30 = add nuw nsw i64 %8, 1
  %31 = icmp eq i64 %30, 16
  br i1 %31, label %32, label %7, !llvm.loop !23

32:                                               ; preds = %27
  %33 = lshr i16 %28, 8
  %34 = add i16 %33, %29
  %35 = and i16 %28, 255
  %36 = getelementptr inbounds nuw [32 x i16], ptr @g_product, i64 0, i64 %5
  store i16 %35, ptr %36, align 2, !tbaa !9
  %37 = add nuw nsw i64 %5, 1
  %38 = icmp eq i64 %37, 32
  br i1 %38, label %39, label %4, !llvm.loop !24

39:                                               ; preds = %39, %32
  %40 = phi i64 [ %41, %39 ], [ 32, %32 ]
  call void @__checkpoint(ptr @checkpoint_name.7)
  %41 = add nsw i64 %40, -1
  %42 = getelementptr inbounds nuw [32 x i16], ptr @g_product, i64 0, i64 %41
  %43 = load i16, ptr %42, align 2, !tbaa !9
  %44 = icmp eq i16 %43, 0
  %45 = icmp samesign ugt i64 %40, 1
  %46 = and i1 %45, %44
  br i1 %46, label %39, label %47, !llvm.loop !25

47:                                               ; preds = %39
  br i1 %44, label %50, label %48

48:                                               ; preds = %47
  %49 = trunc nuw nsw i64 %41 to i32
  br label %222

50:                                               ; preds = %47
  tail call void @llvm.memset.p0.i64(ptr noundef nonnull align 2 dereferenceable(32) %0, i8 0, i64 32, i1 false), !tbaa !9
  br label %245

51:                                               ; preds = %227, %222
  br i1 %225, label %56, label %52

52:                                               ; preds = %68, %53, %51
  br label %76

53:                                               ; preds = %72
  %54 = add nsw i32 %57, -1
  %55 = icmp sgt i32 %57, 0
  br i1 %55, label %56, label %52, !llvm.loop !26

56:                                               ; preds = %53, %51
  %57 = phi i32 [ %54, %53 ], [ %223, %51 ]
  %58 = zext nneg i32 %57 to i64
  %59 = getelementptr inbounds nuw [32 x i16], ptr @g_product, i64 0, i64 %58
  %60 = load i16, ptr %59, align 2, !tbaa !9
  %61 = sub i32 %57, %224
  %62 = icmp ult i32 %61, 16
  br i1 %62, label %63, label %68

63:                                               ; preds = %56
  %64 = zext nneg i32 %61 to i64
  %65 = getelementptr inbounds nuw [16 x i8], ptr @pubkey, i64 0, i64 %64
  %66 = load i8, ptr %65, align 1, !tbaa !6
  %67 = zext i8 %66 to i32
  br label %68

68:                                               ; preds = %63, %56
  %69 = phi i32 [ %67, %63 ], [ 0, %56 ]
  %70 = zext i16 %60 to i32
  %71 = icmp samesign ult i32 %69, %70
  br i1 %71, label %52, label %72

72:                                               ; preds = %68
  call void @__checkpoint(ptr @checkpoint_name.8)
  %73 = icmp samesign ugt i32 %69, %70
  br i1 %73, label %74, label %53

74:                                               ; preds = %72
  %75 = icmp eq i32 %223, 15
  br i1 %75, label %228, label %97

76:                                               ; preds = %76, %52
  %77 = phi i64 [ %95, %76 ], [ 0, %52 ]
  %78 = phi i32 [ %92, %76 ], [ 0, %52 ]
  call void @__checkpoint(ptr @checkpoint_name.9)
  %79 = trunc i64 %77 to i32
  %80 = add i32 %224, %79
  %81 = zext i32 %80 to i64
  %82 = getelementptr inbounds nuw [32 x i16], ptr @g_product, i64 0, i64 %81
  %83 = load i16, ptr %82, align 2, !tbaa !9
  %84 = getelementptr inbounds nuw [16 x i8], ptr @pubkey, i64 0, i64 %77
  %85 = load i8, ptr %84, align 1, !tbaa !6
  %86 = zext i8 %85 to i32
  %87 = add nuw nsw i32 %78, %86
  %88 = zext i16 %83 to i32
  %89 = icmp samesign ugt i32 %87, %88
  %90 = add i16 %83, 256
  %91 = select i1 %89, i16 %90, i16 %83
  %92 = zext i1 %89 to i32
  %93 = trunc nuw nsw i32 %87 to i16
  %94 = sub i16 %91, %93
  store i16 %94, ptr %82, align 2, !tbaa !9
  %95 = add nuw nsw i64 %77, 1
  %96 = icmp eq i64 %95, 16
  br i1 %96, label %227, label %76, !llvm.loop !27

97:                                               ; preds = %74
  %98 = zext nneg i32 %223 to i64
  %99 = getelementptr inbounds nuw [32 x i16], ptr @g_product, i64 0, i64 %98
  %100 = load i16, ptr %99, align 2, !tbaa !9
  %101 = add nsw i32 %223, -1
  %102 = sext i32 %101 to i64
  %103 = getelementptr inbounds [32 x i16], ptr @g_product, i64 0, i64 %102
  %104 = load i16, ptr %103, align 2, !tbaa !9
  %105 = add nsw i32 %223, -2
  %106 = sext i32 %105 to i64
  %107 = getelementptr inbounds [32 x i16], ptr @g_product, i64 0, i64 %106
  %108 = load i16, ptr %107, align 2, !tbaa !9
  %109 = zext i16 %100 to i32
  %110 = icmp eq i16 %100, 234
  br i1 %110, label %111, label %113

111:                                              ; preds = %97
  %112 = zext i16 %104 to i32
  br label %120

113:                                              ; preds = %97
  %114 = shl nuw nsw i32 %109, 8
  %115 = zext i16 %104 to i32
  %116 = add nuw nsw i32 %114, %115
  %117 = udiv i32 %116, 234
  %118 = trunc i32 %117 to i16
  %119 = add i16 %118, 1
  br label %120

120:                                              ; preds = %113, %111
  %121 = phi i32 [ %112, %111 ], [ %115, %113 ]
  %122 = phi i16 [ 256, %111 ], [ %119, %113 ]
  %123 = shl nuw i32 %109, 16
  %124 = shl nuw nsw i32 %121, 8
  %125 = add i32 %124, %123
  %126 = zext i16 %108 to i32
  %127 = add i32 %125, %126
  br label %128

128:                                              ; preds = %128, %120
  %129 = phi i16 [ %122, %120 ], [ %130, %128 ]
  call void @__checkpoint(ptr @checkpoint_name.10)
  %130 = add i16 %129, -1
  %131 = zext i16 %130 to i32
  %132 = mul nuw i32 %131, 59986
  %133 = icmp ugt i32 %132, %127
  br i1 %133, label %128, label %134, !llvm.loop !28

134:                                              ; preds = %128
  call void @llvm.lifetime.start.p0(ptr %3)
  call void @llvm.memset.p0.i64(ptr noundef nonnull align 2 dereferenceable(64) %3, i8 0, i64 64, i1 false), !tbaa !9
  %135 = add nsw i32 %223, -16
  %136 = icmp slt i32 %223, 48
  br i1 %136, label %137, label %160

137:                                              ; preds = %134
  %138 = sext i32 %135 to i64
  br label %139

139:                                              ; preds = %152, %137
  %140 = phi i64 [ %138, %137 ], [ %157, %152 ]
  %141 = phi i16 [ 0, %137 ], [ %154, %152 ]
  call void @__checkpoint(ptr @checkpoint_name.11)
  %142 = trunc nsw i64 %140 to i32
  %143 = icmp ugt i32 %223, %142
  br i1 %143, label %144, label %152

144:                                              ; preds = %139
  %145 = sub i32 %142, %135
  %146 = zext i32 %145 to i64
  %147 = getelementptr inbounds nuw [16 x i8], ptr @pubkey, i64 0, i64 %146
  %148 = load i8, ptr %147, align 1, !tbaa !6
  %149 = zext i8 %148 to i16
  %150 = mul i16 %130, %149
  %151 = add i16 %150, %141
  br label %152

152:                                              ; preds = %144, %139
  %153 = phi i16 [ %151, %144 ], [ %141, %139 ]
  %154 = lshr i16 %153, 8
  %155 = and i16 %153, 255
  %156 = getelementptr inbounds [32 x i16], ptr %3, i64 0, i64 %140
  store i16 %155, ptr %156, align 2, !tbaa !9
  %157 = add nsw i64 %140, 1
  %158 = and i64 %157, 4294967295
  %159 = icmp eq i64 %158, 32
  br i1 %159, label %160, label %139, !llvm.loop !29

160:                                              ; preds = %152, %134
  br label %164

161:                                              ; preds = %172
  %162 = add nsw i32 %165, -1
  %163 = icmp eq i32 %165, 0
  br i1 %163, label %199, label %164, !llvm.loop !30

164:                                              ; preds = %161, %160
  %165 = phi i32 [ %162, %161 ], [ 31, %160 ]
  call void @__checkpoint(ptr @checkpoint_name.12)
  %166 = zext nneg i32 %165 to i64
  %167 = getelementptr inbounds nuw [32 x i16], ptr @g_product, i64 0, i64 %166
  %168 = load i16, ptr %167, align 2, !tbaa !9
  %169 = getelementptr inbounds nuw [32 x i16], ptr %3, i64 0, i64 %166
  %170 = load i16, ptr %169, align 2, !tbaa !9
  %171 = icmp ugt i16 %168, %170
  br i1 %171, label %199, label %172

172:                                              ; preds = %164
  %173 = icmp ult i16 %168, %170
  br i1 %173, label %174, label %161

174:                                              ; preds = %172
  br i1 %136, label %175, label %199

175:                                              ; preds = %174
  %176 = sext i32 %135 to i64
  br label %177

177:                                              ; preds = %188, %175
  %178 = phi i64 [ %176, %175 ], [ %196, %188 ]
  %179 = phi i16 [ 0, %175 ], [ %194, %188 ]
  %180 = trunc nsw i64 %178 to i32
  %181 = icmp ugt i32 %223, %180
  br i1 %181, label %182, label %188

182:                                              ; preds = %177
  %183 = sub i32 %180, %135
  %184 = zext i32 %183 to i64
  %185 = getelementptr inbounds nuw [16 x i8], ptr @pubkey, i64 0, i64 %184
  %186 = load i8, ptr %185, align 1, !tbaa !6
  %187 = zext i8 %186 to i16
  br label %188

188:                                              ; preds = %182, %177
  %189 = phi i16 [ %187, %182 ], [ 0, %177 ]
  call void @__checkpoint(ptr @checkpoint_name.13)
  %190 = getelementptr inbounds [32 x i16], ptr @g_product, i64 0, i64 %178
  %191 = load i16, ptr %190, align 2, !tbaa !9
  %192 = add nuw nsw i16 %189, %179
  %193 = add i16 %192, %191
  %194 = lshr i16 %193, 8
  %195 = and i16 %193, 255
  store i16 %195, ptr %190, align 2, !tbaa !9
  %196 = add nsw i64 %178, 1
  %197 = and i64 %196, 4294967295
  %198 = icmp eq i64 %197, 32
  br i1 %198, label %199, label %177, !llvm.loop !31

199:                                              ; preds = %188, %174, %164, %161
  %200 = zext i32 %135 to i64
  br label %201

201:                                              ; preds = %217, %199
  %202 = phi i64 [ 0, %199 ], [ %219, %217 ]
  %203 = phi i32 [ 0, %199 ], [ %218, %217 ]
  call void @__checkpoint(ptr @checkpoint_name.14)
  %204 = icmp samesign ult i64 %202, %200
  br i1 %204, label %217, label %205

205:                                              ; preds = %201
  %206 = getelementptr inbounds nuw [32 x i16], ptr %3, i64 0, i64 %202
  %207 = load i16, ptr %206, align 2, !tbaa !9
  %208 = trunc nuw nsw i32 %203 to i16
  %209 = add i16 %207, %208
  %210 = getelementptr inbounds nuw [32 x i16], ptr @g_product, i64 0, i64 %202
  %211 = load i16, ptr %210, align 2, !tbaa !9
  %212 = icmp ult i16 %211, %209
  %213 = add i16 %211, 256
  %214 = select i1 %212, i16 %213, i16 %211
  %215 = zext i1 %212 to i32
  %216 = sub i16 %214, %209
  store i16 %216, ptr %210, align 2, !tbaa !9
  br label %217

217:                                              ; preds = %205, %201
  %218 = phi i32 [ %215, %205 ], [ %203, %201 ]
  %219 = add nuw nsw i64 %202, 1
  %220 = icmp eq i64 %219, 32
  br i1 %220, label %221, label %201, !llvm.loop !32

221:                                              ; preds = %217
  call void @llvm.lifetime.end.p0(ptr %3)
  br label %222

222:                                              ; preds = %221, %48
  %223 = phi i32 [ %101, %221 ], [ %49, %48 ]
  %224 = add nsw i32 %223, -15
  %225 = icmp sgt i32 %223, -1
  %226 = icmp eq i32 %224, 0
  br label %51

227:                                              ; preds = %76
  br i1 %226, label %228, label %51

228:                                              ; preds = %227, %74
  %229 = sub i64 %2, ptrtoint (ptr @g_product to i64)
  %230 = icmp ult i64 %229, 32
  br i1 %230, label %238, label %231

231:                                              ; preds = %231, %228
  %232 = phi i64 [ %236, %231 ], [ 0, %228 ]
  call void @__checkpoint(ptr @checkpoint_name.15)
  %233 = getelementptr inbounds nuw [32 x i16], ptr @g_product, i64 0, i64 %232
  %234 = load <8 x i16>, ptr %233, align 2, !tbaa !9
  %235 = getelementptr inbounds nuw i16, ptr %0, i64 %232
  store <8 x i16> %234, ptr %235, align 2, !tbaa !9
  %236 = add nuw i64 %232, 8
  %237 = icmp eq i64 %236, 16
  br i1 %237, label %245, label %231, !llvm.loop !33

238:                                              ; preds = %238, %228
  %239 = phi i64 [ %243, %238 ], [ 0, %228 ]
  call void @__checkpoint(ptr @checkpoint_name.16)
  %240 = getelementptr inbounds nuw [32 x i16], ptr @g_product, i64 0, i64 %239
  %241 = load i16, ptr %240, align 2, !tbaa !9
  %242 = getelementptr inbounds nuw i16, ptr %0, i64 %239
  store i16 %241, ptr %242, align 2, !tbaa !9
  %243 = add nuw nsw i64 %239, 1
  %244 = icmp eq i64 %243, 16
  br i1 %244, label %245, label %238, !llvm.loop !34

245:                                              ; preds = %238, %231, %50
  ret void
}

; Function Attrs: nocallback nofree nounwind willreturn memory(argmem: write)
declare void @llvm.memset.p0.i64(ptr writeonly captures(none), i8, i64, i1 immarg) #2

; Function Attrs: nocallback nofree nounwind willreturn memory(argmem: readwrite)
declare void @llvm.memcpy.p0.p0.i64(ptr noalias writeonly captures(none), ptr noalias readonly captures(none), i64, i1 immarg) #3

declare void @llvm.lifetime.start.i64(i64)

; Function Attrs: nocallback nofree nosync nounwind willreturn memory(argmem: readwrite)
declare void @llvm.lifetime.start.p0(ptr captures(none)) #4

declare void @llvm.lifetime.end.i64(i64)

; Function Attrs: nocallback nofree nosync nounwind willreturn memory(argmem: readwrite)
declare void @llvm.lifetime.end.p0(ptr captures(none)) #4

declare void @__checkpoint(ptr)

attributes #0 = { nofree norecurse nounwind ssp uwtable(sync) "frame-pointer"="non-leaf" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="apple-m1" "target-features"="+aes,+altnzcv,+ccdp,+ccidx,+ccpp,+complxnum,+crc,+dit,+dotprod,+flagm,+fp-armv8,+fp16fml,+fptoint,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+predres,+ras,+rcpc,+rdm,+sb,+sha2,+sha3,+specrestrict,+ssbs,+v8.1a,+v8.2a,+v8.3a,+v8.4a,+v8a" }
attributes #1 = { inlinehint nofree norecurse nosync nounwind ssp memory(readwrite, argmem: write, inaccessiblemem: none) uwtable(sync) "frame-pointer"="non-leaf" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="apple-m1" "target-features"="+aes,+altnzcv,+ccdp,+ccidx,+ccpp,+complxnum,+crc,+dit,+dotprod,+flagm,+fp-armv8,+fp16fml,+fptoint,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+predres,+ras,+rcpc,+rdm,+sb,+sha2,+sha3,+specrestrict,+ssbs,+v8.1a,+v8.2a,+v8.3a,+v8.4a,+v8a" }
attributes #2 = { nocallback nofree nounwind willreturn memory(argmem: write) }
attributes #3 = { nocallback nofree nounwind willreturn memory(argmem: readwrite) }
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
!7 = !{!"omnipotent char", !8, i64 0}
!8 = !{!"Simple C/C++ TBAA"}
!9 = !{!10, !10, i64 0}
!10 = !{!"short", !7, i64 0}
!11 = distinct !{!11, !12, !13}
!12 = !{!"llvm.loop.mustprogress"}
!13 = !{!"llvm.loop.unroll.disable"}
!14 = distinct !{!14, !12, !13}
!15 = !{!16, !16, i64 0}
!16 = !{!"int", !7, i64 0}
!17 = distinct !{!17, !12, !13, !18, !19}
!18 = !{!"llvm.loop.isvectorized", i32 1}
!19 = !{!"llvm.loop.unroll.runtime.disable"}
!20 = distinct !{!20, !12, !13, !18}
!21 = distinct !{!21, !12, !13}
!22 = distinct !{!22, !12, !13}
!23 = distinct !{!23, !12, !13}
!24 = distinct !{!24, !12, !13}
!25 = distinct !{!25, !12, !13}
!26 = distinct !{!26, !12, !13}
!27 = distinct !{!27, !12, !13}
!28 = distinct !{!28, !12, !13}
!29 = distinct !{!29, !12, !13}
!30 = distinct !{!30, !12, !13}
!31 = distinct !{!31, !12, !13}
!32 = distinct !{!32, !12, !13}
!33 = distinct !{!33, !12, !13, !18, !19}
!34 = distinct !{!34, !12, !13, !18}
