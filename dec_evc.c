/*
 *			GPAC - Multimedia Framework C SDK
 *
 *  This file is part of GPAC / MPEG-5 EVC decoder filter, based on xevd.
 *
 *  EVC (MPEG-5 Part 1, "Essential Video Coding") test material comes as a raw
 *  elementary stream, like AVS2 and AVS3 next door - but where those two
 *  delimit their units with MPEG start codes, an EVC stream is a plain
 *  sequence of NAL units each prefixed by its own 4-byte length. Rather than
 *  read that length by hand and guess its byte order, the filter asks
 *  xevd_info(), which is part of the public API and is what xevd's own
 *  application uses.
 *
 *  The decoder is created with threads = 1. Unlike davs2, xevd degrades to a
 *  genuine single-threaded path there: every loop over the worker tasks is
 *  bounded by the task count, so with one task the work happens inline.
 *
 *  xevd decodes internally at 10 bits even for an 8-bit stream, so what comes
 *  out of xevd_pull can be 16 bits per sample; it is brought down to the 8
 *  the pid carries the same way xevd's application does - round to nearest,
 *  then clip.
 */

#include <gpac/filters.h>
#include <gpac/constants.h>
#include <string.h>
#include <stdlib.h>

#include <xevd.h>

typedef struct
{
	GF_FilterPid *ipid, *opid;
	u32 width, height, timescale, frame_dur;
	u64 next_cts;
	Bool cfg_done;
} GF_EVCDecCtx;

static GF_Err evcdec_configure_pid(GF_Filter *filter, GF_FilterPid *pid, Bool is_remove)
{
	GF_EVCDecCtx *ctx = (GF_EVCDecCtx *)gf_filter_get_udta(filter);

	if (is_remove)
	{
		if (ctx->opid)
		{
			gf_filter_pid_remove(ctx->opid);
			ctx->opid = NULL;
		}
		ctx->ipid = NULL;
		return GF_OK;
	}
	if (!gf_filter_pid_check_caps(pid))
		return GF_NOT_SUPPORTED;

	ctx->ipid = pid;
	gf_filter_pid_set_framing_mode(pid, GF_TRUE);

	if (!ctx->opid)
		ctx->opid = gf_filter_pid_new(filter);

	/* Corrected from the first decoded picture; GPAC resolves the graph from
	 * these before any of the stream has been looked at. */
	ctx->width = 320;
	ctx->height = 180;
	ctx->timescale = 25;
	ctx->frame_dur = 1;
	ctx->next_cts = 0;
	ctx->cfg_done = GF_FALSE;

	gf_filter_pid_copy_properties(ctx->opid, pid);
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_STREAM_TYPE, &PROP_UINT(GF_STREAM_VISUAL));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_CODECID, &PROP_UINT(GF_CODECID_RAW));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_PIXFMT, &PROP_UINT(GF_PIXEL_YUV));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_WIDTH, &PROP_UINT(ctx->width));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_HEIGHT, &PROP_UINT(ctx->height));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_STRIDE, &PROP_UINT(ctx->width));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_TIMESCALE, &PROP_UINT(ctx->timescale));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_FPS, &PROP_FRAC_INT(ctx->timescale, ctx->frame_dur));

	return GF_OK;
}

/* One plane of a decoded picture, cropped and reduced to 8 bits, copied row by
 * row into the packed buffer the pid expects. */
static void evcdec_copy_plane(u8 *dst, XEVD_IMGB *img, u32 plane, u32 w, u32 h,
                              u32 crop_l, u32 crop_t, int bd16)
{
	u32 i, j;
	const u8 *row = (const u8 *)img->a[plane]
	                + (size_t)img->s[plane] * (img->y[plane] + crop_t)
	                + (size_t)(img->x[plane] + crop_l) * (bd16 ? 2 : 1);

	for (i = 0; i < h; i++)
	{
		if (bd16)
		{
			/* xevd's own application rounds to nearest and clips; anything
			 * else would shift the picture by half a level. */
			const s16 *s = (const s16 *)row;
			for (j = 0; j < w; j++)
			{
				int t = (s[j] + 2) >> 2;
				dst[j] = (u8)((t < 0) ? 0 : ((t > 255) ? 255 : t));
			}
		}
		else
		{
			memcpy(dst, row, w);
		}
		dst += w;
		row += img->s[plane];
	}
}

