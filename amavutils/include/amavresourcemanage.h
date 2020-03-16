/*
 * amavresourcemanage.h
 *
 * Copyright (C) 2019 Amlogic, Inc. All rights reserved.
 *
 */

#ifndef _AMAV_RESOURCE_MANAGE_H_
#define _AMAV_RESOURCE_MANAGE_H_

#ifdef  __cplusplus
extern "C" {
#endif

#define    RESMAN_IOC_MAGIC  'R'

#define    RESMAN_IOC_QUERY_RES          _IOR(RESMAN_IOC_MAGIC, 0x01, int)
#define    RESMAN_IOC_ACQUIRE_RES        _IOW(RESMAN_IOC_MAGIC, 0x02, int)
#define    RESMAN_IOC_RELEASE_RES        _IOR(RESMAN_IOC_MAGIC, 0x03, int)
#define    RESMAN_IOC_SETAPPINFO         _IOW(RESMAN_IOC_MAGIC, 0x04, int)
#define    RESMAN_IOC_SUPPORT_RES        _IOR(RESMAN_IOC_MAGIC, 0x05, int)

#define BASE_AVAILAB_RES               (0x0)
#define VFM_DEFAULT                    (BASE_AVAILAB_RES + 0)
#define AMVIDEO                        (BASE_AVAILAB_RES + 1)
#define PIPVIDEO                       (BASE_AVAILAB_RES + 2)
#define SEC_TVP                        (BASE_AVAILAB_RES + 3)
#define MAX_AVAILAB_RES                (SEC_TVP + 1)

struct resman_para {
    int para_in;
    char para_str[32];
};

struct app_info {
    char appname[32];
    int  apptype;
};

typedef enum {
    TYPE_NONE     = -1,
    TYPE_OMX      = 0,
    TYPE_DVB,
    TYPE_HDMI_IN,
    TYPE_SEC_TVP,
    TYPE_OTHER    = 10,
}APP_TYPE;

bool amav_resman_support(void);
int amav_resman_init(const char *appname, int type);
int amav_resman_close(int handle);
int amav_resman_setappinfo(int handle, struct app_info *appinfo);
int amav_resman_acquire(int handle, int restype);
int amav_resman_release(int handle, int restype);
int amav_resman_query(int handle, struct resman_para *res_status);
bool amav_resman_acquire_wait(int handle, int restype, const unsigned int time_out);//Timeout unit: milliseconds
bool amav_resman_resource_support(const char* resname);

#ifdef  __cplusplus
}
#endif

#endif /** _AMAV_RESOURCE_MANAGE_H_ **/
