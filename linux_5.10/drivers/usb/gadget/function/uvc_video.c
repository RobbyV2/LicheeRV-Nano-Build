// SPDX-License-Identifier: GPL-2.0+
/*
 *	uvc_video.c  --  USB Video Class Gadget driver
 *
 *	Copyright (C) 2009-2010
 *	    Laurent Pinchart (laurent.pinchart@ideasonboard.com)
 */

#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/moduleparam.h>
#include <linux/errno.h>
#include <linux/usb/ch9.h>
#include <linux/usb/gadget.h>
#include <linux/usb/video.h>
#if IS_ENABLED(CONFIG_USB_UVCG_SG_TRANSFER)
#include <linux/scatterlist.h>
#endif
#include <media/v4l2-dev.h>

#include "uvc.h"
#include "uvc_queue.h"
#include "uvc_video.h"

#define UVCG_MAX_SG_NUM		64	// 8ms in 125us interval.

/*
 * How many empty payloads to keep in the isochronous chain while the encoder
 * has nothing to send. Zero leaves the chain to run out, which is what the
 * driver did before this knob existed.
 *
 * Measured on an SG2002. The depth is not a dial that trades one cost against
 * another - it has a cliff in it, and 8 sits on the wrong side. Swept against a
 * synthetic source that sends known frames, with the target host capturing the
 * raw stream and every received frame matched byte for byte against the frame
 * that was sent, 640x480 at 30fps, 34 payloads to the frame:
 *
 *	idle_depth=0	 63 of 444 frames damaged  (14.0%)
 *	idle_depth=8	118 of 516 frames damaged  (22.7%)
 *	idle_depth=32	  2 of 447 frames damaged   (0.4%)
 *
 * and at 43 payloads to the frame, 74 of 447 (16.4%) at zero against 3 of 446
 * (0.7%) at 32. So a shallow chain is worse than no chain at all: at 8 the
 * damage is not the one lost payload that zero gives but the whole frame,
 * ~23 KB holes opening at payload three, because the chain is refilled too
 * late to keep the endpoint's frame stamping in step and the burst that
 * follows is stamped into the past. At 32 the chain is deep enough that it is
 * never behind, and the loss all but disappears.
 *
 * Thirty two is also what the request ring holds, so at this depth the
 * endpoint is simply never idle.
 *
 * What it costs was measured, and it is not small. Under streaming with the
 * host pulling, the USB interrupt runs at 8252-9298 a second against 1000 when
 * the endpoint is idle, and the CPU goes from 81% idle to 19-32% idle. That
 * cost is the same at every nonzero depth, because an isochronous IN endpoint
 * sends one packet per microframe whatever the chain's lead is; depth 8 is not
 * cheaper than 32, only shallower. See idle_ioc for the half of it that can be
 * given back.
 *
 * The board also went off the network three times in the session that measured
 * this, for 25, 44 and 20 minutes. Those outages are NOT attributed: a second
 * agent was flashing and streaming to the same board throughout, and the kernel
 * under test carried two chain-restart bugs of its own. The default is left at
 * 32 because that is what has been running and what is being measured against;
 * whether it is safe is still open, and the way to close it is an hour-long
 * stream on an uncontended board.
 */
/*
 * Off by default, and the reason is worth stating because the numbers above
 * argue for 32.
 *
 * Every measurement above was taken at streaming_maxpacket=768, where a frame
 * is 34 to 43 payloads and the chain restarts between frames often enough for
 * the restart to be the dominant loss. Widening the endpoint to 3072 - the
 * architectural ceiling, 1024 bytes three times a microframe - removes that
 * mode outright: byte-exact against known sent frames on this board, with the
 * chain NOT fed, 3072 gives 1/451 damaged (0.2%) at 44 KB frames and 0/450 at
 * small ones, against 73/450 (16.2%) at 768. So the picture the deep chain was
 * buying is available without it.
 *
 * And the chain is not free. It completes 8000 requests a second at any
 * non-zero depth, which measured 8252-9298 usb interrupts a second with the
 * CPU down to 19-32% idle, and under that load this board wedged hard three
 * times - twice needing the power pulled, once sitting dead because
 * CONFIG_PANIC_TIMEOUT was 0. A torn frame is recoverable; a board that stops
 * answering is not.
 *
 * The knob stays, because the mechanism is real and a host that cannot take a
 * 3072 byte microframe may still want it. It just no longer defaults on.
 */
