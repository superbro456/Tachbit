; Сделано компилятором Tachbit (--llvm). Не правь руками: правь файл .tb
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-linux-gnu"
; в LLVM (5): main, add_cube, face, sin_t, cos_t
; через C (0): 
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

define void @"f_cubes__main"() #0 {
entry:
  %"v.n.i64" = alloca i64
  %"v.a.ptr" = alloca ptr
  %"v.cube.i64" = alloca i64
  %"v.frames.i64" = alloca i64
  %"v.show_ms.i64" = alloca i64
  %"v.t0.i64" = alloca i64
  %"v.ts.i64" = alloca i64
  %"v.alive.i1" = alloca i1
  %"v.far.double" = alloca double
  %"v.i.i64" = alloca i64
  %"v.j.i64" = alloca i64
  %"buf141" = alloca [24 x i8]
  %"bn142" = alloca i64
  %"buf149" = alloca [24 x i8]
  %"bn150" = alloca i64
  %"v.ms.i64" = alloca i64
  store i64 20, ptr %"v.n.i64"
  %t1 = call ptr @tb_xb_args(i32 7)
  store ptr %t1, ptr %"v.a.ptr"
  %t3 = load ptr, ptr %"v.a.ptr"
  %t4 = getelementptr inbounds i8, ptr %t3, i64 8
  %t5 = load i64, ptr %t4, !tbaa !10
  %t6 = icmp sgt i64 %t5, 0
  br i1 %t6, label %L7, label %L8
L7:
  %t9 = load ptr, ptr %"v.a.ptr"
  %t10 = load ptr, ptr %"v.a.ptr"
  %t11 = getelementptr inbounds i8, ptr %t10, i64 8
  %t12 = load i64, ptr %t11, !tbaa !10
  %t13 = sub nsw i64 %t12, 1
  %t14 = getelementptr inbounds i8, ptr %t9, i64 8
  %t15 = load i64, ptr %t14, !tbaa !10
  %t16 = icmp uge i64 %t13, %t15
  br i1 %t16, label %L17, label %L18, !prof !0
L17:
  call void @tb_ll_crash(i32 9, i32 3)
  unreachable
L18:
  %t19 = load ptr, ptr %t9, !tbaa !10
  %t20 = getelementptr inbounds ptr, ptr %t19, i64 %t13
  %t21 = load ptr, ptr %t20, !tbaa !13
  %t22 = call i64 @tb_xb_to_int(ptr %t21, i32 9)
  %t24 = call ptr @tb_x_errp()
  %t23 = load ptr, ptr %t24
  %t25 = icmp ne ptr %t23, null
  br label %L26
L26:
  br i1 %t25, label %L27, label %L28
L27:
  %t29 = call ptr @tb_x_errp()
  store ptr null, ptr %t29
  br label %L30
L30:
  br label %L28
L28:
  %t31 = phi i64 [ %t22, %L26 ], [ 20, %L30 ]
  store i64 %t31, ptr %"v.n.i64"
  br label %L2
L8:
  br label %L2
L2:
  call void @tb_xb_window_open(i64 1280, i64 720, ptr @"tb.s.f_cubes__main.32", i32 10)
  %t34 = call ptr @tb_x_errp()
  %t33 = load ptr, ptr %t34
  %t35 = icmp ne ptr %t33, null
  br i1 %t35, label %L36, label %L37, !prof !0
L36:
  %t39 = call ptr @tb_x_errp()
  %t38 = load ptr, ptr %t39
  call void @tb_x_die(ptr %t38)
  unreachable
L37:
  call void @tb_xb_set_fps(i64 0, i32 11)
  call void @tb_show_4(ptr @"tb.s.f_cubes__main.41")
  call void @tb_x_ps(ptr @"tb.s.f_cubes__main.40")
  %t42 = call i64 @tb_xb_window_w(i32 12)
  call void @tb_show_1(i64 %t42)
  call void @tb_x_ps(ptr @"tb.s.f_cubes__main.40")
  call void @tb_show_4(ptr @"tb.s.f_cubes__main.43")
  call void @tb_x_ps(ptr @"tb.s.f_cubes__main.40")
  %t44 = call i64 @tb_xb_window_h(i32 12)
  call void @tb_show_1(i64 %t44)
  call void @tb_x_ps(ptr @"tb.s.f_cubes__main.45")
  %t46 = call i64 @tb_xb_mesh_new(i32 13)
  store i64 %t46, ptr %"v.cube.i64"
  %t47 = load i64, ptr %"v.cube.i64"
  %t48 = fdiv double 4.0, 1.0e1
  call void @"f_cubes__add_cube"(i64 %t47, double %t48)
  store i64 0, ptr %"v.frames.i64"
  store i64 0, ptr %"v.show_ms.i64"
  %t49 = call i64 @tb_xb_time_ms(i32 17)
  store i64 %t49, ptr %"v.t0.i64"
  br label %L50
L50:
  %t53 = load i64, ptr %"v.frames.i64"
  %t54 = icmp slt i64 %t53, 600
  br i1 %t54, label %L51, label %L52
