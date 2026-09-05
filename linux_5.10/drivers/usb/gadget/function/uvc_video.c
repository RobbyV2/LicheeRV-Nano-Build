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
#include <linux/ktime.h>
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
 * Whether a frame that has already lost a payload is abandoned instead of
 * finished.
 *
 * Isochronous cannot retransmit, so a frame with a hole in its entropy-coded
 * scan cannot be repaired, only labelled, and this host ignores the label. It
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
	 * no retransmit, so the frame cannot be repaired, only labelled. UVC
	 * keeps an error bit in the payload header for exactly this, and a
	 * host that honours it discards the frame instead of decoding a JPEG
	 * with a hole punched through it. One frame missing at 30fps is not
	 * visible; one frame torn in half is.
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
	bool bad, drop;

	spin_lock(&video->req_lock);
	bad = video->frame_bad;
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

	ctx->frame = video->frame_seq;
	/* Whether this payload is the one that will carry EOF. */
	ctx->eof = buf->bytesused - video->queue.buf_used <=
		   (unsigned int)(len - 2);

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
	 * written without, and the frame would then be finished and sent,
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
		/* Reached from the completion handler, in hard interrupt
		 * context, where a console line is written with interrupts
		 * off and costs the endpoint more than its lead. The refusal
		 * is counted by the caller; a debug line is all it gets here.
		 */
		uvcg_dbg(&video->uvc->func, "Failed to queue request (%d).\n",
			 ret);

		/* Isochronous endpoints can't be halted. */
		if (usb_endpoint_xfer_bulk(video->ep->desc))
			usb_ep_set_halt(video->ep);
	}

	return ret;
}

/*
 * Turns a request into a keep-alive: a payload with nothing in it, queued only
 * so that the endpoint has a packet for the next microframe. Called with
 * req_lock held.
 */
static void uvc_video_make_idle(struct uvc_video *video, struct usb_request *req)
{
	struct uvcg_request *ctx = req->context;

	ctx->idle = true;
	ctx->eof = false;
	req->length = 0;
	req->zero = 0;
#if IS_ENABLED(CONFIG_USB_UVCG_SG_TRANSFER)
	req->num_sgs = 0;
#endif
	video->req_idle++;
}

/*
 * Books a request as in flight and decides whether its completion raises an
 * interrupt. Called with req_lock held; the send itself happens after the lock
 * is released, so the UDC's own lock never nests inside req_lock.
 *
 * One interrupt per UVC_ISOC_IOC_STRIDE requests, plus one at every end of
 * frame so the buffer goes back to userspace within a microframe of its last
 * payload rather than up to a stride later. dwc2 retires every finished
 * descriptor behind the one that interrupted, so asking less often loses
 * nothing, and it forces an interrupt of its own every sixteen descriptors
 * should the stride ever fail to. On a bulk endpoint every request asks, as
 * it always has.
 */
static void uvc_video_arm(struct uvc_video *video, struct usb_request *req)
{
	struct uvcg_request *ctx = req->context;

	video->inflight++;
	req->no_interrupt = !video->max_payload_size && !ctx->eof &&
			    (video->ioc_seq++ % UVC_ISOC_IOC_STRIDE) != 0;
}

/*
 * Takes back a request the endpoint refused. A refused payload is a hole in
 * its frame, and the frame is marked so the encoder ends it rather than sends
 * the rest around the hole.
 *
 * Returns true when encoded payloads are still waiting on req_ready. A
 * refusal in the completion handler is the one event that can leave them
 * there with no completion to come: if the endpoint had drained to this last
 * request, nothing else will ever take the head of req_ready. The caller in
 * that position wakes the pump, which sends from the head. The pump itself
 * does not re-arm on its own refusal, so a bus that keeps refusing (suspended,
 * for instance) costs one attempt per wake and never a loop.
 */