static unsigned int uvcg_idle_depth;
module_param_named(idle_depth, uvcg_idle_depth, uint, 0644);
MODULE_PARM_DESC(idle_depth,
		 "empty payloads kept queued on the isoc IN endpoint while idle");

/*
 * One completion interrupt per this many keep-alive payloads.
 *
 * Keeping the chain fed is not free, and the bill does not scale with
 * idle_depth: an isochronous IN endpoint sends one packet per microframe
 * whatever the depth, so a chain that is never allowed to run out completes
 * 8000 requests a second, and dwc2 stamps interrupt-on-complete into every
 * descriptor it fills. Measured on an SG2002 at 640x480/30 with the host
 * pulling the stream, 43 payloads to the frame:
 *
 *	not streaming, any depth	1000 irq/s, 81% idle
 *	streaming, idle_depth=32	~8800 irq/s, 19-32% idle
 *
 * and at that load the board went off the network entirely - the same
 * signature as the idle_depth=8 outage, and for the same reason, because 8 and
 * 32 cost exactly the same 8000 interrupts a second. The depth is the chain's
 * lead; this is its price.
 *
 * Nothing needs to hear about every keep-alive. They carry no payload, belong
 * to no frame, and the only thing their completion does is hand the request
 * back so the pump can queue it again - which the pump can just as well do
 * eight at a time. dwc2 sweeps every descriptor that has reached DMA-done on
 * each interrupt, not one, so batching costs no request and no ordering.
 *
 * The stride only has to stay well inside the chain's lead: at depth 32 and
 * stride 8 the refill happens every millisecond with 24 descriptors still
 * queued ahead of the core. One disables the batching. Zero would ask for no
 * interrupts at all, which dwc2 refuses - it forces one every
 * DWC2_ISOC_IOC_RUN descriptors regardless - so the smallest real interval is
 * still bounded by the hardware side.
 */
static unsigned int uvcg_idle_ioc = 8;
module_param_named(idle_ioc, uvcg_idle_ioc, uint, 0644);
MODULE_PARM_DESC(idle_ioc,
		 "one completion interrupt per this many keep-alive payloads");

/*
 * Whether the payload carrying EOF waits for the frame's earlier payloads to
 * retire, so that the header can still say the frame is damaged.
 *
 * The wait was introduced to make the error bit in the payload header accurate,
 * and that purpose is gone: this host decodes and displays a frame marked bad
 * exactly as it displays a good one.
 *
 * Nor is it needed for drop_bad, which is the one thing that does reach this
 * host. The ring is deep enough that a lost payload is reported while the
 * encoder is still filling the same frame: measured with the wait off, 15 of 16
 * losses still set frame_bad in time for the frame to be abandoned, and one
 * arrived too late. With the wait on, the frame's last payload is itself the one
 * most often lost - it goes out alone, as the only descriptor of a chain the
 * hold emptied and dwc2 had to rebuild - and a loss reported by that payload is
 * always too late by construction, because the frame ended when it was encoded:
 * 9 frames abandoned against 10 reported too late.
 *
 * So it is off: a cost with nothing left on the other side of it. How large the
 * cost is has not been isolated - a swept comparison is the only way to say, and
 * it needs the host to keep the camera open for longer than it has.
 */
static bool uvcg_eof_hold;
module_param_named(eof_hold, uvcg_eof_hold, bool, 0644);
MODULE_PARM_DESC(eof_hold,
		 "hold a frame's last payload until its earlier ones retire");

/*
 * How many of a frame's payloads may still be outstanding when its EOF payload
 * is released.
 *
 * Zero is the strict reading of the hold - wait for every earlier payload to
 * report - and zero is also, by definition, an empty descriptor chain. That is
 * the restart the comment above warns about: dwc2 returns target_frame to
 * TARGET_FRAME_INITIAL and the endpoint is started again for that one payload,
 * and a restart is where this controller loses payloads. The cost shows up in
 * the stats. Over one session with the hold on, ten of nineteen losses landed
 * on a frame that was already fully queued, which is the signature of the EOF
 * payload itself going missing; over one with the hold off, one of sixteen. The
 * EOF payload is one payload in fifteen and half the late losses.
 *
 * One leaves a descriptor live, so the EOF payload is appended to a chain that
 * is still running rather than starting a new one. The report given up is that
 * of the most recently queued payload, which is no more exposed than any other
 * payload in the middle of a frame; the report bought back is the EOF payload's
 * own arrival, which is the one the hold was spending a restart to obtain.
 *
 * Zero restores the previous behaviour exactly, for sweeping the trade against
 * the hardware.
 */
