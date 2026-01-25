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
  %1 = alloca i32, align 4
  tail call void @llvm.memset.p0.i64(ptr noundef nonnull align 2 dereferenceable(512) @filter, i8 0, i64 512, i1 false), !tbaa !6
  br label %3

2:                                                ; preds = %393
  call void @llvm.lifetime.start.p0(i64 4, ptr nonnull %1)
  store volatile i32 0, ptr %1, align 4, !tbaa !10
  br label %398

3:                                                ; preds = %393, %0
  %4 = phi i16 [ 1, %0 ], [ %299, %393 ]
  %5 = phi i32 [ 0, %0 ], [ %395, %393 ]
  %6 = phi i16 [ -21279, %0 ], [ %394, %393 ]
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
  br label %102

29:                                               ; preds = %3
  %30 = zext nneg i16 %23 to i64
  %31 = getelementptr inbounds nuw i16, ptr @filter, i64 %30
  %32 = load i16, ptr %31, align 2, !tbaa !6
  %33 = icmp eq i16 %32, 0
  br i1 %33, label %34, label %35

34:                                               ; preds = %29
  store i16 %13, ptr %31, align 2, !tbaa !6
  br label %102

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
  br label %47

47:                                               ; preds = %86, %35
  %48 = phi i32 [ 0, %35 ], [ %98, %86 ]
  %49 = phi i16 [ %43, %35 ], [ %93, %86 ]
  %50 = phi i16 [ %46, %35 ], [ %97, %86 ]
  %51 = lshr i16 %50, 8
  %52 = mul i16 %50, 33
  %53 = add i16 %52, 69
  %54 = add i16 %53, %51
  %55 = and i16 %54, 255
  %56 = xor i16 %55, %49
  %57 = zext nneg i16 %56 to i64
  %58 = getelementptr inbounds nuw i16, ptr @filter, i64 %57
  %59 = load i16, ptr %58, align 2, !tbaa !6
  %60 = freeze i16 %59
  store i16 %50, ptr %58, align 2, !tbaa !6
  %61 = icmp eq i16 %60, 0
  br i1 %61, label %102, label %62, !llvm.loop !12

62:                                               ; preds = %47
  %63 = lshr i16 %60, 8
  %64 = mul i16 %60, 33
  %65 = add i16 %64, 69
  %66 = add i16 %65, %63
  %67 = and i16 %66, 255
  %68 = xor i16 %67, %56
  %69 = zext nneg i16 %68 to i64
  %70 = getelementptr inbounds nuw i16, ptr @filter, i64 %69
  %71 = load i16, ptr %70, align 2, !tbaa !6
  %72 = freeze i16 %71
  store i16 %60, ptr %70, align 2, !tbaa !6
  %73 = icmp eq i16 %72, 0
  br i1 %73, label %102, label %74, !llvm.loop !12

74:                                               ; preds = %62
  %75 = lshr i16 %72, 8
  %76 = mul i16 %72, 33
  %77 = add i16 %76, 69
  %78 = add i16 %77, %75
  %79 = and i16 %78, 255
  %80 = xor i16 %79, %68
  %81 = zext nneg i16 %80 to i64
  %82 = getelementptr inbounds nuw i16, ptr @filter, i64 %81
  %83 = load i16, ptr %82, align 2, !tbaa !6
  %84 = freeze i16 %83
  store i16 %72, ptr %82, align 2, !tbaa !6
  %85 = icmp eq i16 %84, 0
  br i1 %85, label %102, label %86, !llvm.loop !12

86:                                               ; preds = %74
  %87 = or disjoint i32 %48, 3
  %88 = lshr i16 %84, 8
  %89 = mul i16 %84, 33
  %90 = add i16 %89, 69
  %91 = add i16 %90, %88
  %92 = and i16 %91, 255
  %93 = xor i16 %92, %80
  %94 = zext nneg i16 %93 to i64
  %95 = getelementptr inbounds nuw i16, ptr @filter, i64 %94
  %96 = load i16, ptr %95, align 2, !tbaa !6
  %97 = freeze i16 %96
  store i16 %84, ptr %95, align 2, !tbaa !6
  %98 = add nuw nsw i32 %48, 4
  %99 = icmp ne i16 %97, 0
  %100 = icmp samesign ult i32 %87, 7
  %101 = select i1 %99, i1 %100, i1 false
  br i1 %101, label %47, label %102, !llvm.loop !14

