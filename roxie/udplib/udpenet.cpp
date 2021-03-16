/*##############################################################################

    HPCC SYSTEMS software Copyright (C) 2019 HPCC Systems®.

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
############################################################################## */

#include <map>
#include "jexcept.hpp"
#include "jqueue.tpp"
#include "udplib.hpp"
#include "udpipmap.hpp"
#include "udpmsgpk.hpp"
#include "udpsha.hpp"
#include "udptrs.hpp"
#include "roxie.hpp"
#ifdef _USE_ENET
#include <enet/enet.h>

extern UDPLIB_API void setEnetProperties(const IPropertyTree *config)
{
}

extern UDPLIB_API int EnetInit()
{
    if (enet_initialize() != 0)
    {
        return 1;
    }
    return 0;
}

extern UDPLIB_API void EnetDeinit()
{
    enet_deinitialize();
}

class CRoxieEnetReceiveManager : public CInterfaceOf<IReceiveManager>
{
private:
    typedef std::map<ruid_t, CMessageCollator*> uid_map;
    uid_map         collators;
    SpinLock collatorsLock; // protects access to collators map

public:
    CRoxieEnetReceiveManager(const SocketEndpoint &myEndpoint)
    {
        // collatePacket(buffer.buffer() + offset, length);
    }
    ~CRoxieEnetReceiveManager()
    {
    }

    void collatePacket( std::uint8_t *buffer, unsigned length)
    {
        const UdpPacketHeader *pktHdr = (UdpPacketHeader*) buffer;
        assert(pktHdr->length == length);

        if (udpTraceLevel >= 4)
        {
            StringBuffer s;
            DBGLOG("EnetReceiver: CPacketCollator - unQed packet - ruid=" RUIDF " id=0x%.8X mseq=%u pkseq=0x%.8X len=%d node=%s",
                pktHdr->ruid, pktHdr->msgId, pktHdr->msgSeq, pktHdr->pktSeq, pktHdr->length, pktHdr->node.getTraceText(s).str());
        }

        Linked <CMessageCollator> msgColl;
        bool isDefault = false; // Don't trace inside the spinBlock!
        {
            SpinBlock b(collatorsLock);
            try
            {
                msgColl.set(collators[pktHdr->ruid]);
                if (!msgColl)
                {
                    msgColl.set(collators[RUID_DISCARD]);
                    // We could consider sending an abort to the agent, but it should have already been done by ccdserver code
                    isDefault = true;
                    unwantedDiscarded++;
                }
            }
            catch (IException *E)
            {
                EXCLOG(E);
                E->Release();
            }
            catch (...)
            {
                IException *E = MakeStringException(ROXIE_INTERNAL_ERROR, "Unexpected exception caught in CPacketCollator::run");
                EXCLOG(E);
                E->Release();
            }
        }
        if (udpTraceLevel && isDefault)
        {
            StringBuffer s;
            DBGLOG("EnetReceiver: CPacketCollator NO msg collator found - using default - ruid=" RUIDF " id=0x%.8X mseq=%u pkseq=0x%.8X node=%s",
                pktHdr->ruid, pktHdr->msgId, pktHdr->msgSeq, pktHdr->pktSeq, pktHdr->node.getTraceText(s).str());
        }
        if (msgColl)
            msgColl->attach_data(buffer, length);
    }

    // Note - some of this code could be in a common base class with udpreceivemanager, but hope to kill that at some point
    virtual IMessageCollator *createMessageCollator(roxiemem::IRowManager *rowManager, ruid_t ruid) override
    {
        CMessageCollator *msgColl = new CMessageCollator(rowManager, ruid);
        if (udpTraceLevel >= 2)
            DBGLOG("EnetReceiver: createMessageCollator %p %u", msgColl, ruid);
        {
            SpinBlock b(collatorsLock);
            collators[ruid] = msgColl;
        }
        msgColl->Link();
        return msgColl;
    }

    virtual void detachCollator(const IMessageCollator *msgColl) override
    {
        ruid_t ruid = msgColl->queryRUID();
        if (udpTraceLevel >= 2)
            DBGLOG("EnetReceiver: detach %p %u", msgColl, ruid);
        {
            SpinBlock b(collatorsLock);
            collators.erase(ruid);
        }
        msgColl->Release();
    }

private:

};

