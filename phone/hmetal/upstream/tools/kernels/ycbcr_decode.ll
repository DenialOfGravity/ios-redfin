; ycbcr_decode (redfin hmyc, HM_YCBCR=native): a two-plane '420f' / '420v' IOSurface (Metal's YCbCr8_420_2P, 500, and
; RGB8_420_2P, 540) read straight from the surface's memory on the GPU -- both planes, no processor copy -- into an RGBA8
; copy that CoreAnimation's shaders sample as RGB (Apple's texture unit converts as it samples; the A6xx TP returns raw
; Y, Cb, Cr, and CoreAnimation's fixed_frag takes a 500 sample as the colour). Four pixels a thread: one word of luma,
; one word of CbCr (two chroma pairs, nearest, the 4:2:0 siting the processor copies use). The AIR a Metal compiler emits for
;
;     struct P { uint w, h, lumaOff, lumaPitch, chromaOff, chromaPitch /*words of SRC*/, dstPitch /*words*/, planes;
;                int ky, kr, kgb, kgr, kb, y0; };
;     kernel void ycbcr_decode(constant P& p [[buffer(0)]], device const uint* src [[buffer(1)]], device uint* dst [[buffer(2)]],
;                              uint2 gid [[thread_position_in_grid]])
;     {
;       uint x = 4 * gid.x;
;       if (x >= p.w || gid.y >= p.h) return;
;       uint l = src[p.lumaOff + gid.y * p.lumaPitch + gid.x], c = src[p.chromaOff + (gid.y >> 1) * p.chromaPitch + gid.x];
;       for (uint k = 0; k < 4; k++) {          // unrolled below
;         int y = l >> 8 * k & 255, cb = c >> 16 * (k >> 1) & 255, cr = c >> 16 * (k >> 1) + 8 & 255;
;         int yy = p.ky * (y - p.y0) + 32768;
;         int r = clamp((yy + p.kr * (cr - 128)) >> 16, 0, 255), g = clamp((yy - p.kgb * (cb - 128) - p.kgr * (cr - 128)) >> 16, 0, 255),
;             b = clamp((yy + p.kb * (cb - 128)) >> 16, 0, 255);
;         uint o = p.planes ? (cr | y << 8 | cb << 16) : (r | g << 8 | b << 16);   // 540: R Cr, G Y, B Cb
;         if (x + k < p.w) dst[gid.y * p.dstPitch + x + k] = o | 0xff000000;
;       }
;     }
;
; The coefficients are Texture::ycbcrCoefficients' 16.16 fixed point (BT.601 / BT.709, full / video range), the same
; integers Texture::ycbcrConvertRows uses on the processor: both paths agree bit for bit. Ours, MIT; the metadata's shape
; is Apple's convention, as in xr_decode.ll. Generated once from the loop above, then kept as the source.

source_filename = "ycbcr_decode"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