102:                                              ; preds = %47, %62, %74, %86, %34, %28
  %103 = phi i16 [ %6, %28 ], [ %6, %34 ], [ %40, %86 ], [ %40, %74 ], [ %40, %62 ], [ %40, %47 ]
  %104 = mul i16 %8, 17
  %105 = add i16 %104, 17
  %106 = lshr i16 %105, 8
  %107 = and i16 %105, 255
  %108 = mul nuw nsw i16 %107, 33
  %109 = add nuw nsw i16 %106, 27205
  %110 = add nuw i16 %109, %108
  %111 = mul i16 %105, 33
  %112 = add i16 %111, 69
  %113 = add i16 %112, %106
  %114 = and i16 %113, 255
  %115 = lshr i16 %110, 8
  %116 = mul i16 %110, 33
  %117 = add i16 %116, 69
  %118 = add i16 %117, %115
  %119 = xor i16 %118, %113
  %120 = and i16 %119, 255
  %121 = zext nneg i16 %114 to i64
  %122 = getelementptr inbounds nuw i16, ptr @filter, i64 %121
  %123 = load i16, ptr %122, align 2, !tbaa !6
  %124 = icmp eq i16 %123, 0
  br i1 %124, label %198, label %125

125:                                              ; preds = %102
  %126 = zext nneg i16 %120 to i64
  %127 = getelementptr inbounds nuw i16, ptr @filter, i64 %126
  %128 = load i16, ptr %127, align 2, !tbaa !6
  %129 = icmp eq i16 %128, 0
  br i1 %129, label %197, label %130

130:                                              ; preds = %125
  %131 = and i16 %103, 1
  %132 = icmp eq i16 %131, 0
  %133 = lshr i16 %103, 1
  %134 = xor i16 %133, -19456
  %135 = select i1 %132, i16 %133, i16 %134
  %136 = and i16 %135, 128
  %137 = icmp eq i16 %136, 0
  %138 = select i1 %137, i16 %120, i16 %114
  %139 = zext nneg i16 %138 to i64
  %140 = getelementptr inbounds nuw i16, ptr @filter, i64 %139
  %141 = load i16, ptr %140, align 2, !tbaa !6
  store i16 %110, ptr %140, align 2, !tbaa !6
  br label %142

142:                                              ; preds = %181, %130
  %143 = phi i32 [ 0, %130 ], [ %193, %181 ]
  %144 = phi i16 [ %138, %130 ], [ %188, %181 ]
  %145 = phi i16 [ %141, %130 ], [ %192, %181 ]
  %146 = lshr i16 %145, 8
  %147 = mul i16 %145, 33
  %148 = add i16 %147, 69
  %149 = add i16 %148, %146
  %150 = and i16 %149, 255
  %151 = xor i16 %150, %144
  %152 = zext nneg i16 %151 to i64
  %153 = getelementptr inbounds nuw i16, ptr @filter, i64 %152
  %154 = load i16, ptr %153, align 2, !tbaa !6
  %155 = freeze i16 %154
  store i16 %145, ptr %153, align 2, !tbaa !6
  %156 = icmp eq i16 %155, 0
  br i1 %156, label %199, label %157, !llvm.loop !12

157:                                              ; preds = %142
  %158 = lshr i16 %155, 8
  %159 = mul i16 %155, 33
  %160 = add i16 %159, 69
  %161 = add i16 %160, %158
  %162 = and i16 %161, 255
  %163 = xor i16 %162, %151
  %164 = zext nneg i16 %163 to i64
  %165 = getelementptr inbounds nuw i16, ptr @filter, i64 %164
  %166 = load i16, ptr %165, align 2, !tbaa !6
  %167 = freeze i16 %166
  store i16 %155, ptr %165, align 2, !tbaa !6
  %168 = icmp eq i16 %167, 0
  br i1 %168, label %199, label %169, !llvm.loop !12

169:                                              ; preds = %157
  %170 = lshr i16 %167, 8
  %171 = mul i16 %167, 33
  %172 = add i16 %171, 69
  %173 = add i16 %172, %170
  %174 = and i16 %173, 255
  %175 = xor i16 %174, %163
  %176 = zext nneg i16 %175 to i64
  %177 = getelementptr inbounds nuw i16, ptr @filter, i64 %176
  %178 = load i16, ptr %177, align 2, !tbaa !6
  %179 = freeze i16 %178
  store i16 %167, ptr %177, align 2, !tbaa !6
  %180 = icmp eq i16 %179, 0
  br i1 %180, label %199, label %181, !llvm.loop !12

