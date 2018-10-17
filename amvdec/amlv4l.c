#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "amlv4l.h"
#include "amvdec_priv.h"
#define V4LDEVICE_ionvideo_NAME  "/dev/video"
#define V4LDEVICE_ionvideo_BASE  13
#define V4LDEVICE_amlvideo_NAME  "/dev/video10"
#define CLEAR(s) memset(&s, 0, sizeof(s))
#define V4L2_CID_USER_AMLOGIC_IONVIDEO_BASE   (V4L2_CID_USER_BASE + 0x1100)

static int amlv4l_unmapbufs(amvideo_dev_t *dev);
static int amlv4l_mapbufs(amvideo_dev_t *dev);
int amlv4l_setfmt(amvideo_dev_t *dev, struct v4l2_format *fmt);
int amlv4l_init(amvideo_dev_t *dev, int type, int width, int height, int fmt, int buffernum) {
    int ret;
    amlv4l_dev_t *v4l = dev->devpriv;
    struct v4l2_format v4lfmt;

    int id = 0 ;
    if (dev->use_frame_mode)
        id = dev->video_id;
    if (dev->display_mode == 0) {
        snprintf(dev->devname, AMVIDEO_DEV_NAME_SIZE, V4LDEVICE_ionvideo_NAME "%d", V4LDEVICE_ionvideo_BASE+id);
        v4l->memory_mode = V4L2_MEMORY_DMABUF;
    }
    else if (dev->display_mode == 1) {
        snprintf(dev->devname, AMVIDEO_DEV_NAME_SIZE, V4LDEVICE_amlvideo_NAME);
        v4l->memory_mode = V4L2_MEMORY_MMAP;
    }

    ret = open(dev->devname, O_RDWR | O_NONBLOCK);
    if (ret < 0) {
        LOGE("v4l device %s open failed!,id=%d,ret=%d,%s(%d)\n",dev->devname, id, ret, strerror(errno), errno);
        return errno;
    }
    v4l->v4l_fd = ret;
    v4l->type=V4L2_BUF_TYPE_VIDEO_CAPTURE;
    v4l->width=width;
    v4l->height=height;
    v4l->pixformat=fmt;
    v4l->buffer_num = buffernum;
    v4lfmt.type = v4l->type;
    if (dev->display_mode == 0) {
        struct v4l2_control ctl;
        memset( &ctl, 0, sizeof(ctl));
        ctl.value = 1; //set ionvideo freerun mode
        ctl.id = V4L2_CID_USER_AMLOGIC_IONVIDEO_BASE;
        ret = ioctl(v4l->v4l_fd, VIDIOC_S_CTRL, &ctl);
        if (ret < 0) {
          //LOGE("Set freerun_mode: %s. ret=%d", strerror(errno),ret);
        }
    }

    v4lfmt.fmt.pix.width       = v4l->width;
    v4lfmt.fmt.pix.height      =  v4l->height;
    v4lfmt.fmt.pix.pixelformat = v4l->pixformat;
    ret=amlv4l_setfmt(dev,&v4lfmt);
    if (ret != 0) {
        goto error_out;
    }
#if 0 //ANDROID_PLATFORM_SDK_VERSION <= 25
    ret=amlv4l_mapbufs(dev);
#endif
    error_out:
    return ret;
}

static int amlv4l_ioctl(amvideo_dev_t *dev, int request, void *arg) {
    int ret;
    amlv4l_dev_t *v4l = dev->devpriv;
    ret = ioctl(v4l->v4l_fd, request, arg);
    if (ret == -1 && errno) {
         //LOGE("amlv4l_ioctlfailed!,request=%x,ret=%d,%s(%d)\n",request, ret, strerror(errno), errno);
         ret = -errno;
    }
    return ret;
}

