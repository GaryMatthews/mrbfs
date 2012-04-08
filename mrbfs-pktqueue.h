#ifndef _MRBFS_PKT_QUEUE_H
#define _MRBFS_PKT_QUEUE_H

void mrbusPacketQueueInitialize(MRBusPacketQueue* q);
int mrbusPacketQueueDepth(MRBusPacketQueue* q);
void mrbusPacketQueuePush(MRBusPacketQueue* q, MRBusPacket* txPkt, UINT8 srcAddress);
MRBusPacket* mrbusPacketQueuePop(MRBusPacketQueue* q, MRBusPacket* pkt);

#endif