static unsigned int uvcg_eof_slack = 1;
module_param_named(eof_slack, uvcg_eof_slack, uint, 0644);
MODULE_PARM_DESC(eof_slack,
		 "payloads that may still be in flight when EOF is released");

/*
 * Whether a frame that has already lost a payload is abandoned instead of
 * finished.
 *
 * Isochronous cannot retransmit, so a frame with a hole in its entropy-coded
 * scan cannot be repaired - only labelled, and this host ignores the label. It
 * decodes the frame anyway, and a JPEG whose Huffman stream has a hole punched
 * through it is what the user sees as the picture shifting, taking on a colour
 * cast and going grey: one lost payload desynchronises the MCU position, the
 * chroma DC predictors and the chroma scan all at once.
 *
 * A frame that never arrives is invisible at 30fps; a frame that arrives torn
 * is not. So stop sending the rest of it and send no EOF, which leaves the host
 * an unterminated fragment and the previous good frame still on screen. The FID
 * toggles as it would at any frame boundary, so the stream stays in step and
 * the next frame starts cleanly.
 */
static bool uvcg_drop_bad = true;
module_param_named(drop_bad, uvcg_drop_bad, bool, 0644);
MODULE_PARM_DESC(drop_bad,
		 "abandon a frame that has already lost a payload");

/* --------------------------------------------------------------------------
 * Video codecs
 */

static int
uvc_video_encode_header(struct uvc_video *video, struct uvc_buffer *buf,
		u8 *data, int len)
{
	data[0] = 2;
	data[1] = UVC_STREAM_EOH | video->fid;

	/*
	 * A payload of this frame never made it onto the bus. Isochronous has
	 * no retransmit, so the frame cannot be repaired - but it can be
	 * labelled. UVC keeps an error bit in the payload header for exactly
	 * this, and a host that sees it discards the frame instead of decoding
	 * a JPEG with a hole punched through it. One frame missing at 30fps is
	 * not visible; one frame torn in half is.
	 */
	if (video->frame_bad)
		data[1] |= UVC_STREAM_ERR;

	if (buf->bytesused - video->queue.buf_used <= len - 2)
		data[1] |= UVC_STREAM_EOF;

	return 2;
}

static int
uvc_video_encode_data(struct uvc_video *video, struct uvc_buffer *buf,
		u8 *data, int len)
{
	struct uvc_video_queue *queue = &video->queue;
	unsigned int nbytes;
	void *mem;

	/* Copy video data to the USB buffer. */
	mem = buf->mem + queue->buf_used;
	nbytes = min((unsigned int)len, buf->bytesused - queue->buf_used);

	memcpy(data, mem, nbytes);
	queue->buf_used += nbytes;

	return nbytes;
}

/*
 * Ends the frame the encoder has just finished filling: hands the buffer back,
 * flips the frame ID, and starts a new serial so that requests still in flight
 * for the frame just ended are no longer mistaken for the new one's.
 *
 * Called with queue->irqlock held; req_lock nests inside it here and nowhere
 * takes them the other way round.
 */
static void uvc_video_frame_done(struct uvc_video *video, struct uvc_buffer *buf)
{
	video->queue.buf_used = 0;
	buf->state = UVC_BUF_STATE_DONE;
	uvcg_queue_next_buffer(&video->queue, buf);
	video->fid ^= UVC_STREAM_FID;

	spin_lock(&video->req_lock);
	if (video->frame_bad) {
		video->frames_marked++;
		video->frame_bad = 0;
	}
	video->frame_seq++;
	/* The payloads of the frame just ended keep their old serial, so their
	 * completions no longer touch this counter.
	 */
	video->frame_inflight = 0;
	spin_unlock(&video->req_lock);
}

/*
 * Gives up on the frame being encoded. Used where a payload was counted
 * against the frame but will never reach a completion handler, which would
 * otherwise leave the frame's last payload waiting forever for it to retire.
 */
static void uvc_video_frame_abandon(struct uvc_video *video)
{
	unsigned long flags;

	spin_lock_irqsave(&video->req_lock, flags);
	video->frame_seq++;
	video->frame_inflight = 0;
	video->frame_bad = 0;
	spin_unlock_irqrestore(&video->req_lock, flags);
}