int amlv4l_dequeue_buf(amvideo_dev_t *dev, vframebuf_t *vf) {
    struct v4l2_buffer vbuf;
    CLEAR(vbuf);
    int ret;
    amlv4l_dev_t *v4l = dev->devpriv;
    vbuf.type = v4l->type;
    vbuf.memory = v4l->memory_mode;
    vbuf.length = vf->length;
    ret = amlv4l_ioctl(dev, VIDIOC_DQBUF, &vbuf);
    if (!ret && vbuf.index < v4l->buffer_num) {
        vf->pts = vbuf.timestamp.tv_sec & 0xFFFFFFFF;
        vf->pts <<= 32;
        vf->pts += vbuf.timestamp.tv_usec & 0xFFFFFFFF;
        if (dev->display_mode == 0) {
            vf->fd = vbuf.m.fd;
            if (vbuf.timecode.frames & 0x1)
                vf->error_recovery = 1;
            else
                vf->error_recovery = 0;
        } else
            vf->error_recovery = 0;
        vf->index = vbuf.index;
        vf->width = vbuf.timecode.type;    //for AdaptivePlayback
        vf->height = vbuf.timecode.flags;
        if (vbuf.timecode.frames & 0x2)
            vf->sync_frame = 1;
        else
            vf->sync_frame = 0;
        //LOGE("vbuf.timecode.frames=%d, vf->sync_frame=%d", vbuf.timecode.frames, vf->sync_frame);
        vf->frame_num = vbuf.sequence;
    }

    return ret;
}

int amlv4l_queue_buf(amvideo_dev_t *dev, vframebuf_t *vf) {
    struct v4l2_buffer vbuf;
    CLEAR(vbuf);
    int ret;
    amlv4l_dev_t *v4l = dev->devpriv;
    vbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    vbuf.memory = v4l->memory_mode;
    vbuf.index = vf->index;

    vbuf.m.fd = vf->fd;
    vbuf.length = vf->length;
    return amlv4l_ioctl(dev, VIDIOC_QBUF, &vbuf);
}

int amlv4l_start(amvideo_dev_t *dev) {
    int type;
    amlv4l_dev_t *v4l = dev->devpriv;
    type=v4l->type;
    return amlv4l_ioctl(dev, VIDIOC_STREAMON,&type);
}

int amlv4l_stop(amvideo_dev_t *dev) {
    int type;
    amlv4l_dev_t *v4l = dev->devpriv;
    type=v4l->type;
    return amlv4l_ioctl(dev, VIDIOC_STREAMOFF,&type);
}


int amlv4l_setfmt(amvideo_dev_t *dev, struct v4l2_format *fmt) {
   int ret = amlv4l_ioctl(dev, VIDIOC_S_FMT, fmt);
      if (ret != 0) {
        LOGE("VIDIOC_G_FMT failed,ret=%d\n", ret);
        return ret;
      }
      return ret;
}


static int amlv4l_unmapbufs(amvideo_dev_t *dev) {
  /*  int i, ret;
    amlv4l_dev_t *v4l = dev->devpriv;
    vframebuf_t *vf = v4l->vframe;
    ret = 0;
    if (!vf)
      return 0;
    for (i = 0; i < v4l->bufferNum; i++) {
        if (vf[i].vbuf == NULL || vf[i].vbuf == MAP_FAILED) {
            continue;
        }
        ret = munmap(vf[i].vbuf, vf[i].length);
        vf[i].vbuf = NULL;
    }
    free(v4l->vframe);
    v4l->vframe=NULL;
    return ret;*/
    return 0;
}

