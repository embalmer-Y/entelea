/* SPDX-License-Identifier: Apache-2.0 */
#ifndef TEST_FAKE_FF_H
#define TEST_FAKE_FF_H

typedef struct {
	int placeholder;
} FATFS;
typedef struct {
	int placeholder;
} FIL;
typedef enum {
	FR_OK,
	FR_DISK_ERR,
	FR_EXIST,
} FRESULT;

#define FA_READ 0x01
#define FA_WRITE 0x02
#define FA_CREATE_NEW 0x04

FRESULT f_open(FIL *file, const char *path, unsigned char mode);
FRESULT f_write(FIL *file, const void *buf, unsigned int size, unsigned int *written);
FRESULT f_read(FIL *file, void *buf, unsigned int size, unsigned int *read);
FRESULT f_sync(FIL *file);
FRESULT f_close(FIL *file);
FRESULT f_unlink(const char *path);

#endif