181:                                              ; preds = %169
  %182 = or disjoint i32 %143, 3
  %183 = lshr i16 %179, 8
  %184 = mul i16 %179, 33
  %185 = add i16 %184, 69
  %186 = add i16 %185, %183
  %187 = and i16 %186, 255
  %188 = xor i16 %187, %175
  %189 = zext nneg i16 %188 to i64
  %190 = getelementptr inbounds nuw i16, ptr @filter, i64 %189
  %191 = load i16, ptr %190, align 2, !tbaa !6
  %192 = freeze i16 %191
  store i16 %179, ptr %190, align 2, !tbaa !6
  %193 = add nuw nsw i32 %143, 4
  %194 = icmp ne i16 %192, 0
  %195 = icmp samesign ult i32 %182, 7
  %196 = select i1 %194, i1 %195, i1 false
  br i1 %196, label %142, label %199, !llvm.loop !14

197:                                              ; preds = %125
  store i16 %110, ptr %127, align 2, !tbaa !6
  br label %199

198:                                              ; preds = %102
  store i16 %110, ptr %122, align 2, !tbaa !6
  br label %199

199:                                              ; preds = %142, %157, %169, %181, %198, %197
  %200 = phi i16 [ %103, %198 ], [ %103, %197 ], [ %135, %181 ], [ %135, %169 ], [ %135, %157 ], [ %135, %142 ]
  %201 = mul i16 %105, 17
  %202 = add i16 %201, 17
  %203 = lshr i16 %202, 8
  %204 = and i16 %202, 255
  %205 = mul nuw nsw i16 %204, 33
  %206 = add nuw nsw i16 %203, 27205
  %207 = add nuw i16 %206, %205
  %208 = mul i16 %202, 33
  %209 = add i16 %208, 69
  %210 = add i16 %209, %203
  %211 = and i16 %210, 255
  %212 = lshr i16 %207, 8
  %213 = mul i16 %207, 33
  %214 = add i16 %213, 69
  %215 = add i16 %214, %212
  %216 = xor i16 %215, %210
  %217 = and i16 %216, 255
  %218 = zext nneg i16 %211 to i64
  %219 = getelementptr inbounds nuw i16, ptr @filter, i64 %218
  %220 = load i16, ptr %219, align 2, !tbaa !6
  %221 = icmp eq i16 %220, 0
  br i1 %221, label %295, label %222

222:                                              ; preds = %199
  %223 = zext nneg i16 %217 to i64
  %224 = getelementptr inbounds nuw i16, ptr @filter, i64 %223
  %225 = load i16, ptr %224, align 2, !tbaa !6
  %226 = icmp eq i16 %225, 0
  br i1 %226, label %294, label %227

227:                                              ; preds = %222
  %228 = and i16 %200, 1
  %229 = icmp eq i16 %228, 0
  %230 = lshr i16 %200, 1
  %231 = xor i16 %230, -19456
  %232 = select i1 %229, i16 %230, i16 %231
  %233 = and i16 %232, 128
  %234 = icmp eq i16 %233, 0
  %235 = select i1 %234, i16 %217, i16 %211
  %236 = zext nneg i16 %235 to i64
  %237 = getelementptr inbounds nuw i16, ptr @filter, i64 %236
  %238 = load i16, ptr %237, align 2, !tbaa !6
  store i16 %207, ptr %237, align 2, !tbaa !6
  br label %239

239:                                              ; preds = %278, %227
  %240 = phi i32 [ 0, %227 ], [ %290, %278 ]
  %241 = phi i16 [ %235, %227 ], [ %285, %278 ]
  %242 = phi i16 [ %238, %227 ], [ %289, %278 ]
  %243 = lshr i16 %242, 8
  %244 = mul i16 %242, 33
  %245 = add i16 %244, 69
  %246 = add i16 %245, %243
  %247 = and i16 %246, 255
  %248 = xor i16 %247, %241
  %249 = zext nneg i16 %248 to i64
  %250 = getelementptr inbounds nuw i16, ptr @filter, i64 %249
  %251 = load i16, ptr %250, align 2, !tbaa !6
  %252 = freeze i16 %251
  store i16 %242, ptr %250, align 2, !tbaa !6
  %253 = icmp eq i16 %252, 0
  br i1 %253, label %296, label %254, !llvm.loop !12

