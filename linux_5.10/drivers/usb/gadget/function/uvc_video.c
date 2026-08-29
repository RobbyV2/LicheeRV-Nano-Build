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
 * Measured on an SG2002: at 8, so that the chain never runs out, the endpoint
 * stops being restarted almost entirely - BNAs fall from 68.7/s to 0.7/s - but
 * the descriptors carrying video fare far worse, not better. The core services
 * this endpoint on roughly one microframe in four, and a descriptor whose
 * target microframe goes unserviced is destroyed rather than deferred, so a
 * chain stamped for consecutive microframes throws away three payloads in four:
 * 26430 of 28840 payloads lost against 139 of 46008 with the chain left alone.
 * Writable at runtime so the trade can be swept against the hardware rather
 * than argued about.
 */
static unsigned int uvcg_idle_depth;
module_param_named(idle_depth, uvcg_idle_depth, uint, 0644);
MODULE_PARM_DESC(idle_depth,
		 "empty payloads kept queued on the isoc IN endpoint while idle");

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
	bool last, wait;

	/* Whether this payload is the one that will carry EOF. */
	last = buf->bytesused - video->queue.buf_used <= (unsigned int)(len - 2);

	spin_lock(&video->req_lock);
	wait = last && video->frame_inflight;
	spin_unlock(&video->req_lock);

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

	/* Add the header. */
	ret = uvc_video_encode_header(video, buf, mem, len);
	mem += ret;
	len -= ret;

	/* Process video data. */
	ret = uvc_video_encode_data(video, buf, mem, len);
	len -= ret;

	req->length = video->req_size - len;

	spin_lock(&video->req_lock);
	video->frame_inflight++;
	spin_unlock(&video->req_lock);

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
		  "VS stream stats: %u requests, %u short (%u zero), %u errored, %u frames marked bad, %u marked too late, %u idle, %u of %u bytes sent\n",
		  video->req_queued, video->req_short, video->req_zero,
		  video->req_err, video->frames_marked, video->frames_late,
		  video->req_idle, video->bytes_sent, video->bytes_queued);
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
			if (ctx->frame == video->frame_seq)
				video->frame_bad = 1;
			else
				video->frames_late++;
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

		/* The frame's last payload waits for the frame's earlier ones
		 * to retire, so that a payload lost on the wire is known
		 * before the header that could have said so is encoded. The
		 * request goes back unqueued and every completion re-runs the
		 * pump, so the wait ends on the retirement it is waiting for.
		 */
		if (video->encode(req, video, buf) == -EAGAIN) {
			spin_unlock_irqrestore(&queue->irqlock, flags);
			/* Waiting on a retirement, which is still a stretch of
			 * intervals with nothing offered to the endpoint.
			 */
			if (uvc_video_queue_idle(video, req))
				continue;
			break;
		}

		ctx = req->context;
		ctx->idle = false;

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
	video->stats_next = jiffies + UVCG_STATS_PERIOD;
	video->req_queued = 0;
	video->req_short = 0;
	video->req_zero = 0;
	video->req_err = 0;
	video->frame_seq = 0;
	video->frame_inflight = 0;
	video->frame_bad = 0;
	video->frames_marked = 0;
	video->frames_late = 0;
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

