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

/* Filesystem Model 

/ - mount point
/interfaces/
/interfaces/name1/stuff
/interfaces/name2/stuff2
/interfaces/name3/stuff3
/bus0/
/bus0/0x11-Unknown/
/bus0/node-0x11/stuff
/bus0/
*/

MRBFSFileNode* mrbfsTraversePath(const char* inputPath, MRBFSFileNode* rootNode, MRBFSFileNode** parentDirectoryNode)
{
	char* dirpath = dirname(strdupa(inputPath));
	char* filename = basename(strdupa(inputPath));
	int i=0, match=0;
	char* tmp = NULL;
	MRBFSFileNode* dirNode = rootNode, *fileNode = NULL;
	

	mrbfsLogMessage(MRBFS_LOG_ANNOYING, "mrbfsTraversePath(%s), dirpath=[%s], filename=[%s]", inputPath, dirpath, filename);
	if (0 == strcmp(filename, "/"))
	{
		mrbfsLogMessage(MRBFS_LOG_ANNOYING, "mrbfsTraversePath(%s) - Looking for root, returning root", inputPath);
		if (NULL != parentDirectoryNode)
			*parentDirectoryNode = NULL;
		return(rootNode);
	}

   if (dirpath[0] == '/')
      dirpath++;

	pthread_mutex_lock(&gMrbfsConfig->fsLock);

	if (strlen(dirpath))
	{
		mrbfsLogMessage(MRBFS_LOG_ANNOYING, "mrbfsTraversePath(%s) - starting directory traversal", inputPath);

		// Go traverse to the right depth
		for (tmp = strsep(&dirpath, "/"); NULL != tmp; tmp = strsep(&dirpath, "/"))
		{
			i=0;
			match=0;
			dirNode = dirNode->childPtr;
			mrbfsLogMessage(MRBFS_LOG_ANNOYING, "Looking for [%s]", tmp);			

			while(NULL != dirNode)
			{
				mrbfsLogMessage(MRBFS_LOG_ANNOYING, "Examining [%s] as [%s]", dirNode->fileName, tmp);						
			
				if (0 == strcmp(tmp, dirNode->fileName) && (FNODE_DIR == dirNode->fileType || FNODE_DIR_NODE == dirNode->fileType))
				{
					match = 1;
					break;
				}
				dirNode = dirNode->siblingPtr;
			}
			if (0 == match)
			{
				pthread_mutex_unlock(&gMrbfsConfig->fsLock);
				return(NULL);
			}
		}
	}
	
	fileNode = dirNode->childPtr;
	if (NULL != parentDirectoryNode)
		*parentDirectoryNode = dirNode;	

	while(NULL != fileNode)
	{
		mrbfsLogMessage(MRBFS_LOG_ANNOYING, "mrbfsTraversePath(%s) - examining [%s] as [%s]", inputPath, fileNode->fileName, filename);	
		if (0 == strcmp(filename, fileNode->fileName))
		{
				pthread_mutex_unlock(&gMrbfsConfig->fsLock);
				return (fileNode);
		}
		fileNode = fileNode->siblingPtr;
	}	

	pthread_mutex_unlock(&gMrbfsConfig->fsLock);
	return(NULL);
}

MRBFSFileNode* mrbfsFilesystemAddFile(const char* fileName, MRBFSFileNodeType fileType, const char* insertionPath)
{
	MRBFSFileNode* node = calloc(1, sizeof(MRBFSFileNode));
	node->fileName = strdup(fileName);
	node->fileType = fileType;
	node->updateTime = node->accessTime = time(NULL);
	return(mrbfsAddFileNode(insertionPath, node));
}

int mrbfsFilesystemInitialize()
{
	mrbfsLogMessage(MRBFS_LOG_INFO, "Setting up filesystem root");
	pthread_mutexattr_t lockAttr;
	
	// Initialize the bus lock
	pthread_mutexattr_init(&lockAttr);
	pthread_mutexattr_settype(&lockAttr, PTHREAD_MUTEX_ADAPTIVE_NP);
	pthread_mutex_init(&gMrbfsConfig->fsLock, &lockAttr);
	pthread_mutexattr_destroy(&lockAttr);	
	
	pthread_mutex_lock(&gMrbfsConfig->fsLock);
	gMrbfsConfig->rootNode = calloc(1, sizeof(MRBFSFileNode));
	gMrbfsConfig->rootNode->fileName = strdup("/");
	gMrbfsConfig->rootNode->fileType = FNODE_DIR;
	pthread_mutex_unlock(&gMrbfsConfig->fsLock);

	mrbfsFilesystemAddFile("interfaces", FNODE_DIR, "/");
	mrbfsFilesystemAddFile("stats", FNODE_DIR, "/");	
	return(0);
}


