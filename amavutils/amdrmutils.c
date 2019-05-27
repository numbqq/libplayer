/*
 * Copyright (C) 2010 Amlogic Corporation.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */




#define LOG_TAG "amdrmutils"

#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <strings.h>
#ifdef ANDROID
#include <cutils/log.h>
#include <cutils/properties.h>
#endif
#include <sys/ioctl.h>
#include "include/Amsyswrite.h"
#include "include/amdrmutils.h"

#define TVP_ENABLE_PATH     "/sys/class/codec_mm/tvp_enable"
#define TVP_REGION_PATH     "/sys/class/codec_mm/tvp_region"
#define FREE_KEEP_BUFFER_PATH   "/sys/class/video/free_keep_buffer"
#define FREE_CMA_BUFFER_PATH   "/sys/class/video/free_cma_buffer"
#define VFM_DEF_MAP_PATH    "/sys/class/vfm/map"
#define DI_TVP_REGION_PATH  "/sys/class/deinterlace/di0/tvp_region"
#define DISABLE_VIDEO_PATH  "/sys/class/video/disable_video"
#ifdef ANDROID
#ifndef LOGD
#define LOGV ALOGV
#define LOGD ALOGD
#define LOGI ALOGI
#define LOGW ALOGW
#define LOGE ALOGE
#endif
#else 
#define LOGV printf
#define LOGD printf
#define LOGI printf
#define LOGW printf
#define LOGE printf
#define ALOGE printf
#endif

//#define LOG_FUNCTION_NAME LOGI("%s-%d\n",__FUNCTION__,__LINE__);
#define LOG_FUNCTION_NAME
#define BUF_LEN 512
#define MAX_REGION 6

int set_tvp_enable(int enable)
{
    int fd;
    char bcmd[16];
    fd = open(TVP_ENABLE_PATH, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd >= 0) {
        sprintf(bcmd, "%d", enable);
        write(fd, bcmd, strlen(bcmd));
        close(fd);
        return 0;
    }

    return -1;
}

int free_keep_buffer(void)
{
    int fd;
    char bcmd[16];
    fd = open(FREE_KEEP_BUFFER_PATH, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd >= 0) {
        sprintf(bcmd, "%d", 1);
        write(fd, bcmd, strlen(bcmd));
        close(fd);
        return 0;
    }

    return -1;
}

int free_cma_buffer(void)
{
    int fd;
    char bcmd[16];
    fd = open(FREE_CMA_BUFFER_PATH, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd >= 0) {
        sprintf(bcmd, "%d", 1);
        write(fd, bcmd, strlen(bcmd));
        close(fd);
        return 0;
    }

    return -1;
}

int set_vfmmap_ppmgr_di(int enable)
{
    int fd;
    char bcmd[128];
    fd = open(VFM_DEF_MAP_PATH, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd >= 0) {
        sprintf(bcmd, "rm default");
        write(fd, bcmd, strlen(bcmd));
        if (enable)
            sprintf(bcmd, "add default decoder ppmgr deinterlace amvideo");
        else
            sprintf(bcmd, "add default decoder amvideo");
        write(fd, bcmd, strlen(bcmd));
        close(fd);
        return 0;
    }
    return -1;
}

int set_disable_video(int mode)
{
    int fd;
    char  bcmd[16];
    fd = open(DISABLE_VIDEO_PATH, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd >= 0) {
        sprintf(bcmd, "%d", mode);
        write(fd, bcmd, strlen(bcmd));
        close(fd);
        return 0;
    }

    return -1;
}

int tvp_mm_enable(int flags)
{
    //flags: bit 1---4k ;
    int is_4k= flags &TVP_MM_ENABLE_FLAGS_FOR_4K;
    free_keep_buffer();
    //set_vfmmap_ppmgr_di(0);
    if (is_4k)
        set_tvp_enable(2);
    else
        set_tvp_enable(1);
    return 0;

}

int tvp_mm_disable(int flags)
{
    set_disable_video(0);
    free_keep_buffer();
    //set_vfmmap_ppmgr_di(1);
    set_tvp_enable(0);
    /*unused flags*/
    return 0;
}

int tvp_mm_get_mem_region(struct tvp_region* region, int region_size)
{
    int fd, len;
    char buf[BUF_LEN];
    uint32_t n=0, i=0, rnum = 0, siz;
    uint64_t start=0, end=0;

    //rnum = min(region_size/sizeof(struct tvp_region), MAX_REGION);
    rnum = region_size/sizeof(struct tvp_region);

    fd = open(TVP_REGION_PATH, O_RDONLY, 0644);
    if (fd >= 0) {
        len = read(fd, buf, BUF_LEN);
        close(fd);
        for (i=0,n=0; (n < len) && (i < rnum); i++, region++) {
            if (4 == sscanf(buf+n, "segment%d:%llx - %llx (size:%x)",
                    &i, &start, &end, &siz))
            {
                ALOGE("segment %d: [%llx-%llx]\n", i, start, end);
                region->start = start;
                region->end = end;
                region->mem_flags = 0;
                n += strcspn(buf+n, "\n") + 1;
            }
        }
        return i;
    }
    return -1;
}

int tvp_mm_get_enable()
{
    int fd, len;
    char buf[BUF_LEN];
    int flag = -1;
    fd = open(TVP_ENABLE_PATH, O_RDONLY);
    if (fd >= 0) {
        len = read(fd, buf, BUF_LEN);
        close(fd);
        if (1 != sscanf(buf, "tvp_flag=%d\n", &flag)) {
            flag = 0xFF; /*compat with old kernel */
        }
    }
    return flag;
}

