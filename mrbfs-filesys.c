#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <libgen.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <termios.h>
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#define FUSE_USE_VERSION 26
#include <fuse.h>
#include "mrbfs.h"
#include "mrbfs-filesys.h"

int mrbfsGetattr(const char *path, struct stat *stbuf)
{
	return -ENOENT;
}

int mrbfsReaddir(const char *path, void *buf, fuse_fill_dir_t filler,
			 off_t offset, struct fuse_file_info *fi)
{
	return -ENOENT;
}

int mrbfsOpen(const char *path, struct fuse_file_info *fi)
{
	if (0 != strcmp("/pktsReceived", path))
		return -ENOENT;

	if ((fi->flags & 3) != O_RDONLY)
		return -EACCES;
	fi->direct_io = 1;


	return 0;
}

int mrbfsRead(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
	size_t len;
	(void) fi;
	char pktsReceivedStr[32];
	UINT32 pktsReceived;
	

	if(strcmp(path, "/pktsReceived") != 0)
		return -ENOENT;

	memset(pktsReceivedStr, 0, sizeof(pktsReceivedStr));
	sprintf(pktsReceivedStr, "%u\n", (unsigned int)pktsReceived);

	len = strlen(pktsReceivedStr);
	if (offset < len) {
		if (offset + size > len)
			size = len - offset;
		memcpy(buf, pktsReceivedStr + offset, size);
	} else
		size = 0;

	return size;
}