int mrbfsFilesystemDestroy()
{
	// FIXME:  Do stuff here
	return(0);

}

MRBFSFileNode* mrbfsAddFileNode(const char* insertionPath, MRBFSFileNode* addNode)
{
	int i;
	MRBFSFileNode *insertionNode, *node, *prevnode, *parentNode;
	
	mrbfsLogMessage(MRBFS_LOG_INFO, "Adding node [%s] to directory [%s]", addNode->fileName, insertionPath);
	
	insertionNode = mrbfsTraversePath(insertionPath, gMrbfsConfig->rootNode, &parentNode);
	
	if (NULL != insertionNode)
		mrbfsLogMessage(MRBFS_LOG_ANNOYING, "mrbfsTraversePath() returned node [%s] - childPtr=%08X siblingPtr=%08X", insertionNode->fileName, insertionNode->childPtr, insertionNode->siblingPtr);	
	
	if (NULL == insertionNode || (insertionNode->fileType != FNODE_DIR && insertionNode->fileType != FNODE_DIR_NODE))
	{
		mrbfsLogMessage(MRBFS_LOG_ERROR, "Cannot insert node [%s] into [%s]", insertionNode->fileName, insertionPath);
		return(NULL);
	}

	pthread_mutex_lock(&gMrbfsConfig->fsLock);

	if (NULL == insertionNode->childPtr)
	{
		mrbfsLogMessage(MRBFS_LOG_ANNOYING, "Directory [%s] has no child ptr, adding [%s]", insertionNode->fileName, addNode->fileName);
		insertionNode->childPtr = addNode;
		addNode->childPtr = NULL;
		addNode->siblingPtr = NULL;		
	}
	else
	{
		mrbfsLogMessage(MRBFS_LOG_ANNOYING, "Directory [%s] has children, searching for add point for [%s]", insertionNode->fileName, addNode->fileName);

		node = insertionNode->childPtr;
		prevnode = NULL;

		do
		{
			mrbfsLogMessage(MRBFS_LOG_ANNOYING, "Examining node [%s] - childPtr=%08X siblingPtr=%08X", node->fileName, node->childPtr, node->siblingPtr);	

			if (0 == strcmp(node->fileName, addNode->fileName))
			{
				mrbfsLogMessage(MRBFS_LOG_ERROR, "Node of name [%s] already exists", addNode->fileName);
				return(NULL);
			} 
			else if (0 > strcmp(addNode->fileName, node->fileName))
			{
				// Insert here on the front end - need to change parent child ptr
				if (NULL == prevnode)
				{
					mrbfsLogMessage(MRBFS_LOG_ANNOYING, "Adding [%s] on front of chain for node [%s]", addNode->fileName, insertionNode->fileName);	
					insertionNode->childPtr = addNode;
				}
				else
				{
					mrbfsLogMessage(MRBFS_LOG_ANNOYING, "Adding [%s] before node [%s] and after node [%s]", addNode->fileName, node->fileName, prevnode->fileName);	
					prevnode->siblingPtr = addNode;
				}
				
				addNode->childPtr = NULL;
				addNode->siblingPtr = node;
				break;
			}
			else
			{
				if (NULL == node->siblingPtr)
				{
					mrbfsLogMessage(MRBFS_LOG_ANNOYING, "Adding [%s] on the end of the chain", addNode->fileName);
					node->siblingPtr = addNode;
					addNode->childPtr = NULL;
					addNode->siblingPtr = NULL;
					break;
				}

				mrbfsLogMessage(MRBFS_LOG_ANNOYING, "Advancing");	
				prevnode = node;
				node = node->siblingPtr;
			}
		} while (node != NULL);
	}
	pthread_mutex_unlock(&gMrbfsConfig->fsLock);

	return(addNode);
}

