; xr_encode (redfin hm-int5, HM_XR_WB=gpu): an extended-range target's RGBA16Float companion written back into its
; IOSurface's rows (BGR10_XR / BGRA10_XR, 'w30r' / '&w4a') on the GPU, the Texture::xrWriteBack loop as a kernel. The
; AIR a Metal compiler emits for
;
;     struct P { uint w, h, srcPitch /*companion words*/, dstPitch /*surface words*/, wide /*0: 4-byte BGR10_XR, 1: 8-byte BGRA10_XR*/,
;                lut /*0 | 65536*/; };
;     kernel void xr_encode(constant P& p [[buffer(0)]], device const uint* src [[buffer(1)]], device uint* dst [[buffer(2)]],
;                           device const uint* tab [[buffer(3)]], uint2 gid [[thread_position_in_grid]])
;     {
;       if (gid.x >= p.w || gid.y >= p.h) return;
;       uint i = gid.y * p.srcPitch + 2 * gid.x;
;       uint lo = src[i], hi = src[i + 1];                      // R | G << 16, B | A << 16 (halves)
;       uint r = tab[p.lut + (lo & 0xffff)], g = tab[p.lut + (lo >> 16)], b = tab[p.lut + (hi & 0xffff)];
;       if (p.wide == 0) { dst[gid.y * p.dstPitch + gid.x] = b | g << 10 | r << 20; }
;       else { uint a = tab[hi >> 16]; uint o = gid.y * p.dstPitch + 2 * gid.x;
;              dst[o] = b << 6 | g << 22; dst[o + 1] = r << 6 | a << 22; }
;     }
;
; TAB: Texture::xrEncodeTable's 131072 words: the 10-bit code of every half (the linear curve, then the _sRGB twin's),
; the processor write-back's tables, so both agree bit for bit (alpha through the linear one, as the processor's).
; Hand-written, ours, MIT; the metadata's shape is Apple's convention, as in xr_decode.ll.

source_filename = "xr_encode"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

define void @xr_encode(ptr addrspace(2) %p, ptr addrspace(1) %src, ptr addrspace(1) %dst, ptr addrspace(1) %tab, <2 x i32> %gid) local_unnamed_addr {
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
  %srow = mul i32 %gy, %sp
  %sx = shl i32 %gx, 1
  %si = add i32 %srow, %sx
  %sz = zext i32 %si to i64
  %spp = getelementptr inbounds i32, ptr addrspace(1) %src, i64 %sz
  %lo = load i32, ptr addrspace(1) %spp, align 4
  %spp1 = getelementptr inbounds i32, ptr addrspace(1) %spp, i64 1
  %hi = load i32, ptr addrspace(1) %spp1, align 4
  %hr = and i32 %lo, 65535
  %hg = lshr i32 %lo, 16
  %hb = and i32 %hi, 65535
  %ri = add i32 %lut, %hr
  %gi = add i32 %lut, %hg
  %bi = add i32 %lut, %hb
  %rz = zext i32 %ri to i64
  %gz = zext i32 %gi to i64
  %bz = zext i32 %bi to i64
  %rp = getelementptr inbounds i32, ptr addrspace(1) %tab, i64 %rz
  %gp = getelementptr inbounds i32, ptr addrspace(1) %tab, i64 %gz
  %bp = getelementptr inbounds i32, ptr addrspace(1) %tab, i64 %bz
  %r = load i32, ptr addrspace(1) %rp, align 4
  %g = load i32, ptr addrspace(1) %gp, align 4
  %b = load i32, ptr addrspace(1) %bp, align 4
  %drow = mul i32 %gy, %dp
  %narrow = icmp eq i32 %wide, 0
  br i1 %narrow, label %n, label %wd

n:
  %g10 = shl i32 %g, 10
  %r20 = shl i32 %r, 20
  %bg = or i32 %b, %g10
  %v = or i32 %bg, %r20
  %ni = add i32 %drow, %gx
  %nz = zext i32 %ni to i64
  %np = getelementptr inbounds i32, ptr addrspace(1) %dst, i64 %nz
  store i32 %v, ptr addrspace(1) %np, align 4
  br label %done

wd:
  %ha = lshr i32 %hi, 16
  %az = zext i32 %ha to i64
  %ap = getelementptr inbounds i32, ptr addrspace(1) %tab, i64 %az
  %a = load i32, ptr addrspace(1) %ap, align 4
  %b6 = shl i32 %b, 6
  %g22 = shl i32 %g, 22
  %w0 = or i32 %b6, %g22
  %r6 = shl i32 %r, 6
  %a22 = shl i32 %a, 22
  %w1 = or i32 %r6, %a22
  %dx = shl i32 %gx, 1
  %di = add i32 %drow, %dx
  %dz = zext i32 %di to i64
  %dpp = getelementptr inbounds i32, ptr addrspace(1) %dst, i64 %dz
  store i32 %w0, ptr addrspace(1) %dpp, align 4
  %dpp1 = getelementptr inbounds i32, ptr addrspace(1) %dpp, i64 1
  store i32 %w1, ptr addrspace(1) %dpp1, align 4
  br label %done

done:
  ret void
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.kernel = !{!2}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @xr_encode, !3, !4}
!3 = !{}
!4 = !{!5, !6, !7, !8, !9}
!5 = !{i32 0, !"air.buffer", !"air.buffer_size", i32 24, !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 24, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"P", !"air.arg_name", !"p"}
!6 = !{i32 1, !"air.buffer", !"air.location_index", i32 1, i32 1, !"air.read", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"src"}
!7 = !{i32 2, !"air.buffer", !"air.location_index", i32 2, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"dst"}
!8 = !{i32 3, !"air.buffer", !"air.location_index", i32 3, i32 1, !"air.read", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"tab"}
!9 = !{i32 4, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint2", !"air.arg_name", !"gid"}