static bool uvc_video_unsend(struct uvc_video *video, struct usb_request *req)
{
	struct uvcg_request *ctx = req->context;
	unsigned long flags;
	bool waiting;

	spin_lock_irqsave(&video->req_lock, flags);
	if (video->inflight)
		video->inflight--;
	video->req_refused++;
	if (!ctx->idle && ctx->frame == video->frame_seq) {
		if (video->frame_inflight)
			video->frame_inflight--;
		video->frame_bad = 1;
	}
	list_add_tail(&req->list, &video->req_free);
	waiting = video->is_enabled && !list_empty(&video->req_ready);
	spin_unlock_irqrestore(&video->req_lock, flags);

	return waiting;
}

/*
 * Sends encoded payloads from the head of req_ready while the endpoint holds
 * fewer than UVC_ISOC_INFLIGHT requests. The pump appends what it has just
 * encoded to the tail and calls this, so payloads leave in the order they
 * were encoded: one left waiting by a refused re-queue, or by a completion
 * that found the endpoint full, is never overtaken by a newer payload from
 * the middle of the next frame. Called with no lock held. Returns the
 * endpoint's refusal, if it refused, with the refused request already taken
 * back; the rest of req_ready is left where it is for the next wake.
 */
static int uvc_video_send_ready(struct uvc_video *video)
{
	struct usb_request *req;
	unsigned long flags;
	int ret;

	while (1) {
		spin_lock_irqsave(&video->req_lock, flags);
		if (!video->is_enabled || list_empty(&video->req_ready) ||
		    video->inflight >= UVC_ISOC_INFLIGHT) {
			spin_unlock_irqrestore(&video->req_lock, flags);
			return 0;
		}
		req = list_first_entry(&video->req_ready, struct usb_request,
				       list);
		list_del(&req->list);
		uvc_video_arm(video, req);
		spin_unlock_irqrestore(&video->req_lock, flags);

		/* Queued with no lock held, so the UDC's lock nests inside
		 * nothing of ours. A disabled endpoint is not offered the
		 * request: the UDC core would refuse it with a stack trace.
		 */
		if (!video->ep->enabled)
			ret = -ESHUTDOWN;
		else
			ret = uvcg_video_ep_queue(video, req);
		if (ret < 0) {
			uvc_video_unsend(video, req);
			return ret;
		}
	}
}

/*
 * Called from the completion handler under req_lock: one ring write and one
 * register read for the microframe. Nothing prints here.
 */
static void uvc_video_log_err(struct uvc_video *video,
			      struct usb_request *req,
			      struct uvcg_request *ctx)
{
	struct usb_function *f = &video->uvc->func;
	struct uvcg_err_ev *ev =
		&video->err_log[video->err_log_head % UVC_ERR_LOG_N];

	ev->ns = ktime_get_ns();
	ev->seq = video->req_queued;
	ev->frame = ctx->frame;
	ev->actual = req->actual;
	ev->length = req->length;
	ev->status = req->status;
	ev->idle = ctx->idle;
	ev->eof = ctx->eof;
	if (f->config && f->config->cdev && f->config->cdev->gadget)
		ev->uframe = usb_gadget_frame_number(f->config->cdev->gadget);
	else
		ev->uframe = -1;
	video->err_log_head++;
}