static int
uvc_video_encode_bulk(struct usb_request *req, struct uvc_video *video,
		struct uvc_buffer *buf)
{
	struct uvcg_request *ctx = req->context;
	void *mem = req->buf;
	int len = video->req_size;
	int ret;

	ctx->frame = video->frame_seq;
	ctx->eof = false;

	/* Add a header at the beginning of the payload. */
	if (video->payload_size == 0) {
		ret = uvc_video_encode_header(video, buf, mem, len);
		video->payload_size += ret;
		mem += ret;
		len -= ret;
	}

	/* Process video data. */
	len = min((int)(video->max_payload_size - video->payload_size), len);
	ret = uvc_video_encode_data(video, buf, mem, len);

	video->payload_size += ret;
	len -= ret;

	req->length = video->req_size - len;
	req->zero = video->payload_size == video->max_payload_size;

	if (buf->bytesused == video->queue.buf_used) {
		uvc_video_frame_done(video, buf);
		video->payload_size = 0;
	}

	if (video->payload_size == video->max_payload_size ||
	    buf->bytesused == video->queue.buf_used)
		video->payload_size = 0;

	return 0;
}

#if !IS_ENABLED(CONFIG_USB_UVCG_SG_TRANSFER)
static int
uvc_video_encode_isoc(struct usb_request *req, struct uvc_video *video,
		struct uvc_buffer *buf)
{
	struct uvcg_request *ctx = req->context;
	void *mem = req->buf;
	int len = video->req_size;
	int ret;
	bool last, wait, bad, drop;

	/* Whether this payload is the one that will carry EOF. */
	last = buf->bytesused - video->queue.buf_used <= (unsigned int)(len - 2);

	/*
	 * "Has this frame lost a payload" and "have its earlier payloads
	 * retired" have to be read together, under one hold of the lock.
	 *
	 * Read apart they raced, and not in a way that could be waved off as
	 * unlikely: the completion that releases the wait is the same
	 * completion that reports the loss, so the single most likely instant
	 * for one to land between the two reads is the instant the hold exists
	 * to catch. Seen that way round it makes bad stale-false and the wait
	 * false at once, and the frame the hold was built to abandon is the
	 * one frame that gets through intact.
	 *
	 * A frame already known bad waits for nothing. There is no news left
	 * for the hold to collect, and either drop_bad is about to end the
	 * frame or the error bit is about to be spent on it.
	 */
	spin_lock(&video->req_lock);
	bad = video->frame_bad;
	wait = uvcg_eof_hold && last && !bad &&
	       video->frame_inflight > uvcg_eof_slack;
	spin_unlock(&video->req_lock);

	/*
	 * This frame has a hole in it and nothing can put the payload back.
	 * End it here rather than spend the rest of the bus on a picture that
	 * will decode into garbage: uvc_video_frame_done() hands the buffer
	 * back, toggles the frame ID and starts a new serial, so the host sees
	 * a fragment that never got its EOF followed by a clean new frame.
	 */
	if (uvcg_drop_bad && bad) {
		video->frames_dropped++;
		uvc_video_frame_done(video, buf);
		return -ENODATA;
	}

	/*
	 * The header of this payload is the last chance to tell the host the
	 * frame is damaged, and a damaged frame is only discovered when the
	 * request that lost the payload completes. So the last payload waits
	 * for the frame's earlier ones to retire.
	 *
	 * This is free. The ring is still full of this frame's payloads, so
	 * the encoder was only running ahead of a queue with nowhere to put
	 * the result; the EOF payload goes out in the same microframe either
	 * way. What it buys is that "bad" is known before the bit is spent.
	 */
	if (wait)
		return -EAGAIN;

	ctx->frame = video->frame_seq;
	ctx->eof = last;

	/* Add the header. */
	ret = uvc_video_encode_header(video, buf, mem, len);
	mem += ret;
	len -= ret;

	/* Process video data. */
	ret = uvc_video_encode_data(video, buf, mem, len);
	len -= ret;

	req->length = video->req_size - len;

	/*
	 * Ask once more, now that there is nothing left to encode. Filling a
	 * payload is most of a kilobyte of memcpy, which is long enough for a
	 * completion to land inside it and report a loss the header above was
	 * written without - and the frame would then be finished and sent,
	 * counted as marked but never dropped. Asking again turns that whole
	 * window into a drop. What remains is the handful of instructions
	 * between this read and the frame ending, which is the last point at
	 * which any of this frame is still unqueued.
	 *
	 * The bytes already copied into this request are simply not sent.
	 * uvc_video_frame_done() resets buf_used, so the next frame starts
	 * from the top of its own buffer.
	 */
	spin_lock(&video->req_lock);
	drop = uvcg_drop_bad && video->frame_bad;
	if (!drop)
		video->frame_inflight++;
	spin_unlock(&video->req_lock);

