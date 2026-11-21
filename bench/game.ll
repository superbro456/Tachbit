; Сделано компилятором Tachbit (--llvm). Не правь руками: правь файл .tb
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-linux-gnu"
; в LLVM (1): main
; через C (0): 
%"tb_o_Bee" = type { i64, i64, i64, i64 }
declare void @tb_ll_crash(i32, i32) noreturn cold
declare void @tb_ll_noreturn(i32, ptr) noreturn cold
declare void @tb_x_ps(ptr)
declare ptr @tb_x_itos(i64)
declare ptr @tb_x_dtos(double)
declare ptr @tb_x_btos(i32)
declare ptr @tb_x_digits(i64, ptr, ptr)
declare ptr @tb_x_mem_np(i64)
declare i64 @tb_x_tlen(ptr)
declare ptr @tb_x_char_at(ptr, i64, i64, i32)
declare ptr @tb_x_cat(ptr, ptr)
declare void @tb_x_app(ptr, ptr, ptr, ptr)
declare void @tb_x_app_i(ptr, ptr, ptr, i64)
declare void @tb_x_app_n(ptr, ptr, ptr, ptr, i64)
declare ptr @tb_x_errp()
declare void @tb_x_die(ptr) noreturn cold
declare ptr @tb_x_grow(ptr)
declare zeroext i1 @tb_x_bget(ptr, i64, i32)
declare void @tb_x_bset(ptr, i64, i1 zeroext, i32)
declare void @tb_x_badd(ptr, i1 zeroext)
declare zeroext i1 @tb_x_bpop(ptr, i32)
declare ptr @tb_x_bits_new()
declare ptr @tb_x_bits_from(i64, ptr)
declare ptr @tb_x_pop(ptr, i32)
declare void @tb_x_lclear(ptr, i32)
declare ptr @tb_x_list_new_np(i64)
declare ptr @tb_x_list_from_np(i64, i64, ptr)
declare ptr @tb_x_dict_new(i32, i64)
declare ptr @tb_x_dict_from(i32, i64, i64, ptr, ptr)
declare ptr @tb_x_dget(ptr, ptr, i32)
declare ptr @tb_x_dput(ptr, ptr, ptr)
declare i32 @tb_x_dhas(ptr, ptr)
declare void @tb_x_dremove(ptr, ptr)
declare void @tb_x_dclear(ptr, i32)
declare i64 @tb_x_dlen(ptr)
declare ptr @tb_x_dkeys_copy(ptr)
declare ptr @tb_x_dvals_copy(ptr)
declare ptr @tb_x_list_new(i64)
declare ptr @tb_x_list_from(i64, i64, ptr)
declare void @tb_x_temp_begin_p(ptr)
declare ptr @tb_x_bits_of_bytes(ptr)
declare void @tb_x_app_take(ptr, ptr, ptr, ptr, i64, i64)
declare i64 @tb_x_dpre1(ptr, ptr)
declare void @tb_x_dpre2(ptr, i64)
declare void @tb_x_lpre(ptr, i64)
declare i64 @tb_x_lock_take(ptr)
declare void @tb_x_lock_give(ptr, i64)
declare ptr @tb_x_chan_new(i64, i32)
declare void @tb_x_chan_send(ptr, ptr, i32)
declare i32 @tb_x_chan_take(ptr, ptr)
declare void @tb_x_chan_close(ptr)
declare void @tb_x_par_run(ptr, ptr, i64)
declare i64 @tb_x_par_step(i64)
declare void @tb_x_par_lock()
declare void @tb_x_par_unlock()
declare void @tb_x_temp_end_p(ptr)
declare i32 @tb_x_fa_begin(ptr)
declare void @tb_x_fa_end(i32, ptr)
declare i64 @strlen(ptr)
declare i32 @strcmp(ptr, ptr)
declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1)

define void @"f_game__main"() #0 {
entry:
  %"v.bees.ptr" = alloca ptr
  %"v.r10.i64" = alloca i64
  %"v.flowers.ptr" = alloca ptr
  %"v.r27.i64" = alloca i64
  %"v.frames.i64" = alloca i64
  %"v.show_ms.i64" = alloca i64
  %"v.t0.i64" = alloca i64
  %"v.ts.i64" = alloca i64
  %"v.alive.i1" = alloca i1
  %"v.e62.i64" = alloca i64
  %"v.b.ptr" = alloca ptr
  %"v.e172.i64" = alloca i64
  %"v.f.i64" = alloca i64
  %"v.e198.i64" = alloca i64
  %"buf242" = alloca [24 x i8]
  %"bn243" = alloca i64
  %"v.ms.i64" = alloca i64
  call void @tb_xb_window_open(i64 640, i64 480, ptr @"tb.s.f_game__main.1", i32 10)
  %t3 = call ptr @tb_x_errp()
  %t2 = load ptr, ptr %t3
  %t4 = icmp ne ptr %t2, null
  br i1 %t4, label %L5, label %L6, !prof !0
