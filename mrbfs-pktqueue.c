#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <malloc.h>
#include <pthread.h>
#include <unistd.h>
#include "mrbfs-module.h"

void mrbusPacketQueueInitialize(MRBusPacketQueue* q)
{
	pthread_mutexattr_t lockAttr;
	memset(q, 0, sizeof(MRBusPacketQueue));
	// Initialize the queue lock
	pthread_mutexattr_init(&lockAttr);
	pthread_mutexattr_settype(&lockAttr, PTHREAD_MUTEX_ADAPTIVE_NP);
	pthread_mutex_init(&q->queueLock, &lockAttr);
	pthread_mutexattr_destroy(&lockAttr);		

}

int mrbusPacketQueueDepth(MRBusPacketQueue* q)
{
	int depth = 0;
	
	pthread_mutex_lock(&q->queueLock);
	depth = (q->headIdx - q->tailIdx) % MRBUS_PACKET_QUEUE_SIZE; 
	pthread_mutex_unlock(&q->queueLock);

	return(depth);
}

void mrbusPacketQueuePush(MRBusPacketQueue* q, MRBusPacket* txPkt, UINT8 srcAddress)
{
	pthread_mutex_lock(&q->queueLock);

	memcpy(&q->pkts[q->headIdx], txPkt, sizeof(MRBusPacket));
	if (0 != srcAddress && 0 == txPkt->pkt[MRBUS_PKT_SRC])
		q->pkts[q->headIdx].pkt[MRBUS_PKT_SRC] = srcAddress;

	if( ++q->headIdx >= MRBUS_PACKET_QUEUE_SIZE )
		q->headIdx = 0;

	pthread_mutex_unlock(&q->queueLock);
}

MRBusPacket* mrbusPacketQueuePop(MRBusPacketQueue* q, MRBusPacket* pkt)
{
	memcpy(pkt, &q->pkts[q->tailIdx], sizeof(MRBusPacket));
	if( ++q->tailIdx >= MRBUS_PACKET_QUEUE_SIZE )
		q->tailIdx = 0;

	return(pkt);
}