	if (drop) {
		video->frames_dropped++;
		uvc_video_frame_done(video, buf);
		return -ENODATA;
	}

	if (buf->bytesused == video->queue.buf_used)
		uvc_video_frame_done(video, buf);

	return 0;
}
#else
static int
uvc_video_encode_isoc(struct usb_request *req, struct uvc_video *video,
		struct uvc_buffer *buf)
{
	struct uvcg_request *ctx = req->context;
	int i;
	struct scatterlist *sg;

	ctx->frame = video->frame_seq;
	ctx->eof = false;
	req->length = 0;

	for_each_sg(req->sg, sg, UVCG_MAX_SG_NUM, i) {
		void *mem = sg_virt(sg);
		int len = video->req_size;
		int ret;

		/* Add the header. */
		ret = uvc_video_encode_header(video, buf, mem, len);
		mem += ret;
		len -= ret;
		/* Process video data. */
		ret = uvc_video_encode_data(video, buf, mem, len);
		len -= ret;

		sg->length = video->req_size - len;
		req->length += sg->length;

		if (buf->bytesused == video->queue.buf_used) {
			uvc_video_frame_done(video, buf);
			i++;
			break;
		}
	}
	req->num_sgs = i;

	return 0;
}
#endif
/* --------------------------------------------------------------------------
 * Request handling
 */

static int uvcg_video_ep_queue(struct uvc_video *video, struct usb_request *req)
{
	int ret;

	ret = usb_ep_queue(video->ep, req, GFP_ATOMIC);
	if (ret < 0) {
		uvcg_err(&video->uvc->func, "Failed to queue request (%d).\n",
			 ret);

		/* Isochronous endpoints can't be halted. */
		if (usb_endpoint_xfer_bulk(video->ep->desc))
			usb_ep_set_halt(video->ep);
	}

	return ret;
}

/* Interval between stats lines while a stream is running. */
#define UVCG_STATS_PERIOD	(20 * HZ)

static void uvc_video_report_stats(struct uvc_video *video)
{
	uvcg_info(&video->uvc->func,
		  "VS stream stats: %u requests, %u short (%u zero), %u errored, %u frames marked bad, %u dropped, %u marked too late (%u on EOF), %u idle, %u of %u bytes sent\n",
		  video->req_queued, video->req_short, video->req_zero,
		  video->req_err, video->frames_marked, video->frames_dropped,
		  video->frames_late, video->frames_late_eof, video->req_idle,
		  video->bytes_sent, video->bytes_queued);
}

static void
uvc_video_complete(struct usb_ep *ep, struct usb_request *req)
{
	struct uvcg_request *ctx = req->context;
	struct uvc_video *video = ctx->video;
	struct uvc_video_queue *queue = &video->queue;
	unsigned long flags;
	bool report;

	spin_lock_irqsave(&video->req_lock, flags);
	if (ctx->idle) {
		/* Carried nothing, so there is nothing to lose and no frame to
		 * blame. It only ever existed to keep the endpoint's descriptor
		 * chain from running out.
		 */
		if (video->idle_inflight)
			video->idle_inflight--;
	} else {
		video->req_queued++;
		video->bytes_queued += req->length;
		video->bytes_sent += req->actual;
		if (req->status)
			video->req_err++;
		/* Only this request's own frame is settled by it. A request
		 * left over from a frame the encoder has already finished
		 * carries the old serial and must not be counted against the
		 * frame now being filled.
		 */
		if (ctx->frame == video->frame_seq && video->frame_inflight)
			video->frame_inflight--;
		if (req->status == 0 && req->actual < req->length) {
			video->req_short++;
			if (req->actual == 0)
				video->req_zero++;
			if (ctx->frame == video->frame_seq) {
				video->frame_bad = 1;
			} else {
				video->frames_late++;
				if (ctx->eof)
					video->frames_late_eof++;
			}
		}
	}
	spin_unlock_irqrestore(&video->req_lock, flags);

	switch (req->status) {
	case 0:
		break;

	case -ESHUTDOWN:	/* disconnect from host. */
		uvcg_dbg(&video->uvc->func, "VS request cancelled.\n");
		uvcg_queue_cancel(queue, 1);
		break;

	default:
		uvcg_info(&video->uvc->func,
			  "VS request completed with status %d.\n",
			  req->status);
		uvcg_queue_cancel(queue, 0);
	}

	spin_lock_irqsave(&video->req_lock, flags);
	list_add_tail(&req->list, &video->req_free);
	report = time_after(jiffies, video->stats_next);
	if (report)
		video->stats_next = jiffies + UVCG_STATS_PERIOD;
	spin_unlock_irqrestore(&video->req_lock, flags);

	if (report)
		uvc_video_report_stats(video);

	queue_work(video->async_wq, &video->pump);
}