static int amlv4l_mapbufs(amvideo_dev_t *dev) {
    int ret;
    struct v4l2_requestbuffers rb;
    CLEAR(rb);
    amlv4l_dev_t *v4l = dev->devpriv;
    rb.count = v4l->buffer_num; // 4
    rb.type = v4l->type;//V4L2_BUF_TYPE_VIDEO_CAPTURE;
    rb.memory = v4l->memory_mode;//V4L2_MEMORY_DMABUF
    ret = amlv4l_ioctl(dev, VIDIOC_REQBUFS, &rb);
    if (ret != 0) {
         LOGE("VIDIOC_REQBUFS failed,ret=%d\n", ret);
    }
    return ret;

 /* struct v4l2_buffer vbuf;
    int ret;
    int i;
    amlv4l_dev_t *v4l = dev->devpriv;
    vframebuf_t *vf;
    struct v4l2_requestbuffers req;

    req.count = 4;
    req.type = v4l->type;;
    req.memory = v4l->memory_mode;  ;
    ret=amlv4l_ioctl(dev, VIDIOC_REQBUFS, &req);
    if (ret != 0) {
         LOGE("VIDIOC_REQBUFS failed,ret=%d\n", ret);
         return ret;
    }
    v4l->bufferNum=req.count;
    vf=(vframebuf_t *)malloc(sizeof(vframebuf_t)*req.count);
    if (!vf)
        goto errors_out;
    v4l->vframe=vf;
    vbuf.type = v4l->type;
    vbuf.memory = v4l->memory_mode;
    for (i = 0; i < v4l->bufferNum; i++) {
        vbuf.index = i;
        ret = amlv4l_ioctl(dev, VIDIOC_QUERYBUF, &vbuf);
        if (ret < 0) {
            LOGE("VIDIOC_QUERYBUF,ret=%d\n", ret);
            goto errors_out;
        }

        vf[i].index = i;
        vf[i].offset = vbuf.m.offset;
        vf[i].pts = 0;
        vf[i].duration = 0;
        vf[i].vbuf = mmap(NULL, vbuf.length, PROT_READ | PROT_WRITE, MAP_SHARED, v4l->v4l_fd, vbuf.m.offset);
        if (vf[i].vbuf == NULL || vf[i].vbuf == MAP_FAILED) {
            LOGE("mmap failed,index=%d,ret=%p, errstr=%s\n", i,vf[i].vbuf, strerror(errno));
            ret = -errno;
            goto errors_out;
        }
        vf[i].length=vbuf.length;
        LOGI("mmaped buf %d,off=%d,vbuf=%p,len=%d\n", vf[i].index,vf[i].offset,vf[i].vbuf,vf[i].length);
    }
    LOGI("mmap ok,bufnum=%d\n", i);
    return 0;
errors_out:
    amlv4l_unmapbufs(dev);
    return ret;*/
}

int amlv4l_release(amvideo_dev_t *dev) {
    int ret = -1;
    amlv4l_dev_t *v4l = dev->devpriv;
    if (v4l->v4l_fd < 0) {
        return 0;
    }
    amlv4l_stop(dev);
#if 0 //ANDROID_PLATFORM_SDK_VERSION <= 25
    amlv4l_unmapbufs(dev);
#endif
    if (v4l->v4l_fd >= 0) {
        ret = close(v4l->v4l_fd);
    }
    v4l->v4l_fd = -1;
    free(dev);
    if (ret == -1 && errno) {
        ret = -errno;
    }

    return ret;
}

amvideo_dev_t *new_amlv4l(void) {
    //...
    amvideo_dev_t *dev;
    amlv4l_dev_t *v4l;
    dev = malloc(sizeof(amvideo_dev_t) + sizeof(amlv4l_dev_t));
    memset(dev, 0, sizeof(amvideo_dev_t) + sizeof(amlv4l_dev_t));
    dev->devpriv=(void *)((void *)(&dev->devpriv)+sizeof(void *));
    v4l = dev->devpriv;
    //v4l->memory_mode=V4L2_MEMORY_MMAP;
    dev->ops.init = amlv4l_init;
    dev->ops.release = amlv4l_release;
    dev->ops.dequeuebuf = amlv4l_dequeue_buf;
    dev->ops.queuebuf = amlv4l_queue_buf;
    dev->ops.start = amlv4l_start;
    dev->ops.stop = amlv4l_stop;
    /**/
    return dev;
}

