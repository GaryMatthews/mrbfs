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
	

	mrbfsLogMessage(MRBFS_LOG_DEBUG, "mrbfsTraversePath(%s), dirpath=[%s], filename=[%s]", inputPath, dirpath, filename);
	if (0 == strcmp(filename, "/"))
	{
		if (NULL != parentDirectoryNode)
			*parentDirectoryNode = NULL;
		return(rootNode);
	}

   if (dirpath[0] == '/')
      dirpath++;

	if (strlen(dirpath))
	{
		mrbfsLogMessage(MRBFS_LOG_DEBUG, "Traversing directories");

		// Go traverse to the right depth
		for (tmp = strsep(&dirpath, "/"); NULL != tmp; tmp = strsep(&dirpath, "/"))
		{
			i=0;
			match=0;
			dirNode = dirNode->childPtr;
			mrbfsLogMessage(MRBFS_LOG_DEBUG, "Looking for [%s]", tmp);			

			while(NULL != dirNode)
			{
				mrbfsLogMessage(MRBFS_LOG_DEBUG, "Examining [%s] as [%s]", dirNode->fileName, tmp);						
			
				if (0 == strcmp(tmp, dirNode->fileName) && FNODE_DIR == dirNode->fileType)
				{
					match = 1;
					break;
				}
				dirNode = dirNode->siblingPtr;
			}
			if (0 == match)
				return(NULL);
		}
	}
	
	i=0;
	match=0;
	
	fileNode = dirNode->childPtr;
	if (NULL != parentDirectoryNode)
		*parentDirectoryNode = dirNode;	

	while(NULL != fileNode)
	{
		mrbfsLogMessage(MRBFS_LOG_DEBUG, "Examining [%s] as [%s]", fileNode->fileName, filename);	
		if (0 == strcmp(filename, fileNode->fileName))
		{
//				if (FNODE_DIR == fileNode[i].type)
//					return(fileNode[i].value.dirFnodeList);
				return (&fileNode[i]);
		}
		fileNode = fileNode->siblingPtr;
	}	

	return(NULL);
}


MRBFSFileNode* mrbfsAddFileNode(const char* insertionPath, MRBFSFileNode* addNode)
{
	int i;
	MRBFSFileNode *insertionNode, *node, *prevnode, *parentNode;
	
	if (NULL == gMrbfsConfig->rootNode)
	{
		mrbfsLogMessage(MRBFS_LOG_INFO, "Setting up filesystem root");
		gMrbfsConfig->rootNode = calloc(1, sizeof(MRBFSFileNode));
		gMrbfsConfig->rootNode->fileName = strdup("/");
		gMrbfsConfig->rootNode->fileType = FNODE_DIR;
	
	}
	
	
	insertionNode = mrbfsTraversePath(insertionPath, gMrbfsConfig->rootNode, &parentNode);
	if (NULL == insertionNode || insertionNode->fileType != FNODE_DIR)
	{
		mrbfsLogMessage(MRBFS_LOG_DEBUG, "Cannot insert node [%s] into [%s]", insertionNode->fileName, insertionPath);
		return(NULL);
	}

	node = insertionNode->childPtr;
	prevnode = NULL;

	while (node != NULL)
	{
		if (0 == strcmp(node->fileName, addNode->fileName))
		{
			mrbfsLogMessage(MRBFS_LOG_ERROR, "Node of name [%s] already exists", addNode->fileName);
			return(NULL);
		} 
		else if (0 < strcmp(addNode->fileName, node->fileName))
		{
			// Insert here on the front end - need to change parent child ptr
			if (NULL == prevnode)
				insertionNode->childPtr = addNode;
			else
				prevnode->childPtr = addNode;
				
			addNode->childPtr = NULL;
			addNode->siblingPtr = node;
			break;
		}
		else
		prevnode = node;
		node = node->siblingPtr;
	}

	if (node == NULL)
	{
			if (NULL == prevnode)
				insertionNode->childPtr = addNode;
			else
				prevnode->childPtr = addNode;
				
			addNode->childPtr = NULL;
			addNode->siblingPtr = node;
	}
	return(addNode);
}

int mrbfsGetattr(const char *path, struct stat *stbuf)
{
	int retval = -ENOENT;
	MRBFSFileNode *parentNode, *fileNode = mrbfsTraversePath(path, gMrbfsConfig->rootNode, &parentNode);

	mrbfsLogMessage(MRBFS_LOG_DEBUG, "mrbfsGetattr(%s), fileNode=%p", path, fileNode);
	
	if (NULL == fileNode)
		return(retval);
	
	switch(fileNode->fileType)
	{
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

		case FNODE_WRITABLE_VALUE_STR:
			stbuf->st_mode = S_IFREG | 0664;
			stbuf->st_nlink = 1;
			stbuf->st_size = strlen(fileNode->value.valueStr);
			retval = 0;			
			break;
			
		case FNODE_WRITABLE_VALUE_INT:
			stbuf->st_mode = S_IFREG | 0664;
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
	
	mrbfsLogMessage(MRBFS_LOG_DEBUG, "mrbfsReaddir(%s), fileNode=%p", path, fileNode);
	
	if (NULL == fileNode || fileNode->fileType != FNODE_DIR)
		return -ENOENT;

	if (fileNode->childPtr != NULL)
		fileNode = fileNode->childPtr;

	while(fileNode->siblingPtr != NULL)
	{
		filler(buf, fileNode->fileName, NULL, 0);
		fileNode = fileNode->siblingPtr;
	}	
	return(0);
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
	MRBFSFileNode *parentNode, *fileNode = mrbfsTraversePath(path, gMrbfsConfig->rootNode, &parentNode);



	return size;
}