static void uvc_video_report_stats(struct uvc_video *video)
{
	u64 now = ktime_get_ns();
	unsigned int n = min_t(unsigned int, video->err_log_head,
			       (unsigned int)UVC_ERR_LOG_N);
	unsigned int i;

	uvcg_info(&video->uvc->func,
		  "VS stream stats: %u requests, %u short (%u zero), %u missed, %u errored, %u refused, %u frames marked bad, %u dropped, %u marked too late (%u on EOF), %u idle, %u of %u bytes sent\n",
		  video->req_queued, video->req_short, video->req_zero,
		  video->req_missed, video->req_err, video->req_refused,
		  video->frames_marked, video->frames_dropped,
		  video->frames_late, video->frames_late_eof, video->req_idle,
		  video->bytes_sent, video->bytes_queued);

	/* Every request has been dequeued and freed by now, so nothing
	 * writes the ring while it is read.
	 */
	if (video->err_teardown) {
		u64 first = now - video->err_teardown_first_ns;
		u64 last = now - video->err_teardown_last_ns;

		uvcg_info(&video->uvc->func,
			  "VS stream errors: %u logged (last %u shown), %u at teardown (%u with payload, %llu.%03llu s to %llu.%03llu s before stop)\n",
			  video->err_log_head, n, video->err_teardown,
			  video->err_teardown_payload,
			  first / NSEC_PER_SEC,
			  (first % NSEC_PER_SEC) / NSEC_PER_MSEC,
			  last / NSEC_PER_SEC,
			  (last % NSEC_PER_SEC) / NSEC_PER_MSEC);
	} else {
		uvcg_info(&video->uvc->func,
			  "VS stream errors: %u logged (last %u shown), 0 at teardown\n",
			  video->err_log_head, n);
	}
	for (i = video->err_log_head - n; i < video->err_log_head; i++) {
		struct uvcg_err_ev *ev = &video->err_log[i % UVC_ERR_LOG_N];
		u64 age = now - ev->ns;

		uvcg_info(&video->uvc->func,
			  "  err %u: status %d, %u of %u bytes%s%s, req %u, frame %u, uf %d, at %llu.%06llu s (%llu.%03llu s before stop)\n",
			  i, ev->status, ev->actual, ev->length,
			  ev->idle ? ", idle" : "", ev->eof ? ", eof" : "",
			  ev->seq, ev->frame, ev->uframe,
			  ev->ns / NSEC_PER_SEC,
			  (ev->ns % NSEC_PER_SEC) / NSEC_PER_USEC,
			  age / NSEC_PER_SEC,
			  (age % NSEC_PER_SEC) / NSEC_PER_MSEC);
	}
	video->err_log_head = 0;
	video->err_teardown = 0;
	video->err_teardown_payload = 0;
	video->err_teardown_first_ns = 0;
	video->err_teardown_last_ns = 0;
}

/*
 * Runs in hard interrupt context, called by the UDC with its own lock dropped.
 * The request that just completed is replaced at the endpoint before this
 * returns: by the next encoded payload if the pump has one ready, otherwise by
 * itself, emptied, as a keep-alive. The number of requests at the UDC therefore
 * never changes while the stream is up, the endpoint never runs out of packets
 * between frames or inside them, and nothing here waits on the scheduler. The
 * pump is woken only when a payload was consumed, so it runs once per interrupt
 * rather than once per packet.
 *
 * Nothing in this function prints. On this board a console line is written
 * with interrupts off at 87 us a character, which is longer than the endpoint
 * holds in descriptors.
 */