static GF_Err evcdec_send_frame(GF_EVCDecCtx *ctx, XEVD_IMGB *img)
{
	GF_FilterPacket *dst_pck;
	u8 *output;
	u32 w, h, cl, ct;
	int bd16;

	if (!img || !img->a[0])
		return GF_OK;
	if (XEVD_CS_GET_FORMAT(img->cs) != XEVD_CF_YCBCR420)
	{
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[EVCDec] Only 4:2:0 is handled (colour format %d)\n",
		                                    XEVD_CS_GET_FORMAT(img->cs)));
		return GF_NOT_SUPPORTED;
	}
	bd16 = (XEVD_CS_GET_BIT_DEPTH(img->cs) > 8) ? 1 : 0;

	/* The conformance window is expressed in chroma samples, so the luma crop
	 * is twice what the picture header carries - the same doubling xevd's
	 * imgb_write does. */
	cl = (u32)img->crop_l * 2;
	ct = (u32)img->crop_t * 2;
	w = (u32)img->w[0] - cl - (u32)img->crop_r * 2;
	h = (u32)img->h[0] - ct - (u32)img->crop_b * 2;
	if (!w || !h)
		return GF_OK;

	if (!ctx->cfg_done || (w != ctx->width) || (h != ctx->height))
	{
		ctx->width = w;
		ctx->height = h;
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_WIDTH, &PROP_UINT(w));
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_HEIGHT, &PROP_UINT(h));
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_STRIDE, &PROP_UINT(w));
		ctx->cfg_done = GF_TRUE;
	}

	dst_pck = gf_filter_pck_new_alloc(ctx->opid, w * h * 3 / 2, &output);
	if (!dst_pck)
		return GF_OUT_OF_MEM;

	evcdec_copy_plane(output, img, 0, w, h, cl, ct, bd16);
	evcdec_copy_plane(output + (size_t)w * h, img, 1, w / 2, h / 2,
	                  (u32)img->crop_l, (u32)img->crop_t, bd16);
	evcdec_copy_plane(output + (size_t)w * h + (size_t)(w / 2) * (h / 2), img, 2,
	                  w / 2, h / 2, (u32)img->crop_l, (u32)img->crop_t, bd16);

	gf_filter_pck_set_cts(dst_pck, ctx->next_cts);
	gf_filter_pck_set_duration(dst_pck, ctx->frame_dur);
	gf_filter_pck_set_sap(dst_pck, GF_FILTER_SAP_1);
	gf_filter_pck_send(dst_pck);
	ctx->next_cts += ctx->frame_dur;
	return GF_OK;
}

