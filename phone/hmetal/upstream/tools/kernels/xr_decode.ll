; xr_decode (redfin hmv11, HM_XR_GPU): an extended-range IOSurface's rows (BGR10_XR / BGRA10_XR, 'w30r' / 'l10r'
; WebKit tiles) decoded on the GPU into an RGBA16Float copy's rows, the Texture::xrDecodeInto loop as a kernel. The
; AIR a Metal compiler emits for
;
;     struct P { uint w, h, srcPitch /*words*/, dstPitch /*words*/, wide /*0: 4-byte BGR10_XR, 1: 8-byte BGRA10_XR*/, lut /*0 | 1024*/; };
;     kernel void xr_decode(constant P& p [[buffer(0)]], device const uint* src [[buffer(1)]], device uint* dst [[buffer(2)]],
;                           device const uint* tab [[buffer(3)]], uint2 gid [[thread_position_in_grid]])
;     {
;       if (gid.x >= p.w || gid.y >= p.h) return;
;       uint r, g, b, a = 0x3c00;
;       if (p.wide == 0) { uint v = src[gid.y * p.srcPitch + gid.x]; r = v >> 20 & 1023; g = v >> 10 & 1023; b = v & 1023; }
;       else { uint lo = src[gid.y * p.srcPitch + 2 * gid.x], hi = src[gid.y * p.srcPitch + 2 * gid.x + 1];
;              b = lo >> 6 & 1023; g = lo >> 22; r = hi >> 6 & 1023; a = tab[2048 + (hi >> 22)]; }
;       uint o = gid.y * p.dstPitch + 2 * gid.x;
;       dst[o] = tab[p.lut + r] | tab[p.lut + g] << 16;
;       dst[o + 1] = tab[p.lut + b] | a << 16;
;     }
;
; TAB: Texture::xrGpuTable's 3072 words, each a half in its low 16 bits: the codes' linear values, the _sRGB twins'
; (the sRGB curve extended by symmetry), alpha clamped to [0, 1] -- the processor decode's tables, so both agree bit for bit.
; Hand-written, ours, MIT; the metadata's shape is Apple's convention, as in tex_fill.ll.

source_filename = "xr_decode"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

define void @xr_decode(ptr addrspace(2) %p, ptr addrspace(1) %src, ptr addrspace(1) %dst, ptr addrspace(1) %tab, <2 x i32> %gid) local_unnamed_addr {
entry:
  %gx = extractelement <2 x i32> %gid, i32 0
  %gy = extractelement <2 x i32> %gid, i32 1
  %w = load i32, ptr addrspace(2) %p, align 4
  %ph = getelementptr inbounds i32, ptr addrspace(2) %p, i64 1
  %h = load i32, ptr addrspace(2) %ph, align 4
  %psp = getelementptr inbounds i32, ptr addrspace(2) %p, i64 2
  %sp = load i32, ptr addrspace(2) %psp, align 4
  %pdp = getelementptr inbounds i32, ptr addrspace(2) %p, i64 3
  %dp = load i32, ptr addrspace(2) %pdp, align 4
  %pwide = getelementptr inbounds i32, ptr addrspace(2) %p, i64 4
  %wide = load i32, ptr addrspace(2) %pwide, align 4
  %plut = getelementptr inbounds i32, ptr addrspace(2) %p, i64 5
  %lut = load i32, ptr addrspace(2) %plut, align 4
  %inx = icmp ult i32 %gx, %w
  %iny = icmp ult i32 %gy, %h
  %in = and i1 %inx, %iny
  br i1 %in, label %body, label %done

body:
  %row = mul i32 %gy, %sp
  %narrow = icmp eq i32 %wide, 0
  br i1 %narrow, label %n, label %wd

n:
  %ni = add i32 %row, %gx
  %nz = zext i32 %ni to i64
  %np = getelementptr inbounds i32, ptr addrspace(1) %src, i64 %nz
  %v = load i32, ptr addrspace(1) %np, align 4
  %nr0 = lshr i32 %v, 20
  %nr = and i32 %nr0, 1023
  %ng0 = lshr i32 %v, 10
  %ng = and i32 %ng0, 1023
  %nb = and i32 %v, 1023
  br label %out

wd:
  %x2 = shl i32 %gx, 1
  %wi = add i32 %row, %x2
  %wz = zext i32 %wi to i64
  %wp = getelementptr inbounds i32, ptr addrspace(1) %src, i64 %wz
  %lo = load i32, ptr addrspace(1) %wp, align 4
  %wp1 = getelementptr inbounds i32, ptr addrspace(1) %wp, i64 1
  %hi = load i32, ptr addrspace(1) %wp1, align 4
  %wb0 = lshr i32 %lo, 6
  %wb = and i32 %wb0, 1023
  %wg = lshr i32 %lo, 22
  %wr0 = lshr i32 %hi, 6
  %wr = and i32 %wr0, 1023
  %wa0 = lshr i32 %hi, 22
  %wai = add i32 %wa0, 2048
  %waz = zext i32 %wai to i64
  %wap = getelementptr inbounds i32, ptr addrspace(1) %tab, i64 %waz
  %wa = load i32, ptr addrspace(1) %wap, align 4
  br label %out

out:
  %r = phi i32 [ %nr, %n ], [ %wr, %wd ]
  %g = phi i32 [ %ng, %n ], [ %wg, %wd ]
  %b = phi i32 [ %nb, %n ], [ %wb, %wd ]
  %a = phi i32 [ 15360, %n ], [ %wa, %wd ]
  %ri = add i32 %lut, %r
  %gi = add i32 %lut, %g
  %bi = add i32 %lut, %b
  %rz = zext i32 %ri to i64
  %gz = zext i32 %gi to i64
  %bz = zext i32 %bi to i64
  %rp = getelementptr inbounds i32, ptr addrspace(1) %tab, i64 %rz
  %gp = getelementptr inbounds i32, ptr addrspace(1) %tab, i64 %gz
  %bp = getelementptr inbounds i32, ptr addrspace(1) %tab, i64 %bz
  %hr = load i32, ptr addrspace(1) %rp, align 4
  %hg = load i32, ptr addrspace(1) %gp, align 4
  %hb = load i32, ptr addrspace(1) %bp, align 4
  %hg16 = shl i32 %hg, 16
  %a16 = shl i32 %a, 16
  %w0 = or i32 %hr, %hg16
  %w1 = or i32 %hb, %a16
  %orow = mul i32 %gy, %dp
  %ox = shl i32 %gx, 1
  %oi = add i32 %orow, %ox
  %oz = zext i32 %oi to i64
  %op = getelementptr inbounds i32, ptr addrspace(1) %dst, i64 %oz
  store i32 %w0, ptr addrspace(1) %op, align 4
  %op1 = getelementptr inbounds i32, ptr addrspace(1) %op, i64 1
  store i32 %w1, ptr addrspace(1) %op1, align 4
  br label %done

done:
  ret void
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.kernel = !{!2}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @xr_decode, !3, !4}
!3 = !{}
!4 = !{!5, !6, !7, !8, !9}
!5 = !{i32 0, !"air.buffer", !"air.buffer_size", i32 24, !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 24, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"P", !"air.arg_name", !"p"}
!6 = !{i32 1, !"air.buffer", !"air.location_index", i32 1, i32 1, !"air.read", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"src"}
!7 = !{i32 2, !"air.buffer", !"air.location_index", i32 2, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"dst"}
!8 = !{i32 3, !"air.buffer", !"air.location_index", i32 3, i32 1, !"air.read", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"tab"}
!9 = !{i32 4, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint2", !"air.arg_name", !"gid"}