L5:
  %t8 = call ptr @tb_x_errp()
  %t7 = load ptr, ptr %t8
  call void @tb_x_die(ptr %t7)
  unreachable
L6:
  %t9 = call ptr @tb_x_list_new(i64 8)
  store ptr %t9, ptr %"v.bees.ptr"
  store i64 0, ptr %"v.r10.i64"
  br label %L11
L11:
  %t15 = load i64, ptr %"v.r10.i64"
  %t16 = icmp slt i64 %t15, 500
  br i1 %t16, label %L12, label %L14
L12:
  %t17 = call i64 @tb_xb_random(i64 10, i64 630, i32 13)
  %t18 = call i64 @tb_xb_random(i64 40, i64 470, i32 13)
  %t19 = call i64 @tb_xb_random(i64 1, i64 4, i32 13)
  %t20 = call i64 @tb_xb_random(i64 1, i64 4, i32 13)
  %t21 = call ptr @tb_new_Bee(i64 %t17, i64 %t18, i64 %t19, i64 %t20)
  %t22 = load ptr, ptr %"v.bees.ptr"
  %t23 = call ptr @tb_x_grow(ptr %t22)
  store ptr %t21, ptr %t23, !tbaa !13
  br label %L13
L13:
  %t24 = load i64, ptr %"v.r10.i64"
  %t25 = add nsw i64 %t24, 1
  store i64 %t25, ptr %"v.r10.i64"
  br label %L11
L14:
  %t26 = call ptr @tb_x_list_new_np(i64 8)
  store ptr %t26, ptr %"v.flowers.ptr"
  store i64 0, ptr %"v.r27.i64"
  br label %L28
L28:
  %t32 = load i64, ptr %"v.r27.i64"
  %t33 = icmp slt i64 %t32, 100
  br i1 %t33, label %L29, label %L31
L29:
  %t34 = mul nsw i64 640, 480
  %t35 = sub nsw i64 %t34, 1
  %t36 = call i64 @tb_xb_random(i64 0, i64 %t35, i32 16)
  %t37 = load ptr, ptr %"v.flowers.ptr"
  %t38 = call ptr @tb_x_grow(ptr %t37)
  store i64 %t36, ptr %t38, !tbaa !11
  br label %L30
L30:
  %t39 = load i64, ptr %"v.r27.i64"
  %t40 = add nsw i64 %t39, 1
  store i64 %t40, ptr %"v.r27.i64"
  br label %L28
L31:
  store i64 0, ptr %"v.frames.i64"
  store i64 0, ptr %"v.show_ms.i64"
  %t41 = call i64 @tb_xb_time_ms(i32 19)
  store i64 %t41, ptr %"v.t0.i64"
  br label %L42
L42:
  %t45 = load i64, ptr %"v.frames.i64"
  %t46 = icmp slt i64 %t45, 1000
  br i1 %t46, label %L43, label %L44
L43:
  %t47 = call i64 @tb_xb_time_ms(i32 22)
  store i64 %t47, ptr %"v.ts.i64"
  %t48 = call zeroext i1 @tb_xb_window_alive(i32 23)
  store i1 %t48, ptr %"v.alive.i1"
  %t49 = call i64 @tb_xb_time_ms(i32 24)
  %t50 = load i64, ptr %"v.ts.i64"
  %t51 = sub nsw i64 %t49, %t50
  %t52 = load i64, ptr %"v.show_ms.i64"
  %t53 = add nsw i64 %t52, %t51
  store i64 %t53, ptr %"v.show_ms.i64"
  %t55 = load i1, ptr %"v.alive.i1"
  %t56 = xor i1 %t55, true
  br i1 %t56, label %L57, label %L58
L57:
  br label %L44
L58:
  br label %L54
L54:
  %t59 = load i64, ptr %"v.frames.i64"
  %t60 = add nsw i64 %t59, 1
  store i64 %t60, ptr %"v.frames.i64"
  %t61 = load ptr, ptr %"v.bees.ptr"
  store i64 0, ptr %"v.e62.i64"
  br label %L63
L63:
  %t67 = load i64, ptr %"v.e62.i64"
  %t68 = getelementptr inbounds i8, ptr %t61, i64 8
  %t69 = load i64, ptr %t68, !tbaa !10
  %t70 = icmp slt i64 %t67, %t69
  br i1 %t70, label %L64, label %L66
L64:
  %t71 = load ptr, ptr %t61, !tbaa !10
  %t72 = getelementptr inbounds ptr, ptr %t71, i64 %t67
  %t73 = load ptr, ptr %t72, !tbaa !13
  store ptr %t73, ptr %"v.b.ptr"
  %t74 = load ptr, ptr %"v.b.ptr"
  %t75 = icmp eq ptr %t74, null
  br i1 %t75, label %L76, label %L77, !prof !0
