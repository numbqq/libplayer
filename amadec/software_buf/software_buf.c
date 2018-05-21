

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <signal.h>
#include <pthread.h>
#include <unistd.h>
#include <math.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/prctl.h>
#include <audio-dec.h>

#define WRITE_MARGIN 16

static inline int GetWriteSpace(unsigned char *WritePoint, unsigned char *ReadPoint, int buffer_size) {
    int bytes;

    if (WritePoint >= ReadPoint) {
        bytes = buffer_size - 1 - (WritePoint - ReadPoint);
    } else {
        bytes = ReadPoint - WritePoint - 1;
    }

    if (bytes >= WRITE_MARGIN)
        bytes -= WRITE_MARGIN;
    return bytes;
}

static inline size_t GetReadSpace(unsigned char *WritePoint, unsigned char *ReadPoint, int buffer_size) {
    int bytes;

    if (WritePoint >= ReadPoint) {
        bytes = WritePoint - ReadPoint;
    } else {
        bytes = buffer_size - (ReadPoint - WritePoint);
    }
    return bytes;
}

static inline int write_to_buffer(unsigned char *current_pointer, unsigned char *buffer, int bytes,
        unsigned char *start_buffer, int buffer_size) {
    int left_bytes = start_buffer + buffer_size - current_pointer;

    if (left_bytes >= bytes) {
        memcpy(current_pointer, buffer, bytes);
    } else {
        memcpy(current_pointer, buffer, left_bytes);
        memcpy(start_buffer, buffer + left_bytes, bytes - left_bytes);
    }
    return 0;
}

static inline int read_from_buffer(unsigned char *current_pointer, unsigned char *buffer, int bytes,
        unsigned char *start_buffer, int buffer_size) {
    int left_bytes = start_buffer + buffer_size - current_pointer;

    if (left_bytes >= bytes) {
        memcpy(buffer, current_pointer, bytes);
    } else {
        memcpy(buffer, current_pointer, left_bytes);
        memcpy(buffer + left_bytes, start_buffer, bytes - left_bytes);
    }
    return 0;
}

static inline void* update_pointer(unsigned char *current_pointer, int bytes,
        unsigned char *start_buffer, int buffer_size) {
    current_pointer += bytes;
    if (current_pointer >= start_buffer + buffer_size) {
        current_pointer -= buffer_size;
    }
    return current_pointer;
}


int buffer_write(struct circle_buffer_s *tmp, unsigned char *buffer, size_t bytes) {
    circle_buffer *buf = tmp;
    pthread_mutex_lock(&buf->lock);
    if (buf->start_add == NULL || buf->rd == NULL || buf->wr == NULL
            || buf->size == 0) {
        adec_print("%s, Buffer malloc fail!\n", __FUNCTION__);
        pthread_mutex_unlock(&buf->lock);
        return -1;
    }
    size_t write_size = GetWriteSpace(buf->wr, buf->rd, buf->size);
    if (write_size > bytes)
        write_size = bytes;
    if (write_size > 0) {
        write_to_buffer(buf->wr, buffer, write_size, buf->start_add, buf->size);
        buf->wr = update_pointer(buf->wr, write_size, buf->start_add, buf->size);
    }
    pthread_mutex_unlock(&buf->lock);
    return write_size;
}

int buffer_read(struct circle_buffer_s *tmp, unsigned char* buffer, size_t bytes) {
    circle_buffer *buf = tmp;
    pthread_mutex_lock(&buf->lock);

    if (buf->start_add == NULL || buf->rd == NULL || buf->wr == NULL
            || buf->size == 0) {
        adec_print("%s, Buffer malloc fail!\n", __FUNCTION__);
        pthread_mutex_unlock(&buf->lock);
        return -1;
    }
    size_t read_space = GetReadSpace(buf->wr, buf->rd, buf->size);
    //adec_print("read_space:%d  buf->wr:%0x buf->rd:%0x buf->size:%d\n",read_space,buf->wr,buf->rd,buf->size);
    if (read_space <= bytes) {
        pthread_mutex_unlock(&buf->lock);
        return -1;
    }
    read_from_buffer(buf->rd, buffer, bytes, buf->start_add, buf->size);
    buf->rd = update_pointer(buf->rd, bytes, buf->start_add, buf->size);
    pthread_mutex_unlock(&buf->lock);
    return bytes;
}