254:                                              ; preds = %239
  %255 = lshr i16 %252, 8
  %256 = mul i16 %252, 33
  %257 = add i16 %256, 69
  %258 = add i16 %257, %255
  %259 = and i16 %258, 255
  %260 = xor i16 %259, %248
  %261 = zext nneg i16 %260 to i64
  %262 = getelementptr inbounds nuw i16, ptr @filter, i64 %261
  %263 = load i16, ptr %262, align 2, !tbaa !6
  %264 = freeze i16 %263
  store i16 %252, ptr %262, align 2, !tbaa !6
  %265 = icmp eq i16 %264, 0
  br i1 %265, label %296, label %266, !llvm.loop !12

266:                                              ; preds = %254
  %267 = lshr i16 %264, 8
  %268 = mul i16 %264, 33
  %269 = add i16 %268, 69
  %270 = add i16 %269, %267
  %271 = and i16 %270, 255
  %272 = xor i16 %271, %260
  %273 = zext nneg i16 %272 to i64
  %274 = getelementptr inbounds nuw i16, ptr @filter, i64 %273
  %275 = load i16, ptr %274, align 2, !tbaa !6
  %276 = freeze i16 %275
  store i16 %264, ptr %274, align 2, !tbaa !6
  %277 = icmp eq i16 %276, 0
  br i1 %277, label %296, label %278, !llvm.loop !12

278:                                              ; preds = %266
  %279 = or disjoint i32 %240, 3
  %280 = lshr i16 %276, 8
  %281 = mul i16 %276, 33
  %282 = add i16 %281, 69
  %283 = add i16 %282, %280
  %284 = and i16 %283, 255
  %285 = xor i16 %284, %272
  %286 = zext nneg i16 %285 to i64
  %287 = getelementptr inbounds nuw i16, ptr @filter, i64 %286
  %288 = load i16, ptr %287, align 2, !tbaa !6
  %289 = freeze i16 %288
  store i16 %276, ptr %287, align 2, !tbaa !6
  %290 = add nuw nsw i32 %240, 4
  %291 = icmp ne i16 %289, 0
  %292 = icmp samesign ult i32 %279, 7
  %293 = select i1 %291, i1 %292, i1 false
  br i1 %293, label %239, label %296, !llvm.loop !14

294:                                              ; preds = %222
  store i16 %207, ptr %224, align 2, !tbaa !6
  br label %296

295:                                              ; preds = %199
  store i16 %207, ptr %219, align 2, !tbaa !6
  br label %296

296:                                              ; preds = %239, %254, %266, %278, %295, %294
  %297 = phi i16 [ %200, %295 ], [ %200, %294 ], [ %232, %278 ], [ %232, %266 ], [ %232, %254 ], [ %232, %239 ]
  %298 = mul i16 %202, 17
  %299 = add i16 %298, 17
  %300 = lshr i16 %299, 8
  %301 = and i16 %299, 255
  %302 = mul nuw nsw i16 %301, 33
  %303 = add nuw nsw i16 %300, 27205
  %304 = add nuw i16 %303, %302
  %305 = mul i16 %299, 33
  %306 = add i16 %305, 69
  %307 = add i16 %306, %300
  %308 = and i16 %307, 255
  %309 = lshr i16 %304, 8
  %310 = mul i16 %304, 33
  %311 = add i16 %310, 69
  %312 = add i16 %311, %309
  %313 = xor i16 %312, %307
  %314 = and i16 %313, 255
  %315 = zext nneg i16 %308 to i64
  %316 = getelementptr inbounds nuw i16, ptr @filter, i64 %315
  %317 = load i16, ptr %316, align 2, !tbaa !6
  %318 = icmp eq i16 %317, 0
  br i1 %318, label %392, label %319

319:                                              ; preds = %296
  %320 = zext nneg i16 %314 to i64
  %321 = getelementptr inbounds nuw i16, ptr @filter, i64 %320
  %322 = load i16, ptr %321, align 2, !tbaa !6
  %323 = icmp eq i16 %322, 0
  br i1 %323, label %391, label %324