static int
uvc_video_free_requests(struct uvc_video *video)
{
	unsigned int i;

	for (i = 0; i < UVC_NUM_REQUESTS; ++i) {
		if (video->req[i]) {
			usb_ep_free_request(video->ep, video->req[i]);
			video->req[i] = NULL;
		}

		if (video->req_buffer[i]) {
			kfree(video->req_buffer[i]);
			video->req_buffer[i] = NULL;
		}
	}

	INIT_LIST_HEAD(&video->req_free);
	video->req_size = 0;
	return 0;
}

static int
uvc_video_alloc_requests(struct uvc_video *video)
{
	unsigned int req_size;
	unsigned int i;
#if IS_ENABLED(CONFIG_USB_UVCG_SG_TRANSFER)
	unsigned int j;
#endif
	int ret = -ENOMEM;

	BUG_ON(video->req_size);

	req_size = video->ep->maxpacket
		 * max_t(unsigned int, video->ep->maxburst, 1)
		 * (video->ep->mult);
#if !IS_ENABLED(CONFIG_USB_UVCG_SG_TRANSFER)
	for (i = 0; i < UVC_NUM_REQUESTS; ++i) {
		video->req_buffer[i] = kmalloc(req_size, GFP_KERNEL);
		if (video->req_buffer[i] == NULL)
			goto error;

		video->req[i] = usb_ep_alloc_request(video->ep, GFP_KERNEL);
		if (video->req[i] == NULL)
			goto error;

		video->req[i]->buf = video->req_buffer[i];
		video->req[i]->length = 0;
		video->req[i]->complete = uvc_video_complete;
		video->req_ctx[i].video = video;
		video->req_ctx[i].frame = video->frame_seq;
		video->req_ctx[i].idle = false;
		video->req_ctx[i].eof = false;
		video->req[i]->context = &video->req_ctx[i];

		list_add_tail(&video->req[i]->list, &video->req_free);
	}

	video->req_size = req_size;
#else
	req_size = ALIGN(req_size, 32);
	for (i = 0; i < UVC_NUM_REQUESTS; ++i) {
		video->req_buffer[i] = kmalloc(req_size * UVCG_MAX_SG_NUM, GFP_KERNEL);
		if (video->req_buffer[i] == NULL)
			goto error;

		video->req[i] = usb_ep_alloc_request(video->ep, GFP_KERNEL);
		if (video->req[i] == NULL)
			goto error;
		video->req[i]->sg = kmalloc(sizeof(struct scatterlist) * UVCG_MAX_SG_NUM, GFP_KERNEL);
		if (video->req[i]->sg == NULL)
			goto error;
		sg_init_table(video->req[i]->sg, UVCG_MAX_SG_NUM);
		video->req[i]->buf = video->req_buffer[i];
		for (j = 0; j < UVCG_MAX_SG_NUM; j++)
			sg_set_buf(&video->req[i]->sg[j], video->req[i]->buf + req_size * j, 0);
		video->req[i]->num_sgs = 0;
		video->req[i]->length = 0;
		video->req[i]->complete = uvc_video_complete;
		video->req_ctx[i].video = video;
		video->req_ctx[i].frame = video->frame_seq;
		video->req_ctx[i].idle = false;
		video->req_ctx[i].eof = false;
		video->req[i]->context = &video->req_ctx[i];
		list_add_tail(&video->req[i]->list, &video->req_free);
	}

	video->req_size = req_size;
#endif

	return 0;

error:
	uvc_video_free_requests(video);
	return ret;
}

/* --------------------------------------------------------------------------
 * Video streaming
 */

