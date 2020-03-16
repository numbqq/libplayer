/*
 * amavresmanage.c
 *
 * Copyright (C) 2019 Amlogic, Inc. All rights reserved.
 *
 */

#define  LOG_TAG    "amavresman"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/poll.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdbool.h>
#ifdef ANDROID
#include <log/log.h>
#else
#define ALOGD(fmt, ...) printf(fmt "\n", ##__VA_ARGS__)
#define ALOGE(fmt, ...) printf(fmt "\n", ##__VA_ARGS__)
#endif
#include "amavresourcemanage.h"

#define RESMAN_DEV "/dev/amresource_mgr"
/* -----------------------------------------------------------------------
* @brief       Check if resource manage device exists
--------------------------------------------------------------------------*/
bool amav_resman_support(void)
{
    if (access(RESMAN_DEV, R_OK | W_OK) == 0)
        return true;
    else
    {
        ALOGD("Don't Support Resource Manage!");
        return false;
    }
}
/* -----------------------------------------------------------------------
* @brief       init resoure manage.
--------------------------------------------------------------------------*/

int amav_resman_init(const char *appname, int type)
{
    int fd = -1;
    struct app_info appinfo;

    fd = open(RESMAN_DEV, O_RDWR | O_TRUNC);
    if (fd < 0)
       return fd;

    memset(&appinfo, 0, sizeof(appinfo));

    strncpy(appinfo.appname, appname, sizeof(appinfo.appname));
    appinfo.appname[sizeof(appinfo.appname)-1] = '\0';
    appinfo.apptype = type;

    amav_resman_setappinfo(fd, &appinfo);

    return fd;
}
/* -----------------------------------------------------------------------
* @brief       close resoure manage.
--------------------------------------------------------------------------*/
int amav_resman_close(int handle)
{
    if (handle >= 0)
    {
        close(handle);
    }else
        return -1;

    return 0;
}
/* -----------------------------------------------------------------------
* @brief       set app info
--------------------------------------------------------------------------*/
int amav_resman_setappinfo(int handle, struct app_info *appinfo)
{
    int r = -1;
    if (handle >= 0)
    {
        r = ioctl(handle, RESMAN_IOC_SETAPPINFO, (unsigned long)appinfo);
        if (r < 0)
        {
            ALOGE("%s error, ret=%d errno=%d\n", __FUNCTION__, r, errno);
        }
    }
    return r;
}
/* -----------------------------------------------------------------------
* @brief       acquire resource.
--------------------------------------------------------------------------*/
int amav_resman_acquire(int handle, int restype)
{
    int r = -1;
    if (handle >= 0)
    {
        r = ioctl(handle, RESMAN_IOC_ACQUIRE_RES, (unsigned long)restype);
    }
    return r;
}
/* -----------------------------------------------------------------------
* @brief       release resource.
--------------------------------------------------------------------------*/
int amav_resman_release(int handle, int restype)
{
    int r = -1;
    if (handle >= 0)
    {
        r = ioctl(handle, RESMAN_IOC_RELEASE_RES, (unsigned long)restype);
        if (r < 0)
        {
            ALOGE("%s error, ret=%d errno=%d\n", __FUNCTION__, r, errno);
        }
    }
    return r;
}
/* -----------------------------------------------------------------------
* @brief       query resoure.
--------------------------------------------------------------------------*/
int amav_resman_query(int handle, struct resman_para *res_status)
{
    int r = -1;
    if (handle >= 0 && res_status)
    {
        r = ioctl(handle, RESMAN_IOC_QUERY_RES, (unsigned long)res_status);
        if (r < 0)
        {
            ALOGE("%s error, ret=%d errno=%d\n", __FUNCTION__, r, errno);
        }
    }
    return r;
}
/* -----------------------------------------------------------------------
* @brief       acquire resource with time out, timeout unit: milliseconds.
--------------------------------------------------------------------------*/
bool amav_resman_acquire_wait(int handle, int restype, const unsigned int time_out)
{
    unsigned int times = 1;

    if (handle < 0)
        return false;

    while (1)
    {
        if (amav_resman_acquire(handle, restype) < 0)
        {
            usleep(1000);
            if (times++ >= time_out)
                return false;
        }else
            return true;
    }
    return true;
}
/* -----------------------------------------------------------------------
* @brief       check whether the current resource is supported
--------------------------------------------------------------------------*/
bool amav_resman_resource_support(const char* resname)
{
    int fd = -1;
    struct resman_para checkres;

    fd = open(RESMAN_DEV, O_RDWR | O_TRUNC);
    if (fd < 0 || !resname)
       return false;

    memset(&checkres, 0, sizeof(checkres));
    checkres.para_in = strlen(resname);
    strncpy(checkres.para_str, resname, sizeof(checkres.para_str));

    if (ioctl(fd, RESMAN_IOC_SUPPORT_RES, (unsigned long)&checkres) < 0)
    {
        close(fd);
        return false;
    }
    close(fd);
    return true;
}