define void @ycbcr_decode(ptr addrspace(2) %p, ptr addrspace(1) %src, ptr addrspace(1) %dst, <2 x i32> %gid) local_unnamed_addr {
entry:
  %gx = extractelement <2 x i32> %gid, i32 0
  %gy = extractelement <2 x i32> %gid, i32 1
  %w = load i32, ptr addrspace(2) %p, align 4
  %ph = getelementptr inbounds i32, ptr addrspace(2) %p, i64 1
  %h = load i32, ptr addrspace(2) %ph, align 4
  %plo = getelementptr inbounds i32, ptr addrspace(2) %p, i64 2
  %lo = load i32, ptr addrspace(2) %plo, align 4
  %plp = getelementptr inbounds i32, ptr addrspace(2) %p, i64 3
  %lp = load i32, ptr addrspace(2) %plp, align 4
  %pco = getelementptr inbounds i32, ptr addrspace(2) %p, i64 4
  %co = load i32, ptr addrspace(2) %pco, align 4
  %pcp = getelementptr inbounds i32, ptr addrspace(2) %p, i64 5
  %cp = load i32, ptr addrspace(2) %pcp, align 4
  %pdp = getelementptr inbounds i32, ptr addrspace(2) %p, i64 6
  %dp = load i32, ptr addrspace(2) %pdp, align 4
  %pplanes = getelementptr inbounds i32, ptr addrspace(2) %p, i64 7
  %planes = load i32, ptr addrspace(2) %pplanes, align 4
  %pky = getelementptr inbounds i32, ptr addrspace(2) %p, i64 8
  %ky = load i32, ptr addrspace(2) %pky, align 4
  %pkr = getelementptr inbounds i32, ptr addrspace(2) %p, i64 9
  %kr = load i32, ptr addrspace(2) %pkr, align 4
  %pkgb = getelementptr inbounds i32, ptr addrspace(2) %p, i64 10
  %kgb = load i32, ptr addrspace(2) %pkgb, align 4
  %pkgr = getelementptr inbounds i32, ptr addrspace(2) %p, i64 11
  %kgr = load i32, ptr addrspace(2) %pkgr, align 4
  %pkb = getelementptr inbounds i32, ptr addrspace(2) %p, i64 12
  %kb = load i32, ptr addrspace(2) %pkb, align 4
  %py0 = getelementptr inbounds i32, ptr addrspace(2) %p, i64 13
  %y0 = load i32, ptr addrspace(2) %py0, align 4
  %x = shl i32 %gx, 2
  %inx = icmp ult i32 %x, %w
  %iny = icmp ult i32 %gy, %h
  %in = and i1 %inx, %iny
  br i1 %in, label %body, label %done

body:
  %lrow = mul i32 %gy, %lp
  %li0 = add i32 %lo, %lrow
  %li = add i32 %li0, %gx
  %lz = zext i32 %li to i64
  %lptr = getelementptr inbounds i32, ptr addrspace(1) %src, i64 %lz
  %l = load i32, ptr addrspace(1) %lptr, align 4
  %cy = lshr i32 %gy, 1
  %crow = mul i32 %cy, %cp
  %ci0 = add i32 %co, %crow
  %ci = add i32 %ci0, %gx
  %cz = zext i32 %ci to i64
  %cptr = getelementptr inbounds i32, ptr addrspace(1) %src, i64 %cz
  %c = load i32, ptr addrspace(1) %cptr, align 4
  %orow = mul i32 %gy, %dp
  %obase = add i32 %orow, %x
  %isplanes = icmp ne i32 %planes, 0
  ; pixel 0
  %k0.y = and i32 %l, 255
  %k0.cb = and i32 %c, 255
  %k0.crs = lshr i32 %c, 8
  %k0.cr = and i32 %k0.crs, 255
  %k0.ym = sub i32 %k0.y, %y0
  %k0.yk = mul i32 %k0.ym, %ky
  %k0.yy = add i32 %k0.yk, 32768
  %k0.cbm = sub i32 %k0.cb, 128
  %k0.crm = sub i32 %k0.cr, 128
  %k0.rk = mul i32 %k0.crm, %kr
  %k0.r0 = add i32 %k0.yy, %k0.rk
  %k0.r1 = ashr i32 %k0.r0, 16
  %k0.gb = mul i32 %k0.cbm, %kgb
  %k0.gr = mul i32 %k0.crm, %kgr
  %k0.g0 = sub i32 %k0.yy, %k0.gb
  %k0.g0b = sub i32 %k0.g0, %k0.gr
  %k0.g1 = ashr i32 %k0.g0b, 16
  %k0.bk = mul i32 %k0.cbm, %kb
  %k0.b0 = add i32 %k0.yy, %k0.bk
  %k0.b1 = ashr i32 %k0.b0, 16
  %k0.r1.lo = icmp slt i32 %k0.r1, 0
  %k0.r1.a = select i1 %k0.r1.lo, i32 0, i32 %k0.r1
  %k0.r.hi = icmp sgt i32 %k0.r1.a, 255
  %k0.r = select i1 %k0.r.hi, i32 255, i32 %k0.r1.a
  %k0.g1.lo = icmp slt i32 %k0.g1, 0
  %k0.g1.a = select i1 %k0.g1.lo, i32 0, i32 %k0.g1
  %k0.g.hi = icmp sgt i32 %k0.g1.a, 255
  %k0.g = select i1 %k0.g.hi, i32 255, i32 %k0.g1.a
  %k0.b1.lo = icmp slt i32 %k0.b1, 0
  %k0.b1.a = select i1 %k0.b1.lo, i32 0, i32 %k0.b1
  %k0.b.hi = icmp sgt i32 %k0.b1.a, 255
  %k0.b = select i1 %k0.b.hi, i32 255, i32 %k0.b1.a
  %k0.g8 = shl i32 %k0.g, 8
  %k0.b16 = shl i32 %k0.b, 16
  %k0.rg = or i32 %k0.r, %k0.g8
  %k0.rgb = or i32 %k0.rg, %k0.b16
  %k0.py8 = shl i32 %k0.y, 8
  %k0.pcb16 = shl i32 %k0.cb, 16
  %k0.pa = or i32 %k0.cr, %k0.py8
  %k0.pb = or i32 %k0.pa, %k0.pcb16
  %k0.v = select i1 %isplanes, i32 %k0.pb, i32 %k0.rgb
  %k0.o = or i32 %k0.v, -16777216
  %k0.x = add i32 %x, 0
  %k0.in = icmp ult i32 %k0.x, %w
  br i1 %k0.in, label %k0.st, label %k0.next

k0.st:
  %k0.oi = add i32 %obase, 0
  %k0.oz = zext i32 %k0.oi to i64
  %k0.op = getelementptr inbounds i32, ptr addrspace(1) %dst, i64 %k0.oz
  store i32 %k0.o, ptr addrspace(1) %k0.op, align 4
  br label %k0.next

k0.next:
  ; pixel 1
  %k1.ys = lshr i32 %l, 8
  %k1.y = and i32 %k1.ys, 255
  %k1.cb = and i32 %c, 255
  %k1.crs = lshr i32 %c, 8
  %k1.cr = and i32 %k1.crs, 255
  %k1.ym = sub i32 %k1.y, %y0
  %k1.yk = mul i32 %k1.ym, %ky
  %k1.yy = add i32 %k1.yk, 32768
  %k1.cbm = sub i32 %k1.cb, 128
  %k1.crm = sub i32 %k1.cr, 128
  %k1.rk = mul i32 %k1.crm, %kr
  %k1.r0 = add i32 %k1.yy, %k1.rk
  %k1.r1 = ashr i32 %k1.r0, 16
  %k1.gb = mul i32 %k1.cbm, %kgb
  %k1.gr = mul i32 %k1.crm, %kgr
  %k1.g0 = sub i32 %k1.yy, %k1.gb
  %k1.g0b = sub i32 %k1.g0, %k1.gr
  %k1.g1 = ashr i32 %k1.g0b, 16
  %k1.bk = mul i32 %k1.cbm, %kb
  %k1.b0 = add i32 %k1.yy, %k1.bk
  %k1.b1 = ashr i32 %k1.b0, 16
  %k1.r1.lo = icmp slt i32 %k1.r1, 0
  %k1.r1.a = select i1 %k1.r1.lo, i32 0, i32 %k1.r1
  %k1.r.hi = icmp sgt i32 %k1.r1.a, 255
  %k1.r = select i1 %k1.r.hi, i32 255, i32 %k1.r1.a
  %k1.g1.lo = icmp slt i32 %k1.g1, 0
  %k1.g1.a = select i1 %k1.g1.lo, i32 0, i32 %k1.g1
  %k1.g.hi = icmp sgt i32 %k1.g1.a, 255
  %k1.g = select i1 %k1.g.hi, i32 255, i32 %k1.g1.a
  %k1.b1.lo = icmp slt i32 %k1.b1, 0
  %k1.b1.a = select i1 %k1.b1.lo, i32 0, i32 %k1.b1
  %k1.b.hi = icmp sgt i32 %k1.b1.a, 255
  %k1.b = select i1 %k1.b.hi, i32 255, i32 %k1.b1.a
  %k1.g8 = shl i32 %k1.g, 8
  %k1.b16 = shl i32 %k1.b, 16
  %k1.rg = or i32 %k1.r, %k1.g8
  %k1.rgb = or i32 %k1.rg, %k1.b16
  %k1.py8 = shl i32 %k1.y, 8
  %k1.pcb16 = shl i32 %k1.cb, 16
  %k1.pa = or i32 %k1.cr, %k1.py8
  %k1.pb = or i32 %k1.pa, %k1.pcb16
  %k1.v = select i1 %isplanes, i32 %k1.pb, i32 %k1.rgb
  %k1.o = or i32 %k1.v, -16777216
  %k1.x = add i32 %x, 1
  %k1.in = icmp ult i32 %k1.x, %w
  br i1 %k1.in, label %k1.st, label %k1.next

k1.st:
  %k1.oi = add i32 %obase, 1
  %k1.oz = zext i32 %k1.oi to i64
  %k1.op = getelementptr inbounds i32, ptr addrspace(1) %dst, i64 %k1.oz
  store i32 %k1.o, ptr addrspace(1) %k1.op, align 4
  br label %k1.next

k1.next:
  ; pixel 2
  %k2.ys = lshr i32 %l, 16
  %k2.y = and i32 %k2.ys, 255
  %k2.cbs = lshr i32 %c, 16
  %k2.cb = and i32 %k2.cbs, 255
  %k2.crs = lshr i32 %c, 24
  %k2.cr = and i32 %k2.crs, 255
  %k2.ym = sub i32 %k2.y, %y0
  %k2.yk = mul i32 %k2.ym, %ky
  %k2.yy = add i32 %k2.yk, 32768
  %k2.cbm = sub i32 %k2.cb, 128
  %k2.crm = sub i32 %k2.cr, 128
  %k2.rk = mul i32 %k2.crm, %kr
  %k2.r0 = add i32 %k2.yy, %k2.rk
  %k2.r1 = ashr i32 %k2.r0, 16
  %k2.gb = mul i32 %k2.cbm, %kgb
  %k2.gr = mul i32 %k2.crm, %kgr
  %k2.g0 = sub i32 %k2.yy, %k2.gb
  %k2.g0b = sub i32 %k2.g0, %k2.gr
  %k2.g1 = ashr i32 %k2.g0b, 16
  %k2.bk = mul i32 %k2.cbm, %kb
  %k2.b0 = add i32 %k2.yy, %k2.bk
  %k2.b1 = ashr i32 %k2.b0, 16
  %k2.r1.lo = icmp slt i32 %k2.r1, 0
  %k2.r1.a = select i1 %k2.r1.lo, i32 0, i32 %k2.r1
  %k2.r.hi = icmp sgt i32 %k2.r1.a, 255
  %k2.r = select i1 %k2.r.hi, i32 255, i32 %k2.r1.a
  %k2.g1.lo = icmp slt i32 %k2.g1, 0
  %k2.g1.a = select i1 %k2.g1.lo, i32 0, i32 %k2.g1
  %k2.g.hi = icmp sgt i32 %k2.g1.a, 255
  %k2.g = select i1 %k2.g.hi, i32 255, i32 %k2.g1.a
  %k2.b1.lo = icmp slt i32 %k2.b1, 0
  %k2.b1.a = select i1 %k2.b1.lo, i32 0, i32 %k2.b1
  %k2.b.hi = icmp sgt i32 %k2.b1.a, 255
  %k2.b = select i1 %k2.b.hi, i32 255, i32 %k2.b1.a
  %k2.g8 = shl i32 %k2.g, 8
  %k2.b16 = shl i32 %k2.b, 16
  %k2.rg = or i32 %k2.r, %k2.g8
  %k2.rgb = or i32 %k2.rg, %k2.b16
  %k2.py8 = shl i32 %k2.y, 8
  %k2.pcb16 = shl i32 %k2.cb, 16
  %k2.pa = or i32 %k2.cr, %k2.py8
  %k2.pb = or i32 %k2.pa, %k2.pcb16
  %k2.v = select i1 %isplanes, i32 %k2.pb, i32 %k2.rgb
  %k2.o = or i32 %k2.v, -16777216
  %k2.x = add i32 %x, 2
  %k2.in = icmp ult i32 %k2.x, %w
  br i1 %k2.in, label %k2.st, label %k2.next

k2.st:
  %k2.oi = add i32 %obase, 2
  %k2.oz = zext i32 %k2.oi to i64
  %k2.op = getelementptr inbounds i32, ptr addrspace(1) %dst, i64 %k2.oz
  store i32 %k2.o, ptr addrspace(1) %k2.op, align 4
  br label %k2.next

k2.next:
  ; pixel 3
  %k3.ys = lshr i32 %l, 24
  %k3.y = and i32 %k3.ys, 255
  %k3.cbs = lshr i32 %c, 16
  %k3.cb = and i32 %k3.cbs, 255
  %k3.crs = lshr i32 %c, 24
  %k3.cr = and i32 %k3.crs, 255
  %k3.ym = sub i32 %k3.y, %y0
  %k3.yk = mul i32 %k3.ym, %ky
  %k3.yy = add i32 %k3.yk, 32768
  %k3.cbm = sub i32 %k3.cb, 128
  %k3.crm = sub i32 %k3.cr, 128
  %k3.rk = mul i32 %k3.crm, %kr
  %k3.r0 = add i32 %k3.yy, %k3.rk
  %k3.r1 = ashr i32 %k3.r0, 16
  %k3.gb = mul i32 %k3.cbm, %kgb
  %k3.gr = mul i32 %k3.crm, %kgr
  %k3.g0 = sub i32 %k3.yy, %k3.gb
  %k3.g0b = sub i32 %k3.g0, %k3.gr
  %k3.g1 = ashr i32 %k3.g0b, 16
  %k3.bk = mul i32 %k3.cbm, %kb
  %k3.b0 = add i32 %k3.yy, %k3.bk
  %k3.b1 = ashr i32 %k3.b0, 16
  %k3.r1.lo = icmp slt i32 %k3.r1, 0
  %k3.r1.a = select i1 %k3.r1.lo, i32 0, i32 %k3.r1
  %k3.r.hi = icmp sgt i32 %k3.r1.a, 255
  %k3.r = select i1 %k3.r.hi, i32 255, i32 %k3.r1.a
  %k3.g1.lo = icmp slt i32 %k3.g1, 0
  %k3.g1.a = select i1 %k3.g1.lo, i32 0, i32 %k3.g1
  %k3.g.hi = icmp sgt i32 %k3.g1.a, 255
  %k3.g = select i1 %k3.g.hi, i32 255, i32 %k3.g1.a
  %k3.b1.lo = icmp slt i32 %k3.b1, 0
  %k3.b1.a = select i1 %k3.b1.lo, i32 0, i32 %k3.b1
  %k3.b.hi = icmp sgt i32 %k3.b1.a, 255
  %k3.b = select i1 %k3.b.hi, i32 255, i32 %k3.b1.a
  %k3.g8 = shl i32 %k3.g, 8
  %k3.b16 = shl i32 %k3.b, 16
  %k3.rg = or i32 %k3.r, %k3.g8
  %k3.rgb = or i32 %k3.rg, %k3.b16
  %k3.py8 = shl i32 %k3.y, 8
  %k3.pcb16 = shl i32 %k3.cb, 16
  %k3.pa = or i32 %k3.cr, %k3.py8
  %k3.pb = or i32 %k3.pa, %k3.pcb16
  %k3.v = select i1 %isplanes, i32 %k3.pb, i32 %k3.rgb
  %k3.o = or i32 %k3.v, -16777216
  %k3.x = add i32 %x, 3
  %k3.in = icmp ult i32 %k3.x, %w
  br i1 %k3.in, label %k3.st, label %k3.next

k3.st:
  %k3.oi = add i32 %obase, 3
  %k3.oz = zext i32 %k3.oi to i64
  %k3.op = getelementptr inbounds i32, ptr addrspace(1) %dst, i64 %k3.oz
  store i32 %k3.o, ptr addrspace(1) %k3.op, align 4
  br label %k3.next

k3.next:
  br label %done

done:
  ret void
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.kernel = !{!2}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @ycbcr_decode, !3, !4}
!3 = !{}
!4 = !{!5, !6, !7, !8}
!5 = !{i32 0, !"air.buffer", !"air.buffer_size", i32 56, !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 56, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"P", !"air.arg_name", !"p"}
!6 = !{i32 1, !"air.buffer", !"air.location_index", i32 1, i32 1, !"air.read", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"src"}
!7 = !{i32 2, !"air.buffer", !"air.location_index", i32 2, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"dst"}
!8 = !{i32 3, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint2", !"air.arg_name", !"gid"}
