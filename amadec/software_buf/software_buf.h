#ifndef SOFTWARE_BUFFER_H
#define SOFTWARE_BUFFER_H

extern int buffer_read(struct circle_buffer_s *tmp, unsigned char* buffer, size_t bytes);
extern int buffer_write(struct circle_buffer_s *tmp, unsigned char* buffer, size_t bytes);

#endif /* SOFTWARE_BUFFER_H */