L76:
  call void @tb_ll_crash(i32 29, i32 4)
  unreachable
L77:
  %t78 = getelementptr inbounds %"tb_o_Bee", ptr %t74, i32 0, i32 2
  %t79 = load i64, ptr %t78
  %t80 = load ptr, ptr %"v.b.ptr"
  %t81 = icmp eq ptr %t80, null
  br i1 %t81, label %L82, label %L83, !prof !0
L82:
  call void @tb_ll_crash(i32 29, i32 4)
  unreachable
L83:
  %t84 = getelementptr inbounds %"tb_o_Bee", ptr %t80, i32 0, i32 0
  %t85 = load i64, ptr %t84
  %t86 = add nsw i64 %t85, %t79
  store i64 %t86, ptr %t84
  %t87 = load ptr, ptr %"v.b.ptr"
  %t88 = icmp eq ptr %t87, null
  br i1 %t88, label %L89, label %L90, !prof !0
L89:
  call void @tb_ll_crash(i32 30, i32 4)
  unreachable
L90:
  %t91 = getelementptr inbounds %"tb_o_Bee", ptr %t87, i32 0, i32 3
  %t92 = load i64, ptr %t91
  %t93 = load ptr, ptr %"v.b.ptr"
  %t94 = icmp eq ptr %t93, null
  br i1 %t94, label %L95, label %L96, !prof !0
L95:
  call void @tb_ll_crash(i32 30, i32 4)
  unreachable
L96:
  %t97 = getelementptr inbounds %"tb_o_Bee", ptr %t93, i32 0, i32 1
  %t98 = load i64, ptr %t97
  %t99 = add nsw i64 %t98, %t92
  store i64 %t99, ptr %t97
  %t101 = load ptr, ptr %"v.b.ptr"
  %t102 = icmp eq ptr %t101, null
  br i1 %t102, label %L103, label %L104, !prof !0
L103:
  call void @tb_ll_crash(i32 31, i32 4)
  unreachable
L104:
  %t105 = getelementptr inbounds %"tb_o_Bee", ptr %t101, i32 0, i32 0
  %t106 = load i64, ptr %t105
  %t107 = icmp slt i64 %t106, 10
  br label %L108
L108:
  br i1 %t107, label %L110, label %L109
L109:
  %t111 = load ptr, ptr %"v.b.ptr"
  %t112 = icmp eq ptr %t111, null
  br i1 %t112, label %L113, label %L114, !prof !0
L113:
  call void @tb_ll_crash(i32 31, i32 4)
  unreachable
L114:
  %t115 = getelementptr inbounds %"tb_o_Bee", ptr %t111, i32 0, i32 0
  %t116 = load i64, ptr %t115
  %t117 = icmp sgt i64 %t116, 630
  br label %L118
L118:
  br label %L110
L110:
  %t119 = phi i1 [ true, %L108 ], [ %t117, %L118 ]
  br i1 %t119, label %L120, label %L121
L120:
  %t122 = load ptr, ptr %"v.b.ptr"
  %t123 = icmp eq ptr %t122, null
  br i1 %t123, label %L124, label %L125, !prof !0
L124:
  call void @tb_ll_crash(i32 32, i32 4)
  unreachable
L125:
  %t126 = getelementptr inbounds %"tb_o_Bee", ptr %t122, i32 0, i32 2
  %t127 = load i64, ptr %t126
  %t128 = sub nsw i64 0, %t127
  %t129 = load ptr, ptr %"v.b.ptr"
  %t130 = icmp eq ptr %t129, null
  br i1 %t130, label %L131, label %L132, !prof !0
L131:
  call void @tb_ll_crash(i32 32, i32 4)
  unreachable
L132:
  %t133 = getelementptr inbounds %"tb_o_Bee", ptr %t129, i32 0, i32 2
  store i64 %t128, ptr %t133
  br label %L100
L121:
  br label %L100
L100:
  %t135 = load ptr, ptr %"v.b.ptr"
  %t136 = icmp eq ptr %t135, null
  br i1 %t136, label %L137, label %L138, !prof !0
L137:
  call void @tb_ll_crash(i32 33, i32 4)
  unreachable
L138:
  %t139 = getelementptr inbounds %"tb_o_Bee", ptr %t135, i32 0, i32 1
  %t140 = load i64, ptr %t139
  %t141 = icmp slt i64 %t140, 40
  br label %L142
L142:
  br i1 %t141, label %L144, label %L143
L143:
  %t145 = load ptr, ptr %"v.b.ptr"
  %t146 = icmp eq ptr %t145, null
  br i1 %t146, label %L147, label %L148, !prof !0
L147:
  call void @tb_ll_crash(i32 33, i32 4)
  unreachable
L148:
  %t149 = getelementptr inbounds %"tb_o_Bee", ptr %t145, i32 0, i32 1
  %t150 = load i64, ptr %t149
  %t151 = icmp sgt i64 %t150, 470
  br label %L152