L51:
  %t55 = call i64 @tb_xb_time_ms(i32 19)
  store i64 %t55, ptr %"v.ts.i64"
  %t56 = call zeroext i1 @tb_xb_window_alive(i32 20)
  store i1 %t56, ptr %"v.alive.i1"
  %t57 = call i64 @tb_xb_time_ms(i32 21)
  %t58 = load i64, ptr %"v.ts.i64"
  %t59 = sub nsw i64 %t57, %t58
  %t60 = load i64, ptr %"v.show_ms.i64"
  %t61 = add nsw i64 %t60, %t59
  store i64 %t61, ptr %"v.show_ms.i64"
  %t63 = load i1, ptr %"v.alive.i1"
  %t64 = xor i1 %t63, true
  br i1 %t64, label %L65, label %L66
L65:
  br label %L52
L66:
  br label %L62
L62:
  %t67 = load i64, ptr %"v.frames.i64"
  %t68 = add nsw i64 %t67, 1
  store i64 %t68, ptr %"v.frames.i64"
  %t69 = load i64, ptr %"v.n.i64"
  %t70 = fdiv double 12.0, 1.0e1
  %t72 = sitofp i64 %t69 to double
  %t71 = fmul double %t72, %t70
  %t74 = sitofp i64 4 to double
  %t73 = fadd double %t71, %t74
  store double %t73, ptr %"v.far.double"
  %t75 = load double, ptr %"v.far.double"
  %t76 = load i64, ptr %"v.frames.i64"
  %t77 = fdiv double 1.0, 1.0e2
  %t79 = sitofp i64 %t76 to double
  %t78 = fmul double %t79, %t77
  %t80 = call double @"f_cubes__cos_t"(double %t78)
  %t81 = fmul double %t75, %t80
  %t82 = load i64, ptr %"v.n.i64"
  %t83 = fdiv double 6.0, 1.0e1
  %t85 = sitofp i64 %t82 to double
  %t84 = fmul double %t85, %t83
  %t86 = load double, ptr %"v.far.double"
  %t87 = load i64, ptr %"v.frames.i64"
  %t88 = fdiv double 1.0, 1.0e2
  %t90 = sitofp i64 %t87 to double
  %t89 = fmul double %t90, %t88
  %t91 = call double @"f_cubes__sin_t"(double %t89)
  %t92 = fmul double %t86, %t91
  %t93 = sitofp i64 0 to double
  %t94 = sitofp i64 0 to double
  %t95 = sitofp i64 0 to double
  call void @tb_xb_camera(double %t81, double %t84, double %t92, double %t93, double %t94, double %t95, i32 26)
  %t96 = call i64 @tb_xb_rgb(i64 16, i64 18, i64 28, i32 27)
  call void @tb_xb_clear_window(i64 %t96, i32 27)
  %t97 = load i64, ptr %"v.n.i64"
  %t98 = sub nsw i64 %t97, 1
  store i64 0, ptr %"v.i.i64"
  br label %L100
L100:
  %t103 = load i64, ptr %"v.i.i64"
  %t104 = icmp sle i64 %t103, %t98
  br i1 %t104, label %L101, label %L99
L101:
  %t105 = load i64, ptr %"v.n.i64"
  %t106 = sub nsw i64 %t105, 1
  store i64 0, ptr %"v.j.i64"
  br label %L108
L108:
  %t111 = load i64, ptr %"v.j.i64"
  %t112 = icmp sle i64 %t111, %t106
  br i1 %t112, label %L109, label %L107
L109:
  %t113 = load i64, ptr %"v.cube.i64"
  %t114 = load i64, ptr %"v.i.i64"
  %t115 = load i64, ptr %"v.n.i64"
  %t117 = sitofp i64 %t115 to double
  %t116 = fdiv double %t117, 2.0
  %t119 = sitofp i64 %t114 to double
  %t118 = fsub double %t119, %t116
  %t120 = sitofp i64 0 to double
  %t121 = load i64, ptr %"v.j.i64"
  %t122 = load i64, ptr %"v.n.i64"
  %t124 = sitofp i64 %t122 to double
  %t123 = fdiv double %t124, 2.0
  %t126 = sitofp i64 %t121 to double
  %t125 = fsub double %t126, %t123
  %t127 = load i64, ptr %"v.frames.i64"
  %t128 = load i64, ptr %"v.i.i64"
  %t129 = mul nsw i64 %t128, 7
  %t130 = add nsw i64 %t127, %t129
  %t131 = load i64, ptr %"v.j.i64"
  %t132 = mul nsw i64 %t131, 3
  %t133 = add nsw i64 %t130, %t132
  %t134 = sitofp i64 %t133 to double
  call void @tb_xb_draw_mesh(i64 %t113, double %t118, double %t120, double %t125, double %t134, i32 30)
  br label %L110
L110:
  %t135 = load i64, ptr %"v.j.i64"
  %t136 = add nsw i64 %t135, 1
  store i64 %t136, ptr %"v.j.i64"
  br label %L108
