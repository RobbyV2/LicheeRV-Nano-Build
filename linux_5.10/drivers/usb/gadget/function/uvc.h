/* SPDX-License-Identifier: GPL-2.0+ */
/*
 *	uvc_gadget.h  --  USB Video Class Gadget driver
 *
 *	Copyright (C) 2009-2010
 *	    Laurent Pinchart (laurent.pinchart@ideasonboard.com)
 */

#ifndef _UVC_GADGET_H_
#define _UVC_GADGET_H_

#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/usb/composite.h>
#include <linux/videodev2.h>

#include <media/v4l2-device.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-fh.h>

#include "uvc_queue.h"

struct usb_ep;
struct usb_request;
struct uvc_descriptor_header;
struct uvc_device;

/* ------------------------------------------------------------------------
 * Debugging, printing and logging
 */

#define UVC_TRACE_PROBE				(1 << 0)
#define UVC_TRACE_DESCR				(1 << 1)
#define UVC_TRACE_CONTROL			(1 << 2)
#define UVC_TRACE_FORMAT			(1 << 3)
#define UVC_TRACE_CAPTURE			(1 << 4)
#define UVC_TRACE_CALLS				(1 << 5)
#define UVC_TRACE_IOCTL				(1 << 6)
#define UVC_TRACE_FRAME				(1 << 7)
#define UVC_TRACE_SUSPEND			(1 << 8)
#define UVC_TRACE_STATUS			(1 << 9)

#define UVC_WARN_MINMAX				0
#define UVC_WARN_PROBE_DEF			1

extern unsigned int uvc_gadget_trace_param;

#define uvc_trace(flag, msg...) \
	do { \
		if (uvc_gadget_trace_param & flag) \
			printk(KERN_DEBUG "uvcvideo: " msg); \
	} while (0)

/* A V4L2 handle on the streaming node outlives the function: uvc_unbind()
 * calls video_unregister_device(), which does not wait for open descriptors,
 * so uvc_v4l2_release() still runs afterwards and logs from there. By then
 * f->config is NULL, and &f->config->cdev->gadget->dev is pointer arithmetic
 * on NULL - a small non-NULL address that dev_printk() dereferences without
 * hesitation. That fires as an oops inside __fput() during do_exit(), which
 * the kernel cannot unwind ("Fixing recursive fault but reboot is needed"),
 * leaving the closing task unkillable in D state forever.
 *
 * Resolving the device defensively costs one branch per message and turns
 * every late log into a no-op instead of a fault.
 */
static inline struct device *uvcg_dev(struct usb_function *f)
{
	if (!f || !f->config || !f->config->cdev || !f->config->cdev->gadget)
		return NULL;
	return &f->config->cdev->gadget->dev;
}