static GF_Err evcdec_process(GF_Filter *filter)
{
	GF_FilterPacket *pck;
	u8 *data;
	u32 size, pos;
	XEVD id = NULL;
	XEVD_CDSC cdsc;
	XEVD_BITB bitb;
	XEVD_STAT stat;
	XEVD_IMGB *img = NULL;
	int err = XEVD_OK;
	GF_Err e = GF_OK;
	GF_EVCDecCtx *ctx = (GF_EVCDecCtx *)gf_filter_get_udta(filter);

	pck = gf_filter_pid_get_packet(ctx->ipid);
	if (!pck)
	{
		if (gf_filter_pid_is_eos(ctx->ipid))
		{
			gf_filter_pid_set_eos(ctx->opid);
			return GF_EOS;
		}
		return GF_OK;
	}
	data = (u8 *)gf_filter_pck_get_data(pck, &size);
	if (!data || (size < XEVD_NAL_UNIT_LENGTH_BYTE))
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[EVCDec] File too short to hold a NAL unit\n"));
		return GF_NON_COMPLIANT_BITSTREAM;
	}

	memset(&cdsc, 0, sizeof(cdsc));
	cdsc.threads = 1; /* side modules are single-threaded */
	id = xevd_create(&cdsc, &err);
	if (!id)
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[EVCDec] Could not create the decoder (%d)\n", err));
		return GF_NOT_SUPPORTED;
	}

	pos = 0;
	while ((pos + XEVD_NAL_UNIT_LENGTH_BYTE) <= size)
	{
		XEVD_INFO info;
		u32 nalu_len;

		memset(&info, 0, sizeof(info));
		if (XEVD_FAILED(xevd_info(data + pos, XEVD_NAL_UNIT_LENGTH_BYTE, 1, &info)))
		{
			GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[EVCDec] Cannot read the NAL unit length at offset %u\n", pos));
			e = GF_NON_COMPLIANT_BITSTREAM;
			break;
		}
		if (info.nalu_len <= 0)
			break;
		nalu_len = (u32)info.nalu_len;
		if ((pos + XEVD_NAL_UNIT_LENGTH_BYTE + nalu_len) > size)
		{
			GF_LOG(GF_LOG_WARNING, GF_LOG_CODEC, ("[EVCDec] Truncated NAL unit at offset %u\n", pos));
			break;
		}

		memset(&bitb, 0, sizeof(bitb));
		memset(&stat, 0, sizeof(stat));
		bitb.addr = data + pos + XEVD_NAL_UNIT_LENGTH_BYTE;
		bitb.ssize = (int)nalu_len;
		bitb.bsize = (int)nalu_len;

		if (XEVD_FAILED(xevd_decode(id, &bitb, &stat)))
		{
			GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[EVCDec] Failed to decode the NAL unit at offset %u\n", pos));
			e = GF_NON_COMPLIANT_BITSTREAM;
			break;
		}

		/* fnum is negative when the unit carried no picture */
		if (stat.fnum >= 0)
		{
			img = NULL;
			if (XEVD_SUCCEEDED(xevd_pull(id, &img)) && img)
			{
				e = evcdec_send_frame(ctx, img);
				if (img->release)
					img->release(img);
				if (e != GF_OK)
					break;
			}
		}
		pos += XEVD_NAL_UNIT_LENGTH_BYTE + nalu_len;
	}

	/* "bumping": the pictures the decoder was still holding back for reorder */
	while (e == GF_OK)
	{
		img = NULL;
		if (XEVD_FAILED(xevd_pull(id, &img)) || !img)
			break;
		e = evcdec_send_frame(ctx, img);
		if (img->release)
			img->release(img);
	}

	xevd_delete(id);
	gf_filter_pid_drop_packet(ctx->ipid);

	if (e != GF_OK)
		return e;
	if (!ctx->cfg_done)
	{
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[EVCDec] No frame decoded\n"));
		return GF_NON_COMPLIANT_BITSTREAM;
	}
	gf_filter_pid_set_eos(ctx->opid);
	return GF_EOS;
}

static void evcdec_finalize(GF_Filter *filter)
{
}

static const GF_FilterCapability EVCDecCaps[] =
	{
		CAP_UINT(GF_CAPS_INPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_FILE),
		CAP_STRING(GF_CAPS_INPUT, GF_PROP_PID_FILE_EXT, "evc"),
		CAP_STRING(GF_CAPS_INPUT, GF_PROP_PID_MIME, "video/evc|video/x-evc"),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_VISUAL),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_CODECID, GF_CODECID_RAW),
};

GF_FilterRegister EVCDecoderRegister = {
	.name = "evcdec",
	GF_FS_SET_DESCRIPTION("MPEG-5 EVC (Essential Video Coding) decoder")
		GF_FS_SET_HELP("This filter decodes raw EVC elementary streams - length-prefixed NAL units, as xeve writes them - using xevd, emitting one raw YUV 4:2:0 frame per picture.")
			.private_size = sizeof(GF_EVCDecCtx),
	SETCAPS(EVCDecCaps),
	.configure_pid = evcdec_configure_pid,
	.process = evcdec_process,
	.finalize = evcdec_finalize,
};

const GF_FilterRegister *EMSCRIPTEN_KEEPALIVE evcdec_register(GF_FilterSession *session)
{
	return &EVCDecoderRegister;
}

#include "filter_register.h"
__attribute__((constructor))
void register_evcdec(void) {
    gf_filter_auto_register("evcdec", evcdec_register);
}