324:                                              ; preds = %319
  %325 = and i16 %297, 1
  %326 = icmp eq i16 %325, 0
  %327 = lshr i16 %297, 1
  %328 = xor i16 %327, -19456
  %329 = select i1 %326, i16 %327, i16 %328
  %330 = and i16 %329, 128
  %331 = icmp eq i16 %330, 0
  %332 = select i1 %331, i16 %314, i16 %308
  %333 = zext nneg i16 %332 to i64
  %334 = getelementptr inbounds nuw i16, ptr @filter, i64 %333
  %335 = load i16, ptr %334, align 2, !tbaa !6
  store i16 %304, ptr %334, align 2, !tbaa !6
  br label %336

336:                                              ; preds = %375, %324
  %337 = phi i32 [ 0, %324 ], [ %387, %375 ]
  %338 = phi i16 [ %332, %324 ], [ %382, %375 ]
  %339 = phi i16 [ %335, %324 ], [ %386, %375 ]
  %340 = lshr i16 %339, 8
  %341 = mul i16 %339, 33
  %342 = add i16 %341, 69
  %343 = add i16 %342, %340
  %344 = and i16 %343, 255
  %345 = xor i16 %344, %338
  %346 = zext nneg i16 %345 to i64
  %347 = getelementptr inbounds nuw i16, ptr @filter, i64 %346
  %348 = load i16, ptr %347, align 2, !tbaa !6
  %349 = freeze i16 %348
  store i16 %339, ptr %347, align 2, !tbaa !6
  %350 = icmp eq i16 %349, 0
  br i1 %350, label %393, label %351, !llvm.loop !12

351:                                              ; preds = %336
  %352 = lshr i16 %349, 8
  %353 = mul i16 %349, 33
  %354 = add i16 %353, 69
  %355 = add i16 %354, %352
  %356 = and i16 %355, 255
  %357 = xor i16 %356, %345
  %358 = zext nneg i16 %357 to i64
  %359 = getelementptr inbounds nuw i16, ptr @filter, i64 %358
  %360 = load i16, ptr %359, align 2, !tbaa !6
  %361 = freeze i16 %360
  store i16 %349, ptr %359, align 2, !tbaa !6
  %362 = icmp eq i16 %361, 0
  br i1 %362, label %393, label %363, !llvm.loop !12

363:                                              ; preds = %351
  %364 = lshr i16 %361, 8
  %365 = mul i16 %361, 33
  %366 = add i16 %365, 69
  %367 = add i16 %366, %364
  %368 = and i16 %367, 255
  %369 = xor i16 %368, %357
  %370 = zext nneg i16 %369 to i64
  %371 = getelementptr inbounds nuw i16, ptr @filter, i64 %370
  %372 = load i16, ptr %371, align 2, !tbaa !6
  %373 = freeze i16 %372
  store i16 %361, ptr %371, align 2, !tbaa !6
  %374 = icmp eq i16 %373, 0
  br i1 %374, label %393, label %375, !llvm.loop !12

375:                                              ; preds = %363
  %376 = or disjoint i32 %337, 3
  %377 = lshr i16 %373, 8
  %378 = mul i16 %373, 33
  %379 = add i16 %378, 69
  %380 = add i16 %379, %377
  %381 = and i16 %380, 255
  %382 = xor i16 %381, %369
  %383 = zext nneg i16 %382 to i64
  %384 = getelementptr inbounds nuw i16, ptr @filter, i64 %383
  %385 = load i16, ptr %384, align 2, !tbaa !6
  %386 = freeze i16 %385
  store i16 %373, ptr %384, align 2, !tbaa !6
  %387 = add nuw nsw i32 %337, 4
  %388 = icmp ne i16 %386, 0
  %389 = icmp samesign ult i32 %376, 7
  %390 = select i1 %388, i1 %389, i1 false
  br i1 %390, label %336, label %393, !llvm.loop !14

391:                                              ; preds = %319
  store i16 %304, ptr %321, align 2, !tbaa !6
  br label %393

392:                                              ; preds = %296
  store i16 %304, ptr %316, align 2, !tbaa !6
  br label %393

393:                                              ; preds = %336, %351, %363, %375, %392, %391
  %394 = phi i16 [ %297, %392 ], [ %297, %391 ], [ %329, %375 ], [ %329, %363 ], [ %329, %351 ], [ %329, %336 ]
  %395 = add nuw nsw i32 %5, 4
  %396 = icmp eq i32 %395, 128
  br i1 %396, label %2, label %3, !llvm.loop !16