L107:
  br label %L102
L102:
  %t137 = load i64, ptr %"v.i.i64"
  %t138 = add nsw i64 %t137, 1
  store i64 %t138, ptr %"v.i.i64"
  br label %L100
L99:
  %t140 = load i64, ptr %"v.frames.i64"
  %t143 = call ptr @tb_x_digits(i64 %t140, ptr %"buf141", ptr %"bn142")
  %t144 = load i64, ptr %"bn142"
  %t146 = load i64, ptr %"v.n.i64"
  %t147 = load i64, ptr %"v.n.i64"
  %t148 = mul nsw i64 %t146, %t147
  %t151 = call ptr @tb_x_digits(i64 %t148, ptr %"buf149", ptr %"bn150")
  %t152 = load i64, ptr %"bn150"
  %t153 = add i64 1, 9
  %t154 = add i64 %t153, %t144
  %t155 = add i64 %t154, 14
  %t156 = add i64 %t155, %t152
  %t157 = call ptr @tb_x_mem_np(i64 %t156)
  call void @llvm.memcpy.p0.p0.i64(ptr %t157, ptr @"tb.s.f_cubes__main.139", i64 9, i1 false)
  %t158 = getelementptr inbounds i8, ptr %t157, i64 9
  call void @llvm.memcpy.p0.p0.i64(ptr %t158, ptr %t143, i64 %t144, i1 false)
  %t159 = getelementptr inbounds i8, ptr %t158, i64 %t144
  call void @llvm.memcpy.p0.p0.i64(ptr %t159, ptr @"tb.s.f_cubes__main.145", i64 14, i1 false)
  %t160 = getelementptr inbounds i8, ptr %t159, i64 14
  call void @llvm.memcpy.p0.p0.i64(ptr %t160, ptr %t151, i64 %t152, i1 false)
  %t161 = getelementptr inbounds i8, ptr %t160, i64 %t152
  store i8 0, ptr %t161
  %t162 = call i64 @tb_xb_rgb(i64 255, i64 255, i64 255, i32 31)
  call void @tb_xb_draw_text(i64 10, i64 10, ptr %t157, i64 2, i64 %t162, i32 31)
  br label %L50
L52:
  %t163 = call i64 @tb_xb_time_ms(i32 32)
  %t164 = load i64, ptr %"v.t0.i64"
  %t165 = sub nsw i64 %t163, %t164
  store i64 %t165, ptr %"v.ms.i64"
  call void @tb_show_4(ptr @"tb.s.f_cubes__main.167")
  call void @tb_x_ps(ptr @"tb.s.f_cubes__main.166")
  %t168 = call i64 @tb_xb_window_w(i32 33)
  call void @tb_show_1(i64 %t168)
  call void @tb_x_ps(ptr @"tb.s.f_cubes__main.166")
  call void @tb_show_4(ptr @"tb.s.f_cubes__main.169")
  call void @tb_x_ps(ptr @"tb.s.f_cubes__main.166")
  %t170 = call i64 @tb_xb_window_h(i32 33)
  call void @tb_show_1(i64 %t170)
  call void @tb_x_ps(ptr @"tb.s.f_cubes__main.166")
  call void @tb_show_4(ptr @"tb.s.f_cubes__main.171")
  call void @tb_x_ps(ptr @"tb.s.f_cubes__main.166")
  %t172 = load i64, ptr %"v.n.i64"
  %t173 = load i64, ptr %"v.n.i64"
  %t174 = mul nsw i64 %t172, %t173
  call void @tb_show_1(i64 %t174)
  call void @tb_x_ps(ptr @"tb.s.f_cubes__main.166")
  call void @tb_show_4(ptr @"tb.s.f_cubes__main.175")
  call void @tb_x_ps(ptr @"tb.s.f_cubes__main.166")
  %t176 = load i64, ptr %"v.n.i64"
  %t177 = load i64, ptr %"v.n.i64"
  %t178 = mul nsw i64 %t176, %t177
  %t179 = mul nsw i64 %t178, 12
  call void @tb_show_1(i64 %t179)
  call void @tb_x_ps(ptr @"tb.s.f_cubes__main.180")
  call void @tb_show_4(ptr @"tb.s.f_cubes__main.182")
  call void @tb_x_ps(ptr @"tb.s.f_cubes__main.181")
  %t183 = load i64, ptr %"v.frames.i64"
  call void @tb_show_1(i64 %t183)
  call void @tb_x_ps(ptr @"tb.s.f_cubes__main.181")
  call void @tb_show_4(ptr @"tb.s.f_cubes__main.184")
  call void @tb_x_ps(ptr @"tb.s.f_cubes__main.181")
  %t185 = load i64, ptr %"v.ms.i64"
  call void @tb_show_1(i64 %t185)
  call void @tb_x_ps(ptr @"tb.s.f_cubes__main.181")
  call void @tb_show_4(ptr @"tb.s.f_cubes__main.186")
  call void @tb_x_ps(ptr @"tb.s.f_cubes__main.187")
  call void @tb_show_4(ptr @"tb.s.f_cubes__main.189")
  call void @tb_x_ps(ptr @"tb.s.f_cubes__main.188")
  %t190 = load i64, ptr %"v.frames.i64"
  %t191 = mul nsw i64 %t190, 1000
  %t192 = load i64, ptr %"v.ms.i64"
  %t193 = call i64 @tb_xb_max_i(i64 %t192, i64 1, i32 35)
  %t195 = icmp eq i64 %t193, 0
  br i1 %t195, label %L196, label %L197, !prof !0