int mrbfsGetattr(const char *path, struct stat *stbuf)
{
	int retval = -ENOENT;
	MRBFSFileNode *parentNode, *fileNode = mrbfsTraversePath(path, gMrbfsConfig->rootNode, &parentNode);
	struct fuse_context *fc = fuse_get_context();
	
	if (NULL == fileNode)
	{
		mrbfsLogMessage(MRBFS_LOG_ANNOYING, "mrbfsGetattr(%s) returned NULL", path);
		return(-ENOENT);
	}
	mrbfsLogMessage(MRBFS_LOG_ANNOYING, "mrbfsGetattr(%s), fileNode=[%s]", path, fileNode->fileName);
	
	stbuf->st_uid = fc->uid;
	stbuf->st_gid = fc->gid;
	stbuf->st_ctime = stbuf->st_mtime = fileNode->updateTime;
	stbuf->st_atime = fileNode->accessTime;
	
	switch(fileNode->fileType)
	{
		case FNODE_DIR_NODE:
		case FNODE_DIR:
			stbuf->st_mode = S_IFDIR | 0555;
			stbuf->st_nlink = 2;
			retval = 0;
			break;

		case FNODE_RO_VALUE_STR:
			stbuf->st_mode = S_IFREG | 0444;
			stbuf->st_nlink = 1;
			stbuf->st_size = strlen(fileNode->value.valueStr);
			retval = 0;			
			break;

		case FNODE_RO_VALUE_INT:
			stbuf->st_mode = S_IFREG | 0444;
			stbuf->st_nlink = 1;
			stbuf->st_size = 1;
			retval = 0;			
			break;

		case FNODE_RW_VALUE_STR:
			if (NULL != fileNode->mrbfsFileNodeWrite)
				stbuf->st_mode = S_IFREG | 0664;
			else
				stbuf->st_mode = S_IFREG | 0444;
			stbuf->st_nlink = 1;
			stbuf->st_size = strlen(fileNode->value.valueStr);
			retval = 0;			
			break;
			
		case FNODE_RW_VALUE_INT:
			if (NULL != fileNode->mrbfsFileNodeWrite)
				stbuf->st_mode = S_IFREG | 0664;
			else
				stbuf->st_mode = S_IFREG | 0444;

			stbuf->st_nlink = 1;
			stbuf->st_size = 1;
			retval = 0;			
			break;

		case FNODE_RO_VALUE_READBACK:
		case FNODE_RW_VALUE_READBACK:
			if (NULL != fileNode->mrbfsFileNodeWrite)
				stbuf->st_mode = S_IFREG | 0220;
			if (NULL != fileNode->mrbfsFileNodeRead)
				stbuf->st_mode = S_IFREG | 0444;

			stbuf->st_nlink = 1;
			stbuf->st_size = 1;
			retval = 0;			
			break;
					
		case FNODE_END_OF_LIST:
			return(retval);
	}
	return(retval);
}


int mrbfsReaddir(const char *path, void *buf, fuse_fill_dir_t filler,
			 off_t offset, struct fuse_file_info *fi)
{
	int i=0;
	MRBFSFileNode *parentNode, *fileNode = mrbfsTraversePath(path, gMrbfsConfig->rootNode, &parentNode);
	
	mrbfsLogMessage(MRBFS_LOG_ANNOYING, "mrbfsReaddir(%s), fileNode=%p", path, fileNode);
	
	if (NULL == fileNode || (fileNode->fileType != FNODE_DIR && fileNode->fileType != FNODE_DIR_NODE))
		return -ENOENT;

	mrbfsLogMessage(MRBFS_LOG_ANNOYING, "mrbfsReaddir(%s) - got back filenode[%s], childPtr=%08X", path, fileNode->fileName, fileNode->childPtr);

	// It's a directory, auto-populate . and ..
	filler(buf, ".", NULL, 0);	
	filler(buf, "..", NULL, 0);	

	fileNode = fileNode->childPtr;
	while (NULL != fileNode)
	{
		filler(buf, fileNode->fileName, NULL, 0);
		fileNode = fileNode->siblingPtr;
	}

	return(0);
}

int mrbfsOpen(const char *path, struct fuse_file_info *fi)
{
	int retval = -ENOENT;
	MRBFSFileNode *parentNode, *fileNode = mrbfsTraversePath(path, gMrbfsConfig->rootNode, &parentNode);

	if (NULL == fileNode)
	{
		mrbfsLogMessage(MRBFS_LOG_DEBUG, "mrbfsOpen(%s) - path [%s] not valid", path);
		return(-ENOENT);
	}
	mrbfsLogMessage(MRBFS_LOG_DEBUG, "mrbfsOpen(%s) found file", path);

	if ( ((fi->flags & (O_RDONLY|O_WRONLY|O_RDWR)) != O_RDONLY)
		&& ( (fileNode->fileType != FNODE_RW_VALUE_STR && fileNode->fileType != FNODE_RW_VALUE_INT && fileNode->fileType != FNODE_RW_VALUE_READBACK) || (NULL == fileNode->mrbfsFileNodeWrite)) )
	{
		mrbfsLogMessage(MRBFS_LOG_DEBUG, "mrbfsOpen(%s) rejected - not writable node", path);
		return -EACCES;
	}
	mrbfsLogMessage(MRBFS_LOG_DEBUG, "mrbfsOpen(%s) successful", path);
	fi->direct_io = 1;
	return 0;
}

