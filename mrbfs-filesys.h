#ifndef _MRBFS_FILESYS_H
#define _MRBFS_FILESYS_H

int mrbfsGetattr(const char *path, struct stat *stbuf);
int mrbfsReaddir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi);
int mrbfsOpen(const char *path, struct fuse_file_info *fi);
int mrbfsRead(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi);

int mrbfsFilesystemInitialize();
int mrbfsFilesystemDestroy();
MRBFSFileNode* mrbfsFilesystemAddFile(const char* fileName, MRBFSFileNodeType fileType, const char* insertionPath);
MRBFSFileNode* mrbfsAddFileNode(const char* insertionPath, MRBFSFileNode* addNode);

#endif

