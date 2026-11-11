/*
 * venusd protocol (video 4a, docs/features/video.md "Stage 4 spec"): /var/run/venusd.sock, AF_UNIX stream, mode 0666.
 * Every message, both ways: struct venusd_msg (96 bytes, little endian, magic VENUSD_MAGIC, version 2; vdcredit), then
 * len payload bytes. One decode session at a time, one per connection:
 *   OPEN    c->d  codec (VD_CODEC_H264; 0 counts as H.264 too; 4e: VD_CODEC_HEVC, 8-bit 4:2:0 Main only), width/height
 *                 = a size hint (0 = from the stream's SPS, else 1920x1088).
 *                 Powers Venus up and boots the firmware if it is down (~1.1 s), then replies OPEN, status 0.
 *                 -EBUSY: another connection has the session; -ENODEV: /var/local/venus-off exists; -EIO: the bring-up
 *                 failed (venusd log); -EINVAL: bad codec (or HEVC with VENUSD_HEVC=0). The HFI session itself starts
 *                 with the first FRAME; an HEVC SPS that isn't 8-bit 4:2:0 Main there: ERROR -ENOTSUP.
 *   FRAME   c->d  one access unit, Annex B (start codes), pts (echoed in DECODED), seq (the client's frame number,
 *                 echoed in ERROR). No reply when queued. venusd reads the next message only once an input buffer is
 *                 free, so a sender blocks: that is the backpressure.
 *   DECODED d->c  one picture in display order: pts, flags (HFI FBD flags), width/height/crop_x/crop_y = the visible
 *                 rect, stride/scanlines = the layout of the payload: the Venus OUTPUT2 buffer as read, NV12 with Y
 *                 stride * scanlines bytes, then interleaved CbCr stride * scanlines / 2 (len = stride * scanlines *
 *                 3 / 2). filled, pic_type, fbd_* = the FBD's words (as venusctl prints them); seq = picture number;
 *                 copy_us = venusd's copy-out of that buffer.
 *                 vdcredit: surface != 0 (len 0): written into that credited IOSurface instead: the visible rect only,
 *                 plane 0 Y and plane 1 CbCr at the surface's own bytes per row (stride/scanlines still Venus's).
 *   CREDIT  c->d  vdcredit (2026-11-03): surface = an IOSurface ID (2-plane 420v/420f at the picture's visible size)
 *                 venusd may fill with one picture; width/height/stride = the client's idea of its size and plane 0
 *                 bytes per row (logged only: venusd checks the surface itself). No reply. A credit is used at most
 *                 once (DECODED with that surface) or handed back (UNCREDIT d->c); it ends at UNCREDIT, EOS_DONE,
 *                 ERROR, CLOSE and a disconnect, after which venusd never writes that surface. Outside a session
 *                 (none, failed, eos done) it is ignored. Up to VENUSD_MAX_CREDITS queued, used oldest first.
 *   UNCREDIT c->d all credits back: reply UNCREDIT surface 0, frames = how many were still queued (every DECODED with
 *                 a surface comes before it). d->c with surface != 0: that credit handed back unused, status -EINVAL
 *                 (the surface doesn't fit the picture width x height), -ENOSPC (queue full) or -ENOTSUP (venusd's
 *                 credit path is off, VENUSD_SURF=0: send no more this session).
 *   EOS     c->d  no more input: an empty EOS buffer, then the remaining pictures, then EOS_DONE (frames = pictures
 *                 sent in the session; status -ETIMEDOUT after 3 s without firmware progress). FRAME after EOS: -EINVAL.
 *   CLOSE   c->d  ends the session (flush, stop, release, session end): reply CLOSE, frames. A disconnect does the same.
 *   ERROR   d->c  status: < 0 a negative errno, > 0 the firmware's session error code (HFI EVENT_NOTIFY
 *                 SESSION_ERROR data1, e.g. 0x1004), seq = the FRAME it is about (or 0). After any ERROR but -E2BIG
 *                 (access unit longer than the input buffers: dropped) the session is closed: only CLOSE is left.
 * Venus stays up for VENUSD_IDLE_S (60) seconds after the last session, then venusd shuts it down (PAS shutdown,
 * SMRs, then power off; a failed shutdown leaves it powered: venusd refuses OPEN with -EIO from then on).
 */
#ifndef VENUS_PROTO_H
#define VENUS_PROTO_H
#include <stdint.h>

#ifndef VENUSD_SOCK	/* -D for a host test build */
#define VENUSD_SOCK	"/var/run/venusd.sock"
#endif
#define VENUSD_MAGIC	0x444e4556u	/* "VEND" */
#define VENUSD_VERSION	2	/* 2: vdcredit (CREDIT, UNCREDIT, DECODED surface) */
#define VENUSD_MAX_AU	(8u << 20)	/* longest FRAME payload venusd reads */
#define VENUSD_MAX_CREDITS 16		/* vdcredit: credits queued per session */

enum { VD_OPEN = 1, VD_FRAME, VD_DECODED, VD_EOS, VD_EOS_DONE, VD_CLOSE, VD_ERROR, VD_CREDIT, VD_UNCREDIT };
#define VD_CODEC_H264	1
#define VD_CODEC_HEVC	2	/* 4e; FRAME payloads are HEVC Annex B access units (VPS/SPS/PPS in band) */

struct venusd_msg {
	uint32_t magic;
	uint16_t type, version;
	uint32_t len;		/* payload bytes after this header */
	int32_t status;		/* replies: 0 ok, < 0 -errno, > 0 firmware session error */
	uint64_t pts;
	uint32_t flags;
	uint32_t codec, width, height;
	uint32_t stride, scanlines, crop_x, crop_y;
	uint32_t filled, pic_type, seq, frames;
	uint32_t fbd_w, fbd_h, fbd_x, fbd_y;
	uint32_t copy_us, surface;	/* vdcredit: an IOSurface ID (CREDIT, UNCREDIT, DECODED), else 0 (was rsvd) */
};
_Static_assert(sizeof(struct venusd_msg) == 96, "venusd_msg");

#endif