L196:
  call void @tb_ll_crash(i32 35, i32 0)
  unreachable
L197:
  %t194 = sdiv i64 %t191, %t193
  call void @tb_show_1(i64 %t194)
  call void @tb_x_ps(ptr @"tb.s.f_cubes__main.198")
  call void @tb_show_4(ptr @"tb.s.f_cubes__main.200")
  call void @tb_x_ps(ptr @"tb.s.f_cubes__main.199")
  %t201 = load i64, ptr %"v.n.i64"
  %t202 = load i64, ptr %"v.n.i64"
  %t203 = mul nsw i64 %t201, %t202
  %t204 = mul nsw i64 %t203, 12
  %t205 = load i64, ptr %"v.frames.i64"
  %t206 = mul nsw i64 %t204, %t205
  %t207 = mul nsw i64 %t206, 1000
  %t208 = load i64, ptr %"v.ms.i64"
  %t209 = call i64 @tb_xb_max_i(i64 %t208, i64 1, i32 36)
  %t211 = icmp eq i64 %t209, 0
  br i1 %t211, label %L212, label %L213, !prof !0
L212:
  call void @tb_ll_crash(i32 36, i32 0)
  unreachable
L213:
  %t210 = sdiv i64 %t207, %t209
  call void @tb_show_1(i64 %t210)
  call void @tb_x_ps(ptr @"tb.s.f_cubes__main.214")
  call void @tb_show_4(ptr @"tb.s.f_cubes__main.216")
  call void @tb_x_ps(ptr @"tb.s.f_cubes__main.215")
  %t217 = load i64, ptr %"v.ms.i64"
  %t218 = load i64, ptr %"v.show_ms.i64"
  %t219 = sub nsw i64 %t217, %t218
  call void @tb_show_1(i64 %t219)
  call void @tb_x_ps(ptr @"tb.s.f_cubes__main.215")
  call void @tb_show_4(ptr @"tb.s.f_cubes__main.220")
  call void @tb_x_ps(ptr @"tb.s.f_cubes__main.215")
  %t221 = load i64, ptr %"v.show_ms.i64"
  call void @tb_show_1(i64 %t221)
  call void @tb_x_ps(ptr @"tb.s.f_cubes__main.215")
  call void @tb_show_4(ptr @"tb.s.f_cubes__main.222")
  call void @tb_x_ps(ptr @"tb.s.f_cubes__main.223")
  ret void
}