397:                                              ; preds = %520
  call void @llvm.lifetime.end.p0(i64 4, ptr nonnull %1)
  ret i32 0

398:                                              ; preds = %520, %2
  %399 = phi i32 [ 0, %2 ], [ %521, %520 ]
  %400 = phi i16 [ 1, %2 ], [ %492, %520 ]
  %401 = mul i16 %400, 17
  %402 = add i16 %401, 17
  %403 = lshr i16 %402, 8
  %404 = and i16 %402, 255
  %405 = mul nuw nsw i16 %404, 33
  %406 = add nuw nsw i16 %403, 27205
  %407 = add nuw i16 %406, %405
  %408 = mul i16 %402, 33
  %409 = add i16 %408, 69
  %410 = add i16 %409, %403
  %411 = and i16 %410, 255
  %412 = zext nneg i16 %411 to i64
  %413 = getelementptr inbounds nuw i16, ptr @filter, i64 %412
  %414 = load i16, ptr %413, align 2, !tbaa !6
  %415 = icmp eq i16 %414, %407
  br i1 %415, label %427, label %416

416:                                              ; preds = %398
  %417 = mul i16 %407, 33
  %418 = add i16 %417, 69
  %419 = lshr i16 %407, 8
  %420 = add i16 %418, %419
  %421 = xor i16 %420, %410
  %422 = and i16 %421, 255
  %423 = zext nneg i16 %422 to i64
  %424 = getelementptr inbounds nuw i16, ptr @filter, i64 %423
  %425 = load i16, ptr %424, align 2, !tbaa !6
  %426 = icmp eq i16 %425, %407
  br i1 %426, label %427, label %430

427:                                              ; preds = %398, %416
  %428 = load volatile i32, ptr %1, align 4, !tbaa !10
  %429 = add i32 %428, 1
  store volatile i32 %429, ptr %1, align 4, !tbaa !10
  br label %430

430:                                              ; preds = %416, %427
  %431 = mul i16 %402, 17
  %432 = add i16 %431, 17
  %433 = lshr i16 %432, 8
  %434 = and i16 %432, 255
  %435 = mul nuw nsw i16 %434, 33
  %436 = add nuw nsw i16 %433, 27205
  %437 = add nuw i16 %436, %435
  %438 = mul i16 %432, 33
  %439 = add i16 %438, 69
  %440 = add i16 %439, %433
  %441 = and i16 %440, 255
  %442 = zext nneg i16 %441 to i64
  %443 = getelementptr inbounds nuw i16, ptr @filter, i64 %442
  %444 = load i16, ptr %443, align 2, !tbaa !6
  %445 = icmp eq i16 %444, %437
  br i1 %445, label %457, label %446

446:                                              ; preds = %430
  %447 = mul i16 %437, 33
  %448 = add i16 %447, 69
  %449 = lshr i16 %437, 8
  %450 = add i16 %448, %449
  %451 = xor i16 %450, %440
  %452 = and i16 %451, 255
  %453 = zext nneg i16 %452 to i64
  %454 = getelementptr inbounds nuw i16, ptr @filter, i64 %453
  %455 = load i16, ptr %454, align 2, !tbaa !6
  %456 = icmp eq i16 %455, %437
  br i1 %456, label %457, label %460

457:                                              ; preds = %446, %430
  %458 = load volatile i32, ptr %1, align 4, !tbaa !10
  %459 = add i32 %458, 1
  store volatile i32 %459, ptr %1, align 4, !tbaa !10
  br label %460

460:                                              ; preds = %457, %446
  %461 = mul i16 %432, 17
  %462 = add i16 %461, 17
  %463 = lshr i16 %462, 8
  %464 = and i16 %462, 255
  %465 = mul nuw nsw i16 %464, 33
  %466 = add nuw nsw i16 %463, 27205
  %467 = add nuw i16 %466, %465
  %468 = mul i16 %462, 33
  %469 = add i16 %468, 69
  %470 = add i16 %469, %463
  %471 = and i16 %470, 255
  %472 = zext nneg i16 %471 to i64
  %473 = getelementptr inbounds nuw i16, ptr @filter, i64 %472
  %474 = load i16, ptr %473, align 2, !tbaa !6
  %475 = icmp eq i16 %474, %467
  br i1 %475, label %487, label %476