L152:
  br label %L144
L144:
  %t153 = phi i1 [ true, %L142 ], [ %t151, %L152 ]
  br i1 %t153, label %L154, label %L155
L154:
  %t156 = load ptr, ptr %"v.b.ptr"
  %t157 = icmp eq ptr %t156, null
  br i1 %t157, label %L158, label %L159, !prof !0
L158:
  call void @tb_ll_crash(i32 34, i32 4)
  unreachable
L159:
  %t160 = getelementptr inbounds %"tb_o_Bee", ptr %t156, i32 0, i32 3
  %t161 = load i64, ptr %t160
  %t162 = sub nsw i64 0, %t161
  %t163 = load ptr, ptr %"v.b.ptr"
  %t164 = icmp eq ptr %t163, null
  br i1 %t164, label %L165, label %L166, !prof !0
L165:
  call void @tb_ll_crash(i32 34, i32 4)
  unreachable
L166:
  %t167 = getelementptr inbounds %"tb_o_Bee", ptr %t163, i32 0, i32 3
  store i64 %t162, ptr %t167
  br label %L134
L155:
  br label %L134
L134:
  br label %L65
L65:
  %t168 = load i64, ptr %"v.e62.i64"
  %t169 = add nsw i64 %t168, 1
  store i64 %t169, ptr %"v.e62.i64"
  br label %L63
L66:
  %t170 = call i64 @tb_xb_rgb(i64 13, i64 13, i64 16, i32 35)
  call void @tb_xb_clear_window(i64 %t170, i32 35)
  %t171 = load ptr, ptr %"v.flowers.ptr"
  store i64 0, ptr %"v.e172.i64"
  br label %L173
L173:
  %t177 = load i64, ptr %"v.e172.i64"
  %t178 = getelementptr inbounds i8, ptr %t171, i64 8
  %t179 = load i64, ptr %t178, !tbaa !10
  %t180 = icmp slt i64 %t177, %t179
  br i1 %t180, label %L174, label %L176
L174:
  %t181 = load ptr, ptr %t171, !tbaa !10
  %t182 = getelementptr inbounds i64, ptr %t181, i64 %t177
  %t183 = load i64, ptr %t182, !tbaa !11
  store i64 %t183, ptr %"v.f.i64"
  %t184 = load i64, ptr %"v.f.i64"
  %t186 = icmp eq i64 640, 0
  br i1 %t186, label %L187, label %L188, !prof !0
L187:
  call void @tb_ll_crash(i32 37, i32 1)
  unreachable
L188:
  %t185 = srem i64 %t184, 640
  %t189 = load i64, ptr %"v.f.i64"
  %t191 = icmp eq i64 640, 0
  br i1 %t191, label %L192, label %L193, !prof !0
L192:
  call void @tb_ll_crash(i32 37, i32 0)
  unreachable
L193:
  %t190 = sdiv i64 %t189, 640
  %t194 = call i64 @tb_xb_rgb(i64 255, i64 95, i64 162, i32 37)
  call void @tb_xb_draw_circle(i64 %t185, i64 %t190, i64 6, i64 %t194, i32 37)
  br label %L175
L175:
  %t195 = load i64, ptr %"v.e172.i64"
  %t196 = add nsw i64 %t195, 1
  store i64 %t196, ptr %"v.e172.i64"
  br label %L173
L176:
  %t197 = load ptr, ptr %"v.bees.ptr"
  store i64 0, ptr %"v.e198.i64"
  br label %L199
L199:
  %t203 = load i64, ptr %"v.e198.i64"
  %t204 = getelementptr inbounds i8, ptr %t197, i64 8
  %t205 = load i64, ptr %t204, !tbaa !10
  %t206 = icmp slt i64 %t203, %t205
  br i1 %t206, label %L200, label %L202
L200:
  %t207 = load ptr, ptr %t197, !tbaa !10
  %t208 = getelementptr inbounds ptr, ptr %t207, i64 %t203
  %t209 = load ptr, ptr %t208, !tbaa !13
  store ptr %t209, ptr %"v.b.ptr"
  %t210 = load ptr, ptr %"v.b.ptr"
  %t211 = icmp eq ptr %t210, null
  br i1 %t211, label %L212, label %L213, !prof !0
L212:
  call void @tb_ll_crash(i32 39, i32 4)
  unreachable
L213:
  %t214 = getelementptr inbounds %"tb_o_Bee", ptr %t210, i32 0, i32 0
  %t215 = load i64, ptr %t214
  %t216 = load ptr, ptr %"v.b.ptr"
  %t217 = icmp eq ptr %t216, null
  br i1 %t217, label %L218, label %L219, !prof !0
L218:
  call void @tb_ll_crash(i32 39, i32 4)
  unreachable