#define uvcg_printk(level, f, fmt, args...)				\
	do {								\
		struct device *__uvcg_dev = uvcg_dev(f);		\
		if (__uvcg_dev)						\
			level(__uvcg_dev, "%s: " fmt, (f)->name, ##args); \
	} while (0)

#define uvcg_dbg(f, fmt, args...)  uvcg_printk(dev_dbg,  f, fmt, ##args)
#define uvcg_info(f, fmt, args...) uvcg_printk(dev_info, f, fmt, ##args)
#define uvcg_warn(f, fmt, args...) uvcg_printk(dev_warn, f, fmt, ##args)
#define uvcg_err(f, fmt, args...)  uvcg_printk(dev_err,  f, fmt, ##args)

/* ------------------------------------------------------------------------
 * Driver specific constants
 */

/* How many isochronous IN requests may be in flight at once. Four is the
 * upstream default and it is not enough here: a 3285 byte frame is five
 * payloads at a 768 byte microframe, so the endpoint runs dry inside every
 * single frame and the refill has to win a 125us race to keep it fed. The
 * same lever on the OUT side - f_uac2's req_number, raised from 4 to 8 - cut
 * duplicated audio blocks from 60.5% to 26.3%, so give the video IN endpoint
 * the same headroom and more.
 *
 * Sixteen was still not enough. Measured over one streaming session:
 *
 *	20856 requests, 123 short (123 zero), 0 errored,
 *	123 frames marked bad, 14756401 of 14839104 bytes sent
 *
 * 123 isochronous requests completed with actual == 0, dropping 82703 queued
 * bytes (0.56%) and corrupting a JPEG every time. At 768 bytes a microframe a
 * sixteen deep ring covers only 2 ms, so any scheduling gap longer than that
 * on this single core empties it. Thirty two doubles the window to 4 ms, the
 * same lever and the same count that ended the audio repeats in c0060b9.
 *
 * This costs buffers only: the endpoint count and the tx FIFO seating are set
 * by maxpacket (768), not by ring depth, and dwc2 gives an isochronous
 * endpoint MAX_DMA_DESC_NUM_HS_ISOC (256) descriptors, so 64 fits with room.
 *
 * Sixty four, with 48 of them in flight, is the fallback for a console that
 * is not quiet. With 24 in flight the chain held 2 ms at its worst moment,
 * and one kernel-log line written from process context during a 120 s stream
 * was enough to run it dry once: the console on this board writes with
 * interrupts off at 87 us a character, so a line of 30 characters covers the
 * lead. Doubling the pool puts 5 ms between the core and the end of the chain
 * at the same interrupt rate. The buffers are plain kmalloc from the general
 * slab, allocated per stream at STREAMON and freed at STREAMOFF, 768 bytes
 * each at this profile, so the doubling costs about 48 KB more while a stream
 * is up and nothing from the DMA pools. Revert by putting 32 and 24 back.
 */
#define UVC_NUM_REQUESTS			64

/* How many of those are kept queued at the UDC at all times while an
 * isochronous stream is up. Every completion re-queues one, so the count is a
 * constant and the endpoint's descriptor chain never runs out; the other
 * sixteen are the pump's working set. Lead at the worst moment, just before
 * a batch of completions is serviced, is 48 minus the stride below: 40
 * descriptors, 5 ms at one microframe each.
 */
#define UVC_ISOC_INFLIGHT			48

/* One completion interrupt per this many requests, plus one at every end of
 * frame. The bus carries 8000 packets a second either way; this only decides
 * which of them are heard about. dwc2 retires every finished descriptor on
 * each interrupt, so nothing is lost by asking less often.
 */
#define UVC_ISOC_IOC_STRIDE			8

#define UVC_MAX_REQUEST_SIZE			64
#define UVC_MAX_EVENTS				4

/* ------------------------------------------------------------------------
 * Structures
 */

struct uvc_video;

/*
 * Per request state, reached from the completion handler through
 * usb_request::context.
 *
 * A lost payload is only ever reported when the request that carried it
 * completes, and by then the encoder is up to UVC_NUM_REQUESTS payloads
 * further on - most of a frame at 768 bytes a payload. Recording which frame
 * a request was filled from is what lets the completion say "this frame lost
 * a payload" rather than "some frame did", which is the whole difference
 * between labelling the damaged frame and labelling the good one after it.
 */
struct uvcg_request {
	struct uvc_video *video;
	unsigned int frame;
	/* This request carries no payload at all; it is only there to keep the
	 * endpoint's descriptor chain from running out. It belongs to no frame,
	 * so its completion must not settle one.
	 */
	bool idle;
	/* This request carries the frame's EOF. Its completion always asks for
	 * an interrupt, so the buffer goes back to userspace promptly, and its
	 * loss is the one loss that can never be reported in time: the frame
	 * ends as it is encoded, so the serial has already moved on by the time
	 * it completes. Counted apart from every other late report.
	 */
	bool eof;
};

struct uvc_video {
	struct uvc_device *uvc;
	struct usb_ep *ep;

	struct work_struct pump;
	/* The pump encodes frames into requests. The endpoint is fed from the
	 * completion handler and never waits on the pump, but a frame leaves
	 * the board only when the pump has encoded it, and on the shared
	 * system workqueue it would queue behind whatever else this single core
	 * is doing, so it gets a dedicated high-priority one of its own.
	 */
	struct workqueue_struct *async_wq;

	/* Count what was queued against what actually went out, so a run that
	 * is fixed can be told apart from a run that was lucky.
	 */
	/* Serial of the frame the encoder is filling requests from. Every
	 * request carries the serial it was filled at, so a completion can be
	 * attributed to the frame that actually lost the payload.
	 */
	unsigned int frame_seq;
	/* Payloads of frame_seq that are queued and have not completed yet. */
	unsigned int frame_inflight;
	/* Set when a payload of frame_seq did not reach the host, cleared when
	 * the frame ends.
	 */
	unsigned int frame_bad;
	unsigned int frames_marked;
	/* Frames ended early because a payload of theirs was lost: the rest of
	 * the frame was never sent and no EOF went with it, so the host has a
	 * fragment to discard rather than a whole-looking JPEG with a hole.
	 */
	unsigned int frames_dropped;
	/* Frames whose loss was reported after every payload of theirs had
	 * already been encoded, so none was left to carry the error bit. Here
	 * so that "the host was told" can be told apart from "the host was
	 * not".
	 */
	unsigned int frames_late;
	/* Of those, the ones where the payload lost was the EOF payload itself.
	 * Nothing in the driver can ever catch that one in time, so it is the
	 * floor on frames_late. Counting it apart is what says whether a late
	 * report is a hole in the accounting or a hole in the wire.
	 */
	unsigned int frames_late_eof;
	/* Zero-length payloads queued to keep the endpoint alive. Counted apart
	 * from the real ones so that the loss rate stays a rate of payloads
	 * that carried video.
	 */
	unsigned int req_idle;
	/* Payloads the UDC retired without sending, reported as -EXDEV. Each
	 * one is a hole in a frame, and the UDC's own isoc_lost counter should
	 * say the same number.
	 */
	unsigned int req_missed;
	/* Requests the UDC refused to queue at all. */
	unsigned int req_refused;
	unsigned int req_queued;
	unsigned int req_short;
	unsigned int req_zero;
	unsigned int req_err;
	unsigned int bytes_queued;
	unsigned int bytes_sent;

	/* Frame parameters */
	u8 bpp;
	u32 fcc;
	unsigned int width;
	unsigned int height;
	unsigned int imagesize;
	struct mutex mutex;	/* protects frame parameters */

	/* Requests */
	unsigned int req_size;
	struct usb_request *req[UVC_NUM_REQUESTS];
	__u8 *req_buffer[UVC_NUM_REQUESTS];
	struct uvcg_request req_ctx[UVC_NUM_REQUESTS];
	struct list_head req_free;
	/* Encoded and waiting for a completion to send it. The pump fills
	 * requests onto this list once the endpoint holds UVC_ISOC_INFLIGHT;
	 * the completion handler takes them from here.
	 */
	struct list_head req_ready;
	/* Requests queued at the UDC right now. Under req_lock. */
	unsigned int inflight;
	/* Which request this is, so that every UVC_ISOC_IOC_STRIDE-th one can
	 * be the one that asks for a completion interrupt. Under req_lock.
	 */
	unsigned int ioc_seq;
	/* False before anything is dequeued at stop, so a completion arriving
	 * during teardown hands its request back rather than re-queueing one
	 * that is about to be freed. Under req_lock.
	 */
	bool is_enabled;
	spinlock_t req_lock;

	/* Answers -ENODATA when the frame was dropped rather than encoded. */
	int (*encode) (struct usb_request *req, struct uvc_video *video,
			struct uvc_buffer *buf);

	/* Context data used by the completion handler */
	__u32 payload_size;
	__u32 max_payload_size;

	struct uvc_video_queue queue;
	unsigned int fid;
};

enum uvc_state {
	UVC_STATE_DISCONNECTED,
	UVC_STATE_CONNECTED,
	UVC_STATE_STREAMING,
};

struct uvc_device {
	struct video_device vdev;
	struct v4l2_device v4l2_dev;
	enum uvc_state state;
	struct usb_function func;
	struct uvc_video video;

	/* Descriptors */
	struct {
		const struct uvc_descriptor_header * const *fs_control;
		const struct uvc_descriptor_header * const *ss_control;
		const struct uvc_descriptor_header * const *fs_streaming;
		const struct uvc_descriptor_header * const *hs_streaming;
		const struct uvc_descriptor_header * const *ss_streaming;
	} desc;

	unsigned int control_intf;
	struct usb_ep *control_ep;
	struct usb_request *control_req;
	void *control_buf;

	unsigned int streaming_intf;

	/* Whether a V4L2 handle is open on the streaming node. struct
	 * video_device is embedded in this struct and uvc_free() kfree()s the
	 * lot, so a handle that outlives the function leaves the v4l2 core
	 * dereferencing freed memory in v4l2_release(). uvc_function_unbind()
	 * waits on this, bounded, so the common case closes first.
	 */
	bool func_connected;
	wait_queue_head_t func_connected_queue;

	/* Events */
	unsigned int event_length;
	unsigned int event_setup_out : 1;
};

static inline struct uvc_device *to_uvc(struct usb_function *f)
{
	return container_of(f, struct uvc_device, func);
}

struct uvc_file_handle {
	struct v4l2_fh vfh;
	struct uvc_video *device;
};

#define to_uvc_file_handle(handle) \
	container_of(handle, struct uvc_file_handle, vfh)

/* ------------------------------------------------------------------------
 * Functions
 */

extern void uvc_function_setup_continue(struct uvc_device *uvc);
extern void uvc_endpoint_stream(struct uvc_device *dev);

extern void uvc_function_connect(struct uvc_device *uvc);
extern void uvc_function_disconnect(struct uvc_device *uvc);

#endif /* _UVC_GADGET_H_ */