define void @"f_cubes__add_cube"(i64 %"p.m", double %"p.h") #0 {
entry:
  %"v.m.i64" = alloca i64
  %"v.h.double" = alloca double
  store i64 %"p.m", ptr %"v.m.i64"
  store double %"p.h", ptr %"v.h.double"
  %t1 = load i64, ptr %"v.m.i64"
  %t2 = load double, ptr %"v.h.double"
  %t3 = fneg double %t2
  %t4 = load double, ptr %"v.h.double"
  %t5 = fneg double %t4
  %t6 = load double, ptr %"v.h.double"
  %t7 = fneg double %t6
  %t8 = load double, ptr %"v.h.double"
  %t9 = fneg double %t8
  %t10 = load double, ptr %"v.h.double"
  %t11 = load double, ptr %"v.h.double"
  %t12 = fneg double %t11
  %t13 = load double, ptr %"v.h.double"
  %t14 = load double, ptr %"v.h.double"
  %t15 = load double, ptr %"v.h.double"
  %t16 = fneg double %t15
  %t17 = load double, ptr %"v.h.double"
  %t18 = load double, ptr %"v.h.double"
  %t19 = fneg double %t18
  %t20 = load double, ptr %"v.h.double"
  %t21 = fneg double %t20
  %t22 = call i64 @tb_xb_rgb(i64 230, i64 80, i64 80, i32 42)
  call void @"f_cubes__face"(i64 %t1, double %t3, double %t5, double %t7, double %t9, double %t10, double %t12, double %t13, double %t14, double %t16, double %t17, double %t19, double %t21, i64 %t22)
  %t23 = load i64, ptr %"v.m.i64"
  %t24 = load double, ptr %"v.h.double"
  %t25 = fneg double %t24
  %t26 = load double, ptr %"v.h.double"
  %t27 = fneg double %t26
  %t28 = load double, ptr %"v.h.double"
  %t29 = load double, ptr %"v.h.double"
  %t30 = load double, ptr %"v.h.double"
  %t31 = fneg double %t30
  %t32 = load double, ptr %"v.h.double"
  %t33 = load double, ptr %"v.h.double"
  %t34 = load double, ptr %"v.h.double"
  %t35 = load double, ptr %"v.h.double"
  %t36 = load double, ptr %"v.h.double"
  %t37 = fneg double %t36
  %t38 = load double, ptr %"v.h.double"
  %t39 = load double, ptr %"v.h.double"
  %t40 = call i64 @tb_xb_rgb(i64 80, i64 200, i64 120, i32 43)
  call void @"f_cubes__face"(i64 %t23, double %t25, double %t27, double %t28, double %t29, double %t31, double %t32, double %t33, double %t34, double %t35, double %t37, double %t38, double %t39, i64 %t40)
  %t41 = load i64, ptr %"v.m.i64"
  %t42 = load double, ptr %"v.h.double"
  %t43 = fneg double %t42
  %t44 = load double, ptr %"v.h.double"
  %t45 = fneg double %t44
  %t46 = load double, ptr %"v.h.double"
  %t47 = fneg double %t46
  %t48 = load double, ptr %"v.h.double"
  %t49 = fneg double %t48
  %t50 = load double, ptr %"v.h.double"
  %t51 = fneg double %t50
  %t52 = load double, ptr %"v.h.double"
  %t53 = load double, ptr %"v.h.double"
  %t54 = fneg double %t53
  %t55 = load double, ptr %"v.h.double"
  %t56 = load double, ptr %"v.h.double"
  %t57 = load double, ptr %"v.h.double"
  %t58 = fneg double %t57
  %t59 = load double, ptr %"v.h.double"
  %t60 = load double, ptr %"v.h.double"
  %t61 = fneg double %t60
  %t62 = call i64 @tb_xb_rgb(i64 80, i64 130, i64 230, i32 44)
  call void @"f_cubes__face"(i64 %t41, double %t43, double %t45, double %t47, double %t49, double %t51, double %t52, double %t54, double %t55, double %t56, double %t58, double %t59, double %t61, i64 %t62)
  %t63 = load i64, ptr %"v.m.i64"
  %t64 = load double, ptr %"v.h.double"
  %t65 = load double, ptr %"v.h.double"
  %t66 = fneg double %t65
  %t67 = load double, ptr %"v.h.double"
  %t68 = fneg double %t67
  %t69 = load double, ptr %"v.h.double"
  %t70 = load double, ptr %"v.h.double"
  %t71 = load double, ptr %"v.h.double"
  %t72 = fneg double %t71
  %t73 = load double, ptr %"v.h.double"
  %t74 = load double, ptr %"v.h.double"
  %t75 = load double, ptr %"v.h.double"
  %t76 = load double, ptr %"v.h.double"
  %t77 = load double, ptr %"v.h.double"
  %t78 = fneg double %t77
  %t79 = load double, ptr %"v.h.double"
  %t80 = call i64 @tb_xb_rgb(i64 240, i64 200, i64 70, i32 45)
  call void @"f_cubes__face"(i64 %t63, double %t64, double %t66, double %t68, double %t69, double %t70, double %t72, double %t73, double %t74, double %t75, double %t76, double %t78, double %t79, i64 %t80)
  %t81 = load i64, ptr %"v.m.i64"
  %t82 = load double, ptr %"v.h.double"
  %t83 = fneg double %t82
  %t84 = load double, ptr %"v.h.double"
  %t85 = load double, ptr %"v.h.double"
  %t86 = fneg double %t85
  %t87 = load double, ptr %"v.h.double"
  %t88 = fneg double %t87
  %t89 = load double, ptr %"v.h.double"
  %t90 = load double, ptr %"v.h.double"
  %t91 = load double, ptr %"v.h.double"
  %t92 = load double, ptr %"v.h.double"
  %t93 = load double, ptr %"v.h.double"
  %t94 = load double, ptr %"v.h.double"
  %t95 = load double, ptr %"v.h.double"
  %t96 = load double, ptr %"v.h.double"
  %t97 = fneg double %t96
  %t98 = call i64 @tb_xb_rgb(i64 200, i64 90, i64 200, i32 46)
  call void @"f_cubes__face"(i64 %t81, double %t83, double %t84, double %t86, double %t88, double %t89, double %t90, double %t91, double %t92, double %t93, double %t94, double %t95, double %t97, i64 %t98)
  %t99 = load i64, ptr %"v.m.i64"
  %t100 = load double, ptr %"v.h.double"
  %t101 = fneg double %t100
  %t102 = load double, ptr %"v.h.double"
  %t103 = fneg double %t102
  %t104 = load double, ptr %"v.h.double"
  %t105 = fneg double %t104
  %t106 = load double, ptr %"v.h.double"
  %t107 = load double, ptr %"v.h.double"
  %t108 = fneg double %t107
  %t109 = load double, ptr %"v.h.double"
  %t110 = fneg double %t109
  %t111 = load double, ptr %"v.h.double"
  %t112 = load double, ptr %"v.h.double"
  %t113 = fneg double %t112
  %t114 = load double, ptr %"v.h.double"
  %t115 = load double, ptr %"v.h.double"
  %t116 = fneg double %t115
  %t117 = load double, ptr %"v.h.double"
  %t118 = fneg double %t117
  %t119 = load double, ptr %"v.h.double"
  %t120 = call i64 @tb_xb_rgb(i64 70, i64 200, i64 210, i32 47)
  call void @"f_cubes__face"(i64 %t99, double %t101, double %t103, double %t105, double %t106, double %t108, double %t110, double %t111, double %t113, double %t114, double %t116, double %t118, double %t119, i64 %t120)
  ret void
}