L219:
  %t220 = getelementptr inbounds %"tb_o_Bee", ptr %t216, i32 0, i32 1
  %t221 = load i64, ptr %t220
  %t222 = call i64 @tb_xb_rgb(i64 246, i64 195, i64 67, i32 39)
  call void @tb_xb_draw_circle(i64 %t215, i64 %t221, i64 8, i64 %t222, i32 39)
  %t223 = load ptr, ptr %"v.b.ptr"
  %t224 = icmp eq ptr %t223, null
  br i1 %t224, label %L225, label %L226, !prof !0
L225:
  call void @tb_ll_crash(i32 40, i32 4)
  unreachable
L226:
  %t227 = getelementptr inbounds %"tb_o_Bee", ptr %t223, i32 0, i32 0
  %t228 = load i64, ptr %t227
  %t229 = sub nsw i64 %t228, 2
  %t230 = load ptr, ptr %"v.b.ptr"
  %t231 = icmp eq ptr %t230, null
  br i1 %t231, label %L232, label %L233, !prof !0
L232:
  call void @tb_ll_crash(i32 40, i32 4)
  unreachable
L233:
  %t234 = getelementptr inbounds %"tb_o_Bee", ptr %t230, i32 0, i32 1
  %t235 = load i64, ptr %t234
  %t236 = sub nsw i64 %t235, 8
  %t237 = call i64 @tb_xb_rgb(i64 20, i64 20, i64 20, i32 40)
  call void @tb_xb_draw_rect(i64 %t229, i64 %t236, i64 3, i64 16, i64 %t237, i32 40)
  br label %L201
L201:
  %t238 = load i64, ptr %"v.e198.i64"
  %t239 = add nsw i64 %t238, 1
  store i64 %t239, ptr %"v.e198.i64"
  br label %L199
L202:
  %t241 = load i64, ptr %"v.frames.i64"
  %t244 = call ptr @tb_x_digits(i64 %t241, ptr %"buf242", ptr %"bn243")
  %t245 = load i64, ptr %"bn243"
  %t246 = add i64 1, 9
  %t247 = add i64 %t246, %t245
  %t248 = call ptr @tb_x_mem_np(i64 %t247)
  call void @llvm.memcpy.p0.p0.i64(ptr %t248, ptr @"tb.s.f_game__main.240", i64 9, i1 false)
  %t249 = getelementptr inbounds i8, ptr %t248, i64 9
  call void @llvm.memcpy.p0.p0.i64(ptr %t249, ptr %t244, i64 %t245, i1 false)
  %t250 = getelementptr inbounds i8, ptr %t249, i64 %t245
  store i8 0, ptr %t250
  %t251 = call i64 @tb_xb_rgb(i64 255, i64 255, i64 255, i32 41)
  call void @tb_xb_draw_text(i64 10, i64 10, ptr %t248, i64 2, i64 %t251, i32 41)
  br label %L42
L44:
  %t252 = call i64 @tb_xb_time_ms(i32 42)
  %t253 = load i64, ptr %"v.t0.i64"
  %t254 = sub nsw i64 %t252, %t253
  store i64 %t254, ptr %"v.ms.i64"
  call void @tb_show_4(ptr @"tb.s.f_game__main.256")
  call void @tb_x_ps(ptr @"tb.s.f_game__main.255")
  %t257 = load i64, ptr %"v.frames.i64"
  call void @tb_show_1(i64 %t257)
  call void @tb_x_ps(ptr @"tb.s.f_game__main.255")
  call void @tb_show_4(ptr @"tb.s.f_game__main.258")
  call void @tb_x_ps(ptr @"tb.s.f_game__main.255")
  %t259 = load i64, ptr %"v.ms.i64"
  call void @tb_show_1(i64 %t259)
  call void @tb_x_ps(ptr @"tb.s.f_game__main.255")
  call void @tb_show_4(ptr @"tb.s.f_game__main.260")
  call void @tb_x_ps(ptr @"tb.s.f_game__main.261")
  call void @tb_show_4(ptr @"tb.s.f_game__main.263")
  call void @tb_x_ps(ptr @"tb.s.f_game__main.262")
  %t264 = load i64, ptr %"v.frames.i64"
  %t265 = mul nsw i64 %t264, 1000
  %t266 = load i64, ptr %"v.ms.i64"
  %t267 = call i64 @tb_xb_max_i(i64 %t266, i64 1, i32 44)
  %t269 = icmp eq i64 %t267, 0
  br i1 %t269, label %L270, label %L271, !prof !0
L270:
  call void @tb_ll_crash(i32 44, i32 0)
  unreachable