/*
 * Hand the endpoint a payload with nothing in it, purely so that its descriptor
 * chain does not run out. Returns false when the chain already has enough of
 * them queued, or when the endpoint is a bulk one, where an empty request is
 * not an idle interval but a short packet that would end a payload early.
 *
 * Called with no locks held. The pump is a single work item, so it cannot race
 * with itself over the endpoint's ordering.
 */
static bool uvc_video_queue_idle(struct uvc_video *video, struct usb_request *req)
{
	struct uvcg_request *ctx = req->context;
	unsigned long flags;

	if (video->max_payload_size || !uvcg_idle_depth)
		return false;

	spin_lock_irqsave(&video->req_lock, flags);
	if (video->idle_inflight >= uvcg_idle_depth) {
		spin_unlock_irqrestore(&video->req_lock, flags);
		return false;
	}
	video->idle_inflight++;
	video->req_idle++;
	spin_unlock_irqrestore(&video->req_lock, flags);

	ctx->idle = true;
	req->length = 0;
	req->zero = 0;
	/* Ask to be told about one keep-alive in uvcg_idle_ioc, not all of
	 * them. The counter is only ever touched from the pump, which is a
	 * single work item, so it needs no lock of its own.
	 */
	req->no_interrupt = uvcg_idle_ioc > 1 &&
			    (video->idle_seq++ % uvcg_idle_ioc) != 0;
#if IS_ENABLED(CONFIG_USB_UVCG_SG_TRANSFER)
	req->num_sgs = 0;
#endif

	if (!video->ep->enabled || uvcg_video_ep_queue(video, req) < 0) {
		spin_lock_irqsave(&video->req_lock, flags);
		if (video->idle_inflight)
			video->idle_inflight--;
		spin_unlock_irqrestore(&video->req_lock, flags);
		ctx->idle = false;
		return false;
	}

	return true;
}

/*
 * uvcg_video_pump - Pump video data into the USB requests
 *
 * This function fills the available USB requests (listed in req_free) with
 * video data from the queued buffers.
 */
static void uvcg_video_pump(struct work_struct *work)
{
	struct uvc_video *video = container_of(work, struct uvc_video, pump);
	struct uvc_video_queue *queue = &video->queue;
	struct uvcg_request *ctx;
	struct usb_request *req;
	struct uvc_buffer *buf;
	unsigned long flags;
	int ret;

	while (1) {
		/* Retrieve the first available USB request, protected by the
		 * request lock.
		 */
		spin_lock_irqsave(&video->req_lock, flags);
		if (list_empty(&video->req_free)) {
			spin_unlock_irqrestore(&video->req_lock, flags);
			return;
		}
		req = list_first_entry(&video->req_free, struct usb_request,
					list);
		list_del(&req->list);
		spin_unlock_irqrestore(&video->req_lock, flags);

		/* Retrieve the first available video buffer and fill the
		 * request, protected by the video queue irqlock.
		 */
		spin_lock_irqsave(&queue->irqlock, flags);
		buf = uvcg_queue_head(queue);
		if (buf == NULL) {
			spin_unlock_irqrestore(&queue->irqlock, flags);
			/* Nothing to send. On isochronous that is not a reason
			 * to stop feeding the endpoint: a chain left to run out
			 * takes the endpoint down with it, and the restart is
			 * where payloads go missing.
			 */
			if (uvc_video_queue_idle(video, req))
				continue;
			break;
		}

		ret = video->encode(req, video, buf);
		if (ret == -ENODATA) {
			/* The buffer was abandoned, not encoded. Put the
			 * request back and take the next frame.
			 */
			spin_unlock_irqrestore(&queue->irqlock, flags);
			spin_lock_irqsave(&video->req_lock, flags);
			list_add_tail(&req->list, &video->req_free);
			spin_unlock_irqrestore(&video->req_lock, flags);
			continue;
		}
		if (ret == -EAGAIN) {
			/* The frame's last payload is waiting for the frame's
			 * earlier ones to retire, so that a payload lost on the
			 * wire is known before the last chance to act on it is
			 * spent. The request goes back unqueued and every
			 * completion re-runs the pump, so the wait ends on the
			 * retirement it is waiting for - which is also a stretch
			 * of intervals with nothing offered to the endpoint.
			 */
			spin_unlock_irqrestore(&queue->irqlock, flags);
			if (uvc_video_queue_idle(video, req))
				continue;
			break;
		}

		ctx = req->context;
		ctx->idle = false;
		/* A payload's completion is the only report of its loss, and
		 * the request was last used as a keep-alive, so the batching
		 * flag has to be taken back off it explicitly.
		 */
		req->no_interrupt = 0;

		if (!video->ep->enabled) {
			spin_unlock_irqrestore(&queue->irqlock, flags);
			uvc_video_frame_abandon(video);
			uvcg_queue_cancel(queue, 0);
			break;
		}
		/* Queue the USB request */
		ret = uvcg_video_ep_queue(video, req);
		spin_unlock_irqrestore(&queue->irqlock, flags);

		if (ret < 0) {
			/* This payload was counted against the frame and will
			 * never complete, so nothing would ever retire it.
			 * Forget the frame rather than leave the next last
			 * payload waiting on a completion that cannot come.
			 */
			uvc_video_frame_abandon(video);
			uvcg_queue_cancel(queue, 0);
			break;
		}
	}

	spin_lock_irqsave(&video->req_lock, flags);
	list_add_tail(&req->list, &video->req_free);
	spin_unlock_irqrestore(&video->req_lock, flags);
	return;
}

