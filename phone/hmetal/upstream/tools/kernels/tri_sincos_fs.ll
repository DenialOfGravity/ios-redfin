; tri_sincos_fs (redfin hm-int4): Metal's sincos with its cosine through a pointer, as PridePoster's updateParticles
; calls air.sincos.f16 (refused by hm-int3: "a pointer where a number belongs"), written as the LLVM IR a Metal compiler
; would emit for
;
;     fragment float4 tri_sincos_fs(constant float &x [[buffer(0)]])
;     {
;       float c;
;       float s = sincos(x, c);
;       return float4(s, c, 0, 1);
;     }
;
; Hand-written, ours, MIT: only the SHAPE of the metadata is Apple's convention (as tri_cflat_fs.ll).

source_filename = "tri_sincos_fs"
target triple = "air64-apple-ios16.5.0"
target triple = "air64-apple-ios16.5.0"

define <4 x float> @tri_sincos_fs(ptr addrspace(2) %b) local_unnamed_addr {
  %c = alloca float, align 4
  %x = load float, ptr addrspace(2) %b, align 4
  %s = call fast float @air.sincos.f32(float %x, ptr nonnull writeonly %c)
  %cv = load float, ptr %c, align 4
  %v0 = insertelement <4 x float> <float 0.0, float 0.0, float 0.0, float 1.0>, float %s, i64 0
  %v1 = insertelement <4 x float> %v0, float %cv, i64 1
  ret <4 x float> %v1
}

declare float @air.sincos.f32(float, ptr)

!air.version = !{!0}
!air.language_version = !{!1}
!air.fragment = !{!2}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @tri_sincos_fs, !3, !5}
!3 = !{!4}
!4 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4"}
!5 = !{!6}
!6 = !{i32 0, !"air.buffer", !"air.buffer_size", i32 4, !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"float", !"air.arg_name", !"c"}