L271:
  %t268 = sdiv i64 %t265, %t267
  call void @tb_show_1(i64 %t268)
  call void @tb_x_ps(ptr @"tb.s.f_game__main.272")
  call void @tb_show_4(ptr @"tb.s.f_game__main.274")
  call void @tb_x_ps(ptr @"tb.s.f_game__main.273")
  %t275 = load i64, ptr %"v.ms.i64"
  %t276 = mul nsw i64 %t275, 1000
  %t277 = load i64, ptr %"v.frames.i64"
  %t278 = call i64 @tb_xb_max_i(i64 %t277, i64 1, i32 45)
  %t280 = icmp eq i64 %t278, 0
  br i1 %t280, label %L281, label %L282, !prof !0
L281:
  call void @tb_ll_crash(i32 45, i32 0)
  unreachable
L282:
  %t279 = sdiv i64 %t276, %t278
  call void @tb_show_1(i64 %t279)
  call void @tb_x_ps(ptr @"tb.s.f_game__main.273")
  call void @tb_show_4(ptr @"tb.s.f_game__main.283")
  call void @tb_x_ps(ptr @"tb.s.f_game__main.284")
  call void @tb_show_4(ptr @"tb.s.f_game__main.286")
  call void @tb_x_ps(ptr @"tb.s.f_game__main.285")
  %t287 = load i64, ptr %"v.ms.i64"
  %t288 = load i64, ptr %"v.show_ms.i64"
  %t289 = sub nsw i64 %t287, %t288
  call void @tb_show_1(i64 %t289)
  call void @tb_x_ps(ptr @"tb.s.f_game__main.285")
  call void @tb_show_4(ptr @"tb.s.f_game__main.290")
  call void @tb_x_ps(ptr @"tb.s.f_game__main.285")
  %t291 = load i64, ptr %"v.show_ms.i64"
  call void @tb_show_1(i64 %t291)
  call void @tb_x_ps(ptr @"tb.s.f_game__main.285")
  call void @tb_show_4(ptr @"tb.s.f_game__main.292")
  call void @tb_x_ps(ptr @"tb.s.f_game__main.293")
  ret void
}

define internal void @tb_show_1(i64 %x) #0 {
entry:
  call void @tb_x_show_i(i64 %x)
  ret void
}
define internal void @tb_show_2(double %x) #0 {
entry:
  call void @tb_x_show_d(double %x)
  ret void
}
define internal void @tb_show_3(i1 zeroext %x) #0 {
entry:
  br i1 %x, label %L1, label %L2
L1:
  call void @tb_x_ps(ptr @"tb.s.f_tb__show3.3")
  ret void
L2:
  call void @tb_x_ps(ptr @"tb.s.f_tb__show3.4")
  ret void
}
define internal void @tb_show_4(ptr %x) #0 {
entry:
  call void @tb_x_ps(ptr %x)
  ret void
}
define internal void @tb_show_5(ptr %x) #0 {
entry:
  %t1 = icmp eq ptr %x, null
  br i1 %t1, label %L2, label %L3
L2:
  call void @tb_x_ps(ptr @"tb.s.f_tb__show5.4")
  ret void
L3:
  call void @tb_x_ps(ptr @"tb.s.f_tb__show5.5")
  call void @tb_x_ps(ptr @"tb.s.f_tb__show5.6")
  %t7 = getelementptr inbounds %"tb_o_Bee", ptr %x, i32 0, i32 0
  %t8 = load i64, ptr %t7
  call void @tb_show_1(i64 %t8)
  call void @tb_x_ps(ptr @"tb.s.f_tb__show5.9")
  %t10 = getelementptr inbounds %"tb_o_Bee", ptr %x, i32 0, i32 1
  %t11 = load i64, ptr %t10
  call void @tb_show_1(i64 %t11)
  call void @tb_x_ps(ptr @"tb.s.f_tb__show5.12")
  %t13 = getelementptr inbounds %"tb_o_Bee", ptr %x, i32 0, i32 2
  %t14 = load i64, ptr %t13
  call void @tb_show_1(i64 %t14)
  call void @tb_x_ps(ptr @"tb.s.f_tb__show5.15")
  %t16 = getelementptr inbounds %"tb_o_Bee", ptr %x, i32 0, i32 3
  %t17 = load i64, ptr %t16
  call void @tb_show_1(i64 %t17)
  call void @tb_x_ps(ptr @"tb.s.f_tb__show5.18")
  ret void
}
define internal void @tb_show_6(ptr %x) #0 {
entry:
  %"i4" = alloca i64
  call void @tb_x_ps(ptr @"tb.s.f_tb__show6.1")
  %t2 = getelementptr inbounds i8, ptr %x, i64 8
  %t3 = load i64, ptr %t2
  store i64 0, ptr %"i4"
  br label %L5
L5:
  %t8 = load i64, ptr %"i4"
  %t9 = icmp slt i64 %t8, %t3
  br i1 %t9, label %L6, label %L7
L6:
  %t10 = icmp eq i64 %t8, 0
  br i1 %t10, label %L12, label %L11
L11:
  call void @tb_x_ps(ptr @"tb.s.f_tb__show6.13")
  br label %L12