/*
 * Enable or disable the video stream.
 */
int uvcg_video_enable(struct uvc_video *video, int enable)
{
	unsigned int i;
	int ret;

	if (video->ep == NULL) {
		uvcg_info(&video->uvc->func,
			  "Video enable failed, device is uninitialized.\n");
		return -ENODEV;
	}

	if (!enable) {
		uvc_video_report_stats(video);
		cancel_work_sync(&video->pump);
		uvcg_queue_cancel(&video->queue, 0);

		for (i = 0; i < UVC_NUM_REQUESTS; ++i)
			if (video->req[i])
				usb_ep_dequeue(video->ep, video->req[i]);

		uvc_video_free_requests(video);
		uvcg_queue_enable(&video->queue, 0);
		return 0;
	}

	video->req_idle = 0;
	video->idle_inflight = 0;
	video->idle_seq = 0;
	video->stats_next = jiffies + UVCG_STATS_PERIOD;
	video->req_queued = 0;
	video->req_short = 0;
	video->req_zero = 0;
	video->req_err = 0;
	video->frame_seq = 0;
	video->frame_inflight = 0;
	video->frame_bad = 0;
	video->frames_marked = 0;
	video->frames_dropped = 0;
	video->frames_late = 0;
	video->frames_late_eof = 0;
	video->bytes_queued = 0;
	video->bytes_sent = 0;

	if ((ret = uvcg_queue_enable(&video->queue, 1)) < 0)
		return ret;

	if ((ret = uvc_video_alloc_requests(video)) < 0)
		return ret;

	if (video->max_payload_size) {
		video->encode = uvc_video_encode_bulk;
		video->payload_size = 0;
	} else
		video->encode = uvc_video_encode_isoc;

	queue_work(video->async_wq, &video->pump);

	return ret;
}

/*
 * Initialize the UVC video stream.
 */
int uvcg_video_init(struct uvc_video *video, struct uvc_device *uvc)
{
	INIT_LIST_HEAD(&video->req_free);
	spin_lock_init(&video->req_lock);
	INIT_WORK(&video->pump, uvcg_video_pump);

	/* Refilling the endpoint is deadline work: a request that is not queued
	 * before its microframe is a payload the host never receives, and
	 * isochronous has no retransmit to recover it. The shared system
	 * workqueue puts that refill behind every other work item this single
	 * core has pending, so the stream gets a workqueue of its own that is
	 * unbound (any CPU may run it) and high priority.
	 */
	video->async_wq = alloc_workqueue("uvcgadget", WQ_UNBOUND | WQ_HIGHPRI, 0);
	if (!video->async_wq)
		return -ENOMEM;

	video->uvc = uvc;
	video->fcc = V4L2_PIX_FMT_YUYV;
	video->bpp = 16;
	video->width = 320;
	video->height = 240;
	video->imagesize = 320 * 240 * 2;

	/* Initialize the video buffers queue. */
	uvcg_queue_init(&video->queue, V4L2_BUF_TYPE_VIDEO_OUTPUT,
			&video->mutex);
	return 0;
}

/*
 * Tear the video stream's workqueue down. The gadget is rebound whenever the
 * presentation profile changes, so a workqueue left behind by an unbind is
 * leaked once per profile change rather than once per boot.
 */
void uvcg_video_exit(struct uvc_video *video)
{
	if (!video->async_wq)
		return;

	cancel_work_sync(&video->pump);
	destroy_workqueue(video->async_wq);
	video->async_wq = NULL;
}