static void
uvc_video_complete(struct usb_ep *ep, struct usb_request *req)
{
	struct uvcg_request *ctx = req->context;
	struct uvc_video *video = ctx->video;
	struct uvc_video_queue *queue = &video->queue;
	struct usb_request *next = NULL;
	unsigned long flags;
	bool alive, wake = false;

	spin_lock_irqsave(&video->req_lock, flags);
	if (video->inflight)
		video->inflight--;
	if (!ctx->idle) {
		video->req_queued++;
		video->bytes_queued += req->length;
		video->bytes_sent += req->actual;
		if (req->status == -EXDEV)
			video->req_missed++;
		else if (req->status)
			video->req_err++;
		/* Only this request's own frame is settled by it. A request
		 * left over from a frame the encoder has already finished
		 * carries the old serial and must not be counted against the
		 * frame now being filled.
		 */
		if (ctx->frame == video->frame_seq && video->frame_inflight)
			video->frame_inflight--;
		/* A payload the UDC retired unsent comes back as -EXDEV with
		 * nothing moved; one cut short on the wire comes back with
		 * status 0. Both are holes in the frame.
		 */
		if ((req->status == 0 || req->status == -EXDEV) &&
		    req->actual < req->length) {
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
	if (req->status && req->status != -EXDEV) {
		if (req->status == -ESHUTDOWN || req->status == -ECONNRESET ||
		    !video->is_enabled) {
			u64 ns = ktime_get_ns();

			if (!video->err_teardown)
				video->err_teardown_first_ns = ns;
			video->err_teardown_last_ns = ns;
			video->err_teardown++;
			if (!ctx->idle)
				video->err_teardown_payload++;
		} else {
			uvc_video_log_err(video, req, ctx);
		}
	}

	alive = video->is_enabled && video->ep->enabled &&
		(req->status == 0 || req->status == -EXDEV);
	if (!alive) {
		/* The stream is stopping or the endpoint is gone. is_enabled
		 * goes false before the first request is dequeued at stop, so
		 * handing the request back is the only thing a completion may
		 * do with a request that is about to be freed.
		 */
		list_add_tail(&req->list, &video->req_free);
	} else if (!list_empty(&video->req_ready)) {
		next = list_first_entry(&video->req_ready, struct usb_request,
					list);
		list_del(&next->list);
		list_add_tail(&req->list, &video->req_free);
		wake = true;
	} else if (!video->max_payload_size) {
		/* Nothing encoded yet: send this one again, empty, so that
		 * the endpoint has a packet for the next microframe. On a
		 * bulk endpoint an empty request is a short packet that ends
		 * a payload early, so bulk waits for the pump instead.
		 */
		next = req;
		uvc_video_make_idle(video, req);
	} else {
		list_add_tail(&req->list, &video->req_free);
		wake = true;
	}
	if (next)
		uvc_video_arm(video, next);
	spin_unlock_irqrestore(&video->req_lock, flags);

	switch (req->status) {
	case 0:
	case -EXDEV:
		break;

	case -ESHUTDOWN:	/* disconnect from host. */
		uvcg_queue_cancel(queue, 1);
		break;

	default:
		uvcg_queue_cancel(queue, 0);
	}

	if (next && uvcg_video_ep_queue(video, next) < 0 &&
	    uvc_video_unsend(video, next))
		wake = true;

	if (wake)
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
	INIT_LIST_HEAD(&video->req_ready);
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
 * uvcg_video_pump - Pump video data into the USB requests
 *
 * Fills free requests with video data from the queued buffers. Every filled
 * request joins the tail of req_ready, and the head of req_ready goes to the
 * endpoint while fewer than UVC_ISOC_INFLIGHT are queued there, which is only
 * the case before the host's first token or after a refusal has let the
 * endpoint drain; otherwise the request waits for the next completion to send
 * it. Sending from the head keeps payloads in the order they were encoded.
 * The pump is never on the endpoint's critical path: it only encodes, and its
 * latency decides when a frame leaves, never whether the endpoint has a packet
 * to send.
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

	/* Payloads left waiting by a refused re-queue go first, before
	 * anything new is encoded behind them.
	 */
	if (uvc_video_send_ready(video) < 0) {
		uvcg_queue_cancel(queue, 0);
		return;
	}

	while (1) {
		/* Retrieve the first available USB request, protected by the
		 * request lock.
		 */
		spin_lock_irqsave(&video->req_lock, flags);
		if (!video->is_enabled || list_empty(&video->req_free)) {
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
			break;
		}

		ctx = req->context;
		ctx->idle = false;
		ret = video->encode(req, video, buf);
		spin_unlock_irqrestore(&queue->irqlock, flags);
		if (ret == -ENODATA) {
			/* The buffer was abandoned, not encoded. Put the
			 * request back and take the next frame.
			 */
			spin_lock_irqsave(&video->req_lock, flags);
			list_add_tail(&req->list, &video->req_free);
			spin_unlock_irqrestore(&video->req_lock, flags);
			continue;
		}

		spin_lock_irqsave(&video->req_lock, flags);
		if (!video->is_enabled) {
			list_add_tail(&req->list, &video->req_free);
			spin_unlock_irqrestore(&video->req_lock, flags);
			return;
		}
		list_add_tail(&req->list, &video->req_ready);
		spin_unlock_irqrestore(&video->req_lock, flags);

		if (uvc_video_send_ready(video) < 0) {
			uvcg_queue_cancel(queue, 0);
			return;
		}
	}

	spin_lock_irqsave(&video->req_lock, flags);
	list_add_tail(&req->list, &video->req_free);
	spin_unlock_irqrestore(&video->req_lock, flags);
}

/*
 * Fills the endpoint with keep-alives at stream start, so that the host's
 * first token finds UVC_ISOC_INFLIGHT packets waiting and the chain they form
 * is never empty from then on: every completion puts one back. Nothing goes
 * out before the host asks; the requests only sit at the UDC until the first
 * token starts the endpoint.
 */
static void uvc_video_prime(struct uvc_video *video)
{
	struct usb_request *req;
	unsigned long flags;
	unsigned int i;

	for (i = 0; i < UVC_ISOC_INFLIGHT; i++) {
		spin_lock_irqsave(&video->req_lock, flags);
		if (list_empty(&video->req_free)) {
			spin_unlock_irqrestore(&video->req_lock, flags);
			return;
		}
		req = list_first_entry(&video->req_free, struct usb_request,
					list);
		list_del(&req->list);
		uvc_video_make_idle(video, req);
		uvc_video_arm(video, req);
		spin_unlock_irqrestore(&video->req_lock, flags);

		if (uvcg_video_ep_queue(video, req) < 0) {
			uvc_video_unsend(video, req);
			return;
		}
	}
}

/*
 * Enable or disable the video stream.
 */
int uvcg_video_enable(struct uvc_video *video, int enable)
{
	unsigned long flags;
	unsigned int i;
	int ret;

	if (video->ep == NULL) {
		uvcg_info(&video->uvc->func,
			  "Video enable failed, device is uninitialized.\n");
		return -ENODEV;
	}

	if (!enable) {
		/* Off before anything is dequeued. From here on a completion
		 * only hands its request back and the pump exits at its first
		 * check, so nothing can give the UDC a request that is about
		 * to be freed. The endpoint itself may still be enabled here
		 * (STREAMOFF from a closing file handle rather than from the
		 * host's alt 0); the dequeue below drains it either way.
		 */
		spin_lock_irqsave(&video->req_lock, flags);
		video->is_enabled = false;
		spin_unlock_irqrestore(&video->req_lock, flags);

		cancel_work_sync(&video->pump);
		uvcg_queue_cancel(&video->queue, 0);

		for (i = 0; i < UVC_NUM_REQUESTS; ++i)
			if (video->req[i])
				usb_ep_dequeue(video->ep, video->req[i]);

		uvc_video_free_requests(video);
		uvcg_queue_enable(&video->queue, 0);

		/* Once per stream, from process context, where a console
		 * line costs the stream nothing because there is no stream.
		 */
		uvc_video_report_stats(video);
		return 0;
	}

	video->inflight = 0;
	video->ioc_seq = 0;
	video->req_idle = 0;
	video->req_queued = 0;
	video->req_short = 0;
	video->req_zero = 0;
	video->req_missed = 0;
	video->req_err = 0;
	video->err_log_head = 0;
	video->err_teardown = 0;
	video->err_teardown_payload = 0;
	video->err_teardown_first_ns = 0;
	video->err_teardown_last_ns = 0;
	video->req_refused = 0;
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

	spin_lock_irqsave(&video->req_lock, flags);
	video->is_enabled = true;
	spin_unlock_irqrestore(&video->req_lock, flags);

	/* STREAMON arrives after the host's alt 1 has enabled the endpoint,
	 * so the chain can be primed now. If the endpoint is not up yet the
	 * pump sends directly once it is, and the chain starts from there.
	 */
	if (!video->max_payload_size && video->ep->enabled)
		uvc_video_prime(video);

	queue_work(video->async_wq, &video->pump);

	return 0;
}

/*
 * Initialize the UVC video stream.
 */
int uvcg_video_init(struct uvc_video *video, struct uvc_device *uvc)
{
	INIT_LIST_HEAD(&video->req_free);
	INIT_LIST_HEAD(&video->req_ready);
	spin_lock_init(&video->req_lock);
	INIT_WORK(&video->pump, uvcg_video_pump);

	/* Encoding is still deadline work in the loose sense: a frame leaves
	 * the board when the pump has encoded it, and the shared system
	 * workqueue puts that behind every other work item this single core
	 * has pending. So the stream gets a workqueue of its own that is
	 * unbound (any CPU may run it) and high priority. The endpoint no
	 * longer depends on it for packets; only the frame's latency does.
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