L12:
  %t14 = load ptr, ptr %x
  %t15 = getelementptr inbounds ptr, ptr %t14, i64 %t8
  %t16 = load ptr, ptr %t15
  call void @tb_show_5(ptr %t16)
  %t17 = add i64 %t8, 1
  store i64 %t17, ptr %"i4"
  br label %L5
L7:
  call void @tb_x_ps(ptr @"tb.s.f_tb__show6.18")
  ret void
}
define internal void @tb_show_7(ptr %x) #0 {
entry:
  %"i4" = alloca i64
  call void @tb_x_ps(ptr @"tb.s.f_tb__show7.1")
  %t2 = getelementptr inbounds i8, ptr %x, i64 8
  %t3 = load i64, ptr %t2
  store i64 0, ptr %"i4"
  br label %L5
L5:
  %t8 = load i64, ptr %"i4"
  %t9 = icmp slt i64 %t8, %t3
  br i1 %t9, label %L6, label %L7
L6:
  %t10 = icmp eq i64 %t8, 0
  br i1 %t10, label %L12, label %L11
L11:
  call void @tb_x_ps(ptr @"tb.s.f_tb__show7.13")
  br label %L12
L12:
  %t14 = load ptr, ptr %x
  %t15 = getelementptr inbounds i64, ptr %t14, i64 %t8
  %t16 = load i64, ptr %t15
  call void @tb_show_1(i64 %t16)
  %t17 = add i64 %t8, 1
  store i64 %t17, ptr %"i4"
  br label %L5