class UdpEnetReceiverEntry : public IUdpReceiverEntry
{
private:
    unsigned numQueues;
    const IpAddress dest;

public:
    UdpEnetReceiverEntry(const IpAddress &_ip, unsigned _dataPort, unsigned _numQueues) : dest(_ip), numQueues(_numQueues)
    {
        StringBuffer channel("enet:udp?endpoint=");
        dest.getIpText(channel);
        channel.append(':').append(_dataPort);
        for (unsigned queue = 0; queue < numQueues; queue++)
        {
            if (udpTraceLevel)
                DBGLOG("EnetSender: Creating enet channel %s for queue %d", channel.str(), queue);
        }
    }

    void write(roxiemem::DataBuffer *buffer, unsigned len, unsigned queue)
    {
        printf("write(%u bytes here)\n", len);
    }
};

class CRoxieEnetSendManager : public CInterfaceOf<ISendManager>
{
    const unsigned dataPort = 0;
    const unsigned numQueues = 0;
    IpMapOf<UdpEnetReceiverEntry> receiversTable;
    const IpAddress myIP;

    std::atomic<unsigned> msgSeq{0};

    inline unsigned getNextMessageSequence()
    {
        unsigned res;
        do
        {
            res = ++msgSeq;
        } while (unlikely(!res));
        return res;
    }
public:
    CRoxieEnetSendManager(unsigned _dataPort, unsigned _numQueues, const IpAddress &_myIP) : dataPort(_dataPort), numQueues(_numQueues),
      receiversTable([this](const ServerIdentifier &ip) { return new UdpEnetReceiverEntry(ip.getIpAddress(), dataPort, numQueues);}),
      myIP(_myIP)
    {
    }
    virtual void writeOwn(IUdpReceiverEntry &receiver, roxiemem::DataBuffer *buffer, unsigned len, unsigned queue) override
    {
        assert(queue < numQueues);
        static_cast<UdpEnetReceiverEntry &>(receiver).write(buffer, len, queue);
        buffer->Release();
    }
    virtual IMessagePacker *createMessagePacker(ruid_t id, unsigned sequence, const void *messageHeader, unsigned headerSize, const ServerIdentifier &destNode, int queue) override;
    virtual bool dataQueued(ruid_t ruid, unsigned sequence, const ServerIdentifier &destNode) override { return false; }
    virtual bool abortData(ruid_t ruid, unsigned sequence, const ServerIdentifier &destNode) override { return false; }
    virtual bool allDone() override { return true; }
};

IMessagePacker *CRoxieEnetSendManager::createMessagePacker(ruid_t ruid, unsigned sequence, const void *messageHeader, unsigned headerSize, const ServerIdentifier &destNode, int queue)
{
    const IpAddress dest = destNode.getIpAddress();
    return ::createMessagePacker(ruid, sequence, messageHeader, headerSize, *this, receiversTable[dest], myIP, getNextMessageSequence(), queue);
}

extern UDPLIB_API IReceiveManager *createEnetReceiveManager(const SocketEndpoint &ep)
{
    return new CRoxieEnetReceiveManager(ep);
}

extern UDPLIB_API ISendManager *createEnetSendManager(unsigned dataPort, unsigned numQueues, const IpAddress &myIP)
{
    return new CRoxieEnetSendManager(dataPort, numQueues, myIP);
}

#else

extern UDPLIB_API void setEnetProperties(const IPropertyTree *config)
{
}

extern UDPLIB_API IReceiveManager *createEnetReceiveManager(const SocketEndpoint &ep)
{
    UNIMPLEMENTED;
}

extern UDPLIB_API ISendManager *createEnetSendManager(unsigned dataPort, unsigned numQueues, const IpAddress &myIP)
{
    UNIMPLEMENTED;
}

extern UDPLIB_API int EnetInit()
{
    UNIMPLEMENTED;
}

extern UDPLIB_API void EnetDeinit()
{
}
#endif