476:                                              ; preds = %460
  %477 = mul i16 %467, 33
  %478 = add i16 %477, 69
  %479 = lshr i16 %467, 8
  %480 = add i16 %478, %479
  %481 = xor i16 %480, %470
  %482 = and i16 %481, 255
  %483 = zext nneg i16 %482 to i64
  %484 = getelementptr inbounds nuw i16, ptr @filter, i64 %483
  %485 = load i16, ptr %484, align 2, !tbaa !6
  %486 = icmp eq i16 %485, %467
  br i1 %486, label %487, label %490

487:                                              ; preds = %476, %460
  %488 = load volatile i32, ptr %1, align 4, !tbaa !10
  %489 = add i32 %488, 1
  store volatile i32 %489, ptr %1, align 4, !tbaa !10
  br label %490

490:                                              ; preds = %487, %476
  %491 = mul i16 %462, 17
  %492 = add i16 %491, 17
  %493 = lshr i16 %492, 8
  %494 = and i16 %492, 255
  %495 = mul nuw nsw i16 %494, 33
  %496 = add nuw nsw i16 %493, 27205
  %497 = add nuw i16 %496, %495
  %498 = mul i16 %492, 33
  %499 = add i16 %498, 69
  %500 = add i16 %499, %493
  %501 = and i16 %500, 255
  %502 = zext nneg i16 %501 to i64
  %503 = getelementptr inbounds nuw i16, ptr @filter, i64 %502
  %504 = load i16, ptr %503, align 2, !tbaa !6
  %505 = icmp eq i16 %504, %497
  br i1 %505, label %517, label %506

506:                                              ; preds = %490
  %507 = mul i16 %497, 33
  %508 = add i16 %507, 69
  %509 = lshr i16 %497, 8
  %510 = add i16 %508, %509
  %511 = xor i16 %510, %500
  %512 = and i16 %511, 255
  %513 = zext nneg i16 %512 to i64
  %514 = getelementptr inbounds nuw i16, ptr @filter, i64 %513
  %515 = load i16, ptr %514, align 2, !tbaa !6
  %516 = icmp eq i16 %515, %497
  br i1 %516, label %517, label %520

517:                                              ; preds = %506, %490
  %518 = load volatile i32, ptr %1, align 4, !tbaa !10
  %519 = add i32 %518, 1
  store volatile i32 %519, ptr %1, align 4, !tbaa !10
  br label %520

520:                                              ; preds = %517, %506
  %521 = add nuw nsw i32 %399, 4
  %522 = icmp eq i32 %521, 128
  br i1 %522, label %397, label %398, !llvm.loop !17
}

; Function Attrs: nocallback nofree nounwind willreturn memory(argmem: write)
declare void @llvm.memset.p0.i64(ptr writeonly captures(none), i8, i64, i1 immarg) #3

attributes #0 = { mustprogress nofree norecurse nosync nounwind ssp willreturn memory(none) uwtable(sync) "frame-pointer"="non-leaf" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="apple-m1" "target-features"="+aes,+altnzcv,+ccdp,+ccidx,+ccpp,+complxnum,+crc,+dit,+dotprod,+flagm,+fp-armv8,+fp16fml,+fptoint,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+predres,+ras,+rcpc,+rdm,+sb,+sha2,+sha3,+specrestrict,+ssbs,+v8.1a,+v8.2a,+v8.3a,+v8.4a,+v8a" }
attributes #1 = { mustprogress nocallback nofree nosync nounwind willreturn memory(argmem: readwrite) }
attributes #2 = { nofree norecurse nounwind ssp memory(readwrite, argmem: none) uwtable(sync) "frame-pointer"="non-leaf" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="apple-m1" "target-features"="+aes,+altnzcv,+ccdp,+ccidx,+ccpp,+complxnum,+crc,+dit,+dotprod,+flagm,+fp-armv8,+fp16fml,+fptoint,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+predres,+ras,+rcpc,+rdm,+sb,+sha2,+sha3,+specrestrict,+ssbs,+v8.1a,+v8.2a,+v8.3a,+v8.4a,+v8a" }
attributes #3 = { nocallback nofree nounwind willreturn memory(argmem: write) }

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
!14 = distinct !{!14, !13, !15}
!15 = !{!"llvm.loop.unroll.disable"}
!16 = distinct !{!16, !13, !15}
!17 = distinct !{!17, !13, !15}