define void @"f_cubes__face"(i64 %"p.m", double %"p.x1", double %"p.y1", double %"p.z1", double %"p.x2", double %"p.y2", double %"p.z2", double %"p.x3", double %"p.y3", double %"p.z3", double %"p.x4", double %"p.y4", double %"p.z4", i64 %"p.c") #0 {
entry:
  %"v.m.i64" = alloca i64
  %"v.x1.double" = alloca double
  %"v.y1.double" = alloca double
  %"v.z1.double" = alloca double
  %"v.x2.double" = alloca double
  %"v.y2.double" = alloca double
  %"v.z2.double" = alloca double
  %"v.x3.double" = alloca double
  %"v.y3.double" = alloca double
  %"v.z3.double" = alloca double
  %"v.x4.double" = alloca double
  %"v.y4.double" = alloca double
  %"v.z4.double" = alloca double
  %"v.c.i64" = alloca i64
  store i64 %"p.m", ptr %"v.m.i64"
  store double %"p.x1", ptr %"v.x1.double"
  store double %"p.y1", ptr %"v.y1.double"
  store double %"p.z1", ptr %"v.z1.double"
  store double %"p.x2", ptr %"v.x2.double"
  store double %"p.y2", ptr %"v.y2.double"
  store double %"p.z2", ptr %"v.z2.double"
  store double %"p.x3", ptr %"v.x3.double"
  store double %"p.y3", ptr %"v.y3.double"
  store double %"p.z3", ptr %"v.z3.double"
  store double %"p.x4", ptr %"v.x4.double"
  store double %"p.y4", ptr %"v.y4.double"
  store double %"p.z4", ptr %"v.z4.double"
  store i64 %"p.c", ptr %"v.c.i64"
  %t1 = load i64, ptr %"v.m.i64"
  %t2 = load double, ptr %"v.x1.double"
  %t3 = load double, ptr %"v.y1.double"
  %t4 = load double, ptr %"v.z1.double"
  %t5 = load double, ptr %"v.x2.double"
  %t6 = load double, ptr %"v.y2.double"
  %t7 = load double, ptr %"v.z2.double"
  %t8 = load double, ptr %"v.x3.double"
  %t9 = load double, ptr %"v.y3.double"
  %t10 = load double, ptr %"v.z3.double"
  %t11 = load i64, ptr %"v.c.i64"
  call void @tb_xb_mesh_add(i64 %t1, double %t2, double %t3, double %t4, double %t5, double %t6, double %t7, double %t8, double %t9, double %t10, i64 %t11, i32 50)
  %t12 = load i64, ptr %"v.m.i64"
  %t13 = load double, ptr %"v.x1.double"
  %t14 = load double, ptr %"v.y1.double"
  %t15 = load double, ptr %"v.z1.double"
  %t16 = load double, ptr %"v.x3.double"
  %t17 = load double, ptr %"v.y3.double"
  %t18 = load double, ptr %"v.z3.double"
  %t19 = load double, ptr %"v.x4.double"
  %t20 = load double, ptr %"v.y4.double"
  %t21 = load double, ptr %"v.z4.double"
  %t22 = load i64, ptr %"v.c.i64"
  call void @tb_xb_mesh_add(i64 %t12, double %t13, double %t14, double %t15, double %t16, double %t17, double %t18, double %t19, double %t20, double %t21, i64 %t22, i32 51)
  ret void
}

define double @"f_cubes__sin_t"(double %"p.x") #0 {
entry:
  %"v.x.double" = alloca double
  %"v.x2.double" = alloca double
  store double %"p.x", ptr %"v.x.double"
  br label %L1
L1:
  %t4 = load double, ptr %"v.x.double"
  %t5 = fdiv double 314159265.0, 1.0e8
  %t6 = fcmp ogt double %t4, %t5
  br i1 %t6, label %L2, label %L3
L2:
  %t7 = fdiv double 62831853.0, 1.0e7
  %t8 = load double, ptr %"v.x.double"
  %t9 = fsub double %t8, %t7
  store double %t9, ptr %"v.x.double"
  br label %L1
L3:
  br label %L10
L10:
  %t13 = load double, ptr %"v.x.double"
  %t14 = fdiv double 314159265.0, 1.0e8
  %t15 = fneg double %t14
  %t16 = fcmp olt double %t13, %t15
  br i1 %t16, label %L11, label %L12
L11:
  %t17 = fdiv double 62831853.0, 1.0e7
  %t18 = load double, ptr %"v.x.double"
  %t19 = fadd double %t18, %t17
  store double %t19, ptr %"v.x.double"
  br label %L10