L7:
  call void @tb_x_ps(ptr @"tb.s.f_tb__show7.18")
  ret void
}
define internal ptr @tb_new_Bee(i64 %a0, i64 %a1, i64 %a2, i64 %a3) #0 {
entry:
  %t1 = getelementptr %"tb_o_Bee", ptr null, i32 1
  %t2 = ptrtoint ptr %t1 to i64
  %o = call ptr @tb_x_obj_mem_np(i64 %t2)
  %t3 = getelementptr inbounds %"tb_o_Bee", ptr %o, i32 0, i32 0
  store i64 %a0, ptr %t3
  %t4 = getelementptr inbounds %"tb_o_Bee", ptr %o, i32 0, i32 1
  store i64 %a1, ptr %t4
  %t5 = getelementptr inbounds %"tb_o_Bee", ptr %o, i32 0, i32 2
  store i64 %a2, ptr %t5
  %t6 = getelementptr inbounds %"tb_o_Bee", ptr %o, i32 0, i32 3
  store i64 %a3, ptr %t6
  ret ptr %o
}
define i32 @main(i32 %argc, ptr %argv) #0 {
entry:
  %top = alloca i8
  call void @tb_x_start(i32 %argc, ptr %argv, ptr %top)
  call void @tb_x_call_main(ptr @"f_game__main")
  ret i32 0
}
declare void @tb_x_show_i(i64)
declare void @tb_x_show_d(double)
declare ptr @tb_x_obj_mem(i64)
declare ptr @tb_x_obj_mem_np(i64)
declare ptr @tb_x_cold_mem(i64)
declare ptr @tb_x_cold_mem_np(i64)
declare void @tb_x_start(i32, ptr, ptr)
declare void @tb_x_gc_root(ptr, i64)
declare void @tb_x_call_main(ptr)
declare ptr @tb_x_mem(i64)
declare ptr @tb_x_resize(ptr, i64, i64)
@"tb.s.f_game__main.1" = private unnamed_addr constant [19 x i8] c"\D0\97\D0\B0\D0\BC\D0\B5\D1\80 Tachbit\00", align 1
declare void @tb_xb_window_open(i64, i64, ptr, i32)
declare i64 @tb_xb_random(i64, i64, i32)
declare i64 @tb_xb_time_ms(i32)
declare zeroext i1 @tb_xb_window_alive(i32)
declare i64 @tb_xb_rgb(i64, i64, i64, i32)
declare void @tb_xb_clear_window(i64, i32)
declare void @tb_xb_draw_circle(i64, i64, i64, i64, i32)
declare void @tb_xb_draw_rect(i64, i64, i64, i64, i64, i32)
@"tb.s.f_game__main.240" = private unnamed_addr constant [10 x i8] c"\D0\9A\D0\B0\D0\B4\D1\80 \00", align 1
declare void @tb_xb_draw_text(i64, i64, ptr, i64, i64, i32)
@"tb.s.f_game__main.255" = private unnamed_addr constant [2 x i8] c" \00", align 1
@"tb.s.f_game__main.256" = private unnamed_addr constant [14 x i8] c"\D0\BA\D0\B0\D0\B4\D1\80\D0\BE\D0\B2:\00", align 1
@"tb.s.f_game__main.258" = private unnamed_addr constant [13 x i8] c" \D0\B2\D1\80\D0\B5\D0\BC\D1\8F:\00", align 1
@"tb.s.f_game__main.260" = private unnamed_addr constant [5 x i8] c"\D0\BC\D1\81\00", align 1
@"tb.s.f_game__main.261" = private unnamed_addr constant [2 x i8] c"\0A\00", align 1
@"tb.s.f_game__main.262" = private unnamed_addr constant [2 x i8] c" \00", align 1
@"tb.s.f_game__main.263" = private unnamed_addr constant [32 x i8] c"\D0\BA\D0\B0\D0\B4\D1\80\D0\BE\D0\B2 \D0\B2 \D1\81\D0\B5\D0\BA\D1\83\D0\BD\D0\B4\D1\83:\00", align 1
declare i64 @tb_xb_max_i(i64, i64, i32)
@"tb.s.f_game__main.272" = private unnamed_addr constant [2 x i8] c"\0A\00", align 1
@"tb.s.f_game__main.273" = private unnamed_addr constant [2 x i8] c" \00", align 1
@"tb.s.f_game__main.274" = private unnamed_addr constant [24 x i8] c"\D0\BD\D0\B0 \D0\BE\D0\B4\D0\B8\D0\BD \D0\BA\D0\B0\D0\B4\D1\80:\00", align 1
@"tb.s.f_game__main.283" = private unnamed_addr constant [7 x i8] c"\D0\BC\D0\BA\D1\81\00", align 1
@"tb.s.f_game__main.284" = private unnamed_addr constant [2 x i8] c"\0A\00", align 1
@"tb.s.f_game__main.285" = private unnamed_addr constant [2 x i8] c" \00", align 1
@"tb.s.f_game__main.286" = private unnamed_addr constant [48 x i8] c"\D0\B8\D0\B7 \D0\BD\D0\B8\D1\85 \D1\80\D0\B0\D1\81\D1\87\D1\91\D1\82 \D0\B8 \D1\80\D0\B8\D1\81\D0\BE\D0\B2\D0\B0\D0\BD\D0\B8\D0\B5:\00", align 1
@"tb.s.f_game__main.290" = private unnamed_addr constant [36 x i8] c"\D0\BC\D1\81, \D0\BF\D0\BE\D0\BA\D0\B0\D0\B7 \D0\BD\D0\B0 \D1\8D\D0\BA\D1\80\D0\B0\D0\BD\D0\B5:\00", align 1
@"tb.s.f_game__main.292" = private unnamed_addr constant [5 x i8] c"\D0\BC\D1\81\00", align 1
@"tb.s.f_game__main.293" = private unnamed_addr constant [2 x i8] c"\0A\00", align 1
@"tb.s.f_tb__show3.3" = private unnamed_addr constant [5 x i8] c"true\00", align 1
@"tb.s.f_tb__show3.4" = private unnamed_addr constant [6 x i8] c"false\00", align 1
@"tb.s.f_tb__show5.4" = private unnamed_addr constant [11 x i8] c"\D0\BF\D1\83\D1\81\D1\82\D0\BE\00", align 1
@"tb.s.f_tb__show5.5" = private unnamed_addr constant [5 x i8] c"Bee(\00", align 1
@"tb.s.f_tb__show5.6" = private unnamed_addr constant [4 x i8] c"x: \00", align 1
@"tb.s.f_tb__show5.9" = private unnamed_addr constant [6 x i8] c", y: \00", align 1
@"tb.s.f_tb__show5.12" = private unnamed_addr constant [7 x i8] c", dx: \00", align 1
@"tb.s.f_tb__show5.15" = private unnamed_addr constant [7 x i8] c", dy: \00", align 1
@"tb.s.f_tb__show5.18" = private unnamed_addr constant [2 x i8] c")\00", align 1
@"tb.s.f_tb__show6.1" = private unnamed_addr constant [2 x i8] c"[\00", align 1
@"tb.s.f_tb__show6.13" = private unnamed_addr constant [3 x i8] c", \00", align 1
@"tb.s.f_tb__show6.18" = private unnamed_addr constant [2 x i8] c"]\00", align 1
@"tb.s.f_tb__show7.1" = private unnamed_addr constant [2 x i8] c"[\00", align 1
@"tb.s.f_tb__show7.13" = private unnamed_addr constant [3 x i8] c", \00", align 1
@"tb.s.f_tb__show7.18" = private unnamed_addr constant [2 x i8] c"]\00", align 1
attributes #0 = { nounwind uwtable "min-legal-vector-width"="0" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
!0 = !{!"branch_weights", i32 1, i32 2000}
!1 = !{!"tachbit"}
!2 = !{!"tb.list", !1, i64 0}
!3 = !{!"tb.int", !1, i64 0}
!4 = !{!"tb.dec", !1, i64 0}
!5 = !{!"tb.ptr", !1, i64 0}
!10 = !{!2, !2, i64 0}
!11 = !{!3, !3, i64 0}
!12 = !{!4, !4, i64 0}
!13 = !{!5, !5, i64 0}