int mrbfsRead(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
	MRBFSFileNode *parentNode, *fileNode = mrbfsTraversePath(path, gMrbfsConfig->rootNode, &parentNode);
	if (NULL == fileNode)
	{
		mrbfsLogMessage(MRBFS_LOG_DEBUG, "mrbfsRead(%s) - path [%s] not valid", path);
		return(-ENOENT);
	}
	
	switch(fileNode->fileType)
	{
		case FNODE_DIR_NODE:
		case FNODE_DIR:
			return(-ENOENT);

		case FNODE_RO_VALUE_INT:
		case FNODE_RW_VALUE_INT:
			{
				char intval[32];
				size_t len=0;
				
				memset(intval, 0, sizeof(intval));	
				sprintf(intval, "%d\n", fileNode->value.valueInt);
		
				len = strlen(intval);
				mrbfsLogMessage(MRBFS_LOG_DEBUG, "mrbfsRead(%s) - string[%s], len[%d], offset[%d], size[%d]", fileNode->fileName, intval, len, offset, size);

				if (offset < len) 
				{
					if (offset + size > len)
						size = len - offset;
					memcpy(buf, intval + offset, size);
				} else
					size = 0;		
			}
			break;
		case FNODE_RO_VALUE_STR:
		case FNODE_RW_VALUE_STR:		
			{
				size_t len=0;
				
				len = strlen(fileNode->value.valueStr);
				mrbfsLogMessage(MRBFS_LOG_DEBUG, "mrbfsRead(%s) - string value, len[%d], offset[%d], size[%d]", fileNode->fileName, len, offset, size);

				if (offset < len) 
				{
					if (offset + size > len)
						size = len - offset;
					memcpy(buf, fileNode->value.valueStr + offset, size);
				} else
					size = 0;		



				mrbfsLogMessage(MRBFS_LOG_DEBUG, "mrbfsRead(%s) - reading str, value [%s]", fileNode->fileName, fileNode->value.valueStr);		
			}
			break;

		case FNODE_RO_VALUE_READBACK:
		case FNODE_RW_VALUE_READBACK:
			if (NULL == fileNode->mrbfsFileNodeRead)
				return(-ENOENT);
			mrbfsLogMessage(MRBFS_LOG_DEBUG, "mrbfsRead(%s) - readback function called offset[%d], size[%d]", fileNode->fileName, offset, size);
			size = (*fileNode->mrbfsFileNodeRead)(fileNode, buf, size, offset);
			if (size >= 0)
				mrbfsLogMessage(MRBFS_LOG_DEBUG, "mrbfsRead(%s) - readback, value [%.*s] size [%d]", fileNode->fileName, size, buf, size);
			else
				mrbfsLogMessage(MRBFS_LOG_DEBUG, "mrbfsRead(%s) - readback error, size=%d", fileNode->fileName, size);
			break;
	}
	
	return(size);
}

int mrbfsTruncate(const char *path, off_t offset)
{
	MRBFSFileNode *parentNode, *fileNode = mrbfsTraversePath(path, gMrbfsConfig->rootNode, &parentNode);
	if (NULL == fileNode)
	{
		mrbfsLogMessage(MRBFS_LOG_DEBUG, "mrbfsTruncate(%s) - path [%s] not valid", path);
		return(-ENOENT);
	}
	
	if (!(fileNode->fileType == FNODE_RW_VALUE_STR || fileNode->fileType == FNODE_RW_VALUE_INT || fileNode->fileType == FNODE_RW_VALUE_READBACK) || (NULL == fileNode->mrbfsFileNodeWrite))
	{
		mrbfsLogMessage(MRBFS_LOG_DEBUG, "mrbfsTruncate(%s) rejected - not writable node", path);
		return -EACCES;
	}

	return 0;
}

int mrbfsWrite(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
	MRBFSFileNode *parentNode, *fileNode = mrbfsTraversePath(path, gMrbfsConfig->rootNode, &parentNode);
	if (NULL == fileNode)
	{
		mrbfsLogMessage(MRBFS_LOG_DEBUG, "mrbfsWrite(%s) - path [%s] not valid", path);
		return(-ENOENT);
	}
	
	if (!(fileNode->fileType == FNODE_RW_VALUE_STR || fileNode->fileType == FNODE_RW_VALUE_INT || fileNode->fileType == FNODE_RW_VALUE_READBACK) || (NULL == fileNode->mrbfsFileNodeWrite))
	{
		mrbfsLogMessage(MRBFS_LOG_DEBUG, "mrbfsWrite(%s) rejected - not writable node", path);
		return -EACCES;
	}

	switch(fileNode->fileType)
	{
		case FNODE_DIR_NODE:
		case FNODE_DIR:
			return(-ENOENT);

		case FNODE_RW_VALUE_INT:
		case FNODE_RW_VALUE_STR:
		case FNODE_RW_VALUE_READBACK:	
			{
				fileNode->mrbfsFileNodeWrite(fileNode, buf, size);
				mrbfsLogMessage(MRBFS_LOG_DEBUG, "mrbfsWrite(%s) - write string[%s], len[%d]", fileNode->fileName, buf, size);
			}
			break;
	}
	
	return(size);
}