L12:
  %t20 = load double, ptr %"v.x.double"
  %t21 = load double, ptr %"v.x.double"
  %t22 = fmul double %t20, %t21
  store double %t22, ptr %"v.x2.double"
  %t23 = load double, ptr %"v.x.double"
  %t24 = load double, ptr %"v.x2.double"
  %t26 = sitofp i64 6 to double
  %t25 = fdiv double %t24, %t26
  %t27 = load double, ptr %"v.x2.double"
  %t29 = sitofp i64 20 to double
  %t28 = fdiv double %t27, %t29
  %t30 = load double, ptr %"v.x2.double"
  %t32 = sitofp i64 42 to double
  %t31 = fdiv double %t30, %t32
  %t33 = load double, ptr %"v.x2.double"
  %t35 = sitofp i64 72 to double
  %t34 = fdiv double %t33, %t35
  %t36 = load double, ptr %"v.x2.double"
  %t38 = sitofp i64 110 to double
  %t37 = fdiv double %t36, %t38
  %t40 = sitofp i64 1 to double
  %t39 = fsub double %t40, %t37
  %t41 = fmul double %t34, %t39
  %t43 = sitofp i64 1 to double
  %t42 = fsub double %t43, %t41
  %t44 = fmul double %t31, %t42
  %t46 = sitofp i64 1 to double
  %t45 = fsub double %t46, %t44
  %t47 = fmul double %t28, %t45
  %t49 = sitofp i64 1 to double
  %t48 = fsub double %t49, %t47
  %t50 = fmul double %t25, %t48
  %t52 = sitofp i64 1 to double
  %t51 = fsub double %t52, %t50
  %t53 = fmul double %t23, %t51
  ret double %t53
}

define double @"f_cubes__cos_t"(double %"p.x") #0 {
entry:
  %"v.x.double" = alloca double
  store double %"p.x", ptr %"v.x.double"
  %t1 = load double, ptr %"v.x.double"
  %t2 = fdiv double 157079633.0, 1.0e8
  %t3 = fadd double %t1, %t2
  %t4 = call double @"f_cubes__sin_t"(double %t3)
  ret double %t4
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
  %"i4" = alloca i64
  call void @tb_x_ps(ptr @"tb.s.f_tb__show5.1")
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
  call void @tb_x_ps(ptr @"tb.s.f_tb__show5.13")
  br label %L12
L12:
  %t14 = load ptr, ptr %x
  %t15 = getelementptr inbounds ptr, ptr %t14, i64 %t8
  %t16 = load ptr, ptr %t15
  call void @tb_show_4(ptr %t16)
  %t17 = add i64 %t8, 1
  store i64 %t17, ptr %"i4"
  br label %L5
L7:
  call void @tb_x_ps(ptr @"tb.s.f_tb__show5.18")
  ret void
}
define i32 @main(i32 %argc, ptr %argv) #0 {
entry:
  %top = alloca i8
  call void @tb_x_start(i32 %argc, ptr %argv, ptr %top)
  call void @tb_x_call_main(ptr @"f_cubes__main")
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
declare ptr @tb_xb_args(i32)
declare i64 @tb_xb_to_int(ptr, i32)
@"tb.s.f_cubes__main.32" = private unnamed_addr constant [14 x i8] c"\D0\97\D0\B0\D0\BC\D0\B5\D1\80 3D\00", align 1
declare void @tb_xb_window_open(i64, i64, ptr, i32)
declare void @tb_xb_set_fps(i64, i32)
@"tb.s.f_cubes__main.40" = private unnamed_addr constant [2 x i8] c" \00", align 1
@"tb.s.f_cubes__main.41" = private unnamed_addr constant [25 x i8] c"\D0\BE\D0\BA\D0\BD\D0\BE \D1\81\D0\BD\D0\B0\D1\87\D0\B0\D0\BB\D0\B0:\00", align 1
declare i64 @tb_xb_window_w(i32)
@"tb.s.f_cubes__main.43" = private unnamed_addr constant [2 x i8] c"x\00", align 1
declare i64 @tb_xb_window_h(i32)
@"tb.s.f_cubes__main.45" = private unnamed_addr constant [2 x i8] c"\0A\00", align 1
declare i64 @tb_xb_mesh_new(i32)
declare i64 @tb_xb_time_ms(i32)
declare zeroext i1 @tb_xb_window_alive(i32)
declare void @tb_xb_camera(double, double, double, double, double, double, i32)
declare i64 @tb_xb_rgb(i64, i64, i64, i32)
declare void @tb_xb_clear_window(i64, i32)
declare void @tb_xb_draw_mesh(i64, double, double, double, double, i32)
@"tb.s.f_cubes__main.139" = private unnamed_addr constant [10 x i8] c"\D0\BA\D0\B0\D0\B4\D1\80 \00", align 1
@"tb.s.f_cubes__main.145" = private unnamed_addr constant [15 x i8] c"   \D0\BA\D1\83\D0\B1\D0\BE\D0\B2 \00", align 1
declare void @tb_xb_draw_text(i64, i64, ptr, i64, i64, i32)
@"tb.s.f_cubes__main.166" = private unnamed_addr constant [2 x i8] c" \00", align 1
@"tb.s.f_cubes__main.167" = private unnamed_addr constant [10 x i8] c"\D0\BE\D0\BA\D0\BD\D0\BE:\00", align 1
@"tb.s.f_cubes__main.169" = private unnamed_addr constant [2 x i8] c"x\00", align 1
@"tb.s.f_cubes__main.171" = private unnamed_addr constant [14 x i8] c"  \D0\BA\D1\83\D0\B1\D0\BE\D0\B2:\00", align 1
@"tb.s.f_cubes__main.175" = private unnamed_addr constant [44 x i8] c"  \D1\82\D1\80\D0\B5\D1\83\D0\B3\D0\BE\D0\BB\D1\8C\D0\BD\D0\B8\D0\BA\D0\BE\D0\B2 \D0\B2 \D0\BA\D0\B0\D0\B4\D1\80\D0\B5:\00", align 1
@"tb.s.f_cubes__main.180" = private unnamed_addr constant [2 x i8] c"\0A\00", align 1
@"tb.s.f_cubes__main.181" = private unnamed_addr constant [2 x i8] c" \00", align 1
@"tb.s.f_cubes__main.182" = private unnamed_addr constant [14 x i8] c"\D0\BA\D0\B0\D0\B4\D1\80\D0\BE\D0\B2:\00", align 1
@"tb.s.f_cubes__main.184" = private unnamed_addr constant [13 x i8] c" \D0\B2\D1\80\D0\B5\D0\BC\D1\8F:\00", align 1
@"tb.s.f_cubes__main.186" = private unnamed_addr constant [5 x i8] c"\D0\BC\D1\81\00", align 1
@"tb.s.f_cubes__main.187" = private unnamed_addr constant [2 x i8] c"\0A\00", align 1
@"tb.s.f_cubes__main.188" = private unnamed_addr constant [2 x i8] c" \00", align 1
@"tb.s.f_cubes__main.189" = private unnamed_addr constant [32 x i8] c"\D0\BA\D0\B0\D0\B4\D1\80\D0\BE\D0\B2 \D0\B2 \D1\81\D0\B5\D0\BA\D1\83\D0\BD\D0\B4\D1\83:\00", align 1
declare i64 @tb_xb_max_i(i64, i64, i32)
@"tb.s.f_cubes__main.198" = private unnamed_addr constant [2 x i8] c"\0A\00", align 1
@"tb.s.f_cubes__main.199" = private unnamed_addr constant [2 x i8] c" \00", align 1
@"tb.s.f_cubes__main.200" = private unnamed_addr constant [46 x i8] c"\D1\82\D1\80\D0\B5\D1\83\D0\B3\D0\BE\D0\BB\D1\8C\D0\BD\D0\B8\D0\BA\D0\BE\D0\B2 \D0\B2 \D1\81\D0\B5\D0\BA\D1\83\D0\BD\D0\B4\D1\83:\00", align 1
@"tb.s.f_cubes__main.214" = private unnamed_addr constant [2 x i8] c"\0A\00", align 1
@"tb.s.f_cubes__main.215" = private unnamed_addr constant [2 x i8] c" \00", align 1
@"tb.s.f_cubes__main.216" = private unnamed_addr constant [48 x i8] c"\D0\B8\D0\B7 \D0\BD\D0\B8\D1\85 \D1\80\D0\B0\D1\81\D1\87\D1\91\D1\82 \D0\B8 \D1\80\D0\B8\D1\81\D0\BE\D0\B2\D0\B0\D0\BD\D0\B8\D0\B5:\00", align 1
@"tb.s.f_cubes__main.220" = private unnamed_addr constant [36 x i8] c"\D0\BC\D1\81, \D0\BF\D0\BE\D0\BA\D0\B0\D0\B7 \D0\BD\D0\B0 \D1\8D\D0\BA\D1\80\D0\B0\D0\BD\D0\B5:\00", align 1
@"tb.s.f_cubes__main.222" = private unnamed_addr constant [5 x i8] c"\D0\BC\D1\81\00", align 1
@"tb.s.f_cubes__main.223" = private unnamed_addr constant [2 x i8] c"\0A\00", align 1
declare void @tb_xb_mesh_add(i64, double, double, double, double, double, double, double, double, double, i64, i32)
@"tb.s.f_tb__show3.3" = private unnamed_addr constant [5 x i8] c"true\00", align 1
@"tb.s.f_tb__show3.4" = private unnamed_addr constant [6 x i8] c"false\00", align 1
@"tb.s.f_tb__show5.1" = private unnamed_addr constant [2 x i8] c"[\00", align 1
@"tb.s.f_tb__show5.13" = private unnamed_addr constant [3 x i8] c", \00", align 1
@"tb.s.f_tb__show5.18" = private unnamed_addr constant [2 x i8] c"]\00", align 1
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
