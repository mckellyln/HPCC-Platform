/*##############################################################################

    HPCC SYSTEMS software Copyright (C) 2021 HPCC Systems®.

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

extern UDPLIB_API void udpEnetInit(int state)
{
    if (state)
    {
        int srtn = enet_initialize();
        if (srtn)
            throw MakeStringException(ROXIE_INTERNAL_ERROR, "An error occurred while initializing Enet.");
    }
    else
        enet_deinitialize();
}

class CRoxieEnetReceiveManager : public CInterfaceOf<IReceiveManager>
{
private:
    typedef std::map<ruid_t, CMessageCollator*> uid_map;
    uid_map         collators;
    SpinLock collatorsLock; // protects access to collators map
    bool encrypted;

public:
    CRoxieEnetReceiveManager(const SocketEndpoint &myEndpoint, bool _encrypted)
    : encrypted(_encrypted)
    {
        ENetAddress address;
        ENetHost * server;

        address.host = ENET_HOST_ANY;
        address.port = 1234;

        server = enet_host_create (&address /* the address to bind the server host to */,
                                   1000     /* allow up to 32 clients and/or outgoing connections */,
                                   2        /* allow up to 2 channels to be used, 0 and 1 */,
                                   0        /* assume any amount of incoming bandwidth */,
                                   0        /* assume any amount of outgoing bandwidth */);
        if (server == NULL)
            throw MakeStringException(ROXIE_INTERNAL_ERROR, "An error occurred while trying to create an Enet server.");

        ENetEvent event;

        /* Wait up to 1000 milliseconds for an event. */
        while (enet_host_service (server, & event, 1000) > 0)
        {
            switch (event.type)
            {
            case ENET_EVENT_TYPE_CONNECT:
                printf ("A new client connected from %x:%u.\n",
                        event.peer -> address.host,
                        event.peer -> address.port);

                /* Store any relevant client information here. */
                event.peer -> data = (void *)"Client information";

                break;

            case ENET_EVENT_TYPE_RECEIVE:
                printf ("A packet of length %lu containing %s was received from %s on channel %u.\n",
                        event.packet -> dataLength,
                        event.packet -> data,
                        (char *)event.peer -> data,
                        event.channelID);

                /* Clean up the packet now that we're done using it. */
                enet_packet_destroy (event.packet);

                break;

            case ENET_EVENT_TYPE_DISCONNECT:
                printf ("%s disconnected.\n", (char *)event.peer -> data);

                /* Reset the peer's client information. */

                event.peer -> data = NULL;

            case ENET_EVENT_TYPE_NONE:

                break;
            }
        }

    }

    ~CRoxieEnetReceiveManager()
    {
        // enet_host_destroy(server);
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
        CMessageCollator *msgColl = new CMessageCollator(rowManager, ruid, encrypted);
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

};

class UdpEnetReceiverEntry : public IUdpReceiverEntry
{
private:
    unsigned numQueues;
    const IpAddress dest;

public:
    UdpEnetReceiverEntry(const IpAddress &_ip, unsigned _dataPort, unsigned _numQueues)
    : dest(_ip), numQueues(_numQueues)
    {
    }

    void write(roxiemem::DataBuffer *buffer, unsigned len, unsigned queue)
    {
    }
};

class CRoxieEnetSendManager : public CInterfaceOf<ISendManager>
{
    const unsigned dataPort = 0;
    const unsigned numQueues = 0;
    IpMapOf<UdpEnetReceiverEntry> receiversTable;
    const IpAddress myIP;
    bool encrypted;

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
    CRoxieEnetSendManager(unsigned _dataPort, unsigned _numQueues, const IpAddress &_myIP, bool _encrypted)
    : dataPort(_dataPort),
      numQueues(_numQueues),
      receiversTable([this](const ServerIdentifier ip) { return new UdpEnetReceiverEntry(ip.getIpAddress(), dataPort, numQueues);}),
      myIP(_myIP),
      encrypted(_encrypted)
    {
        ENetHost* client = { 0 };

        client = enet_host_create (NULL /* create a client host */,
                                   1 /* only allow 1 outgoing connection */,
                                   2 /* allow up 2 channels to be used, 0 and 1 */,
                                   0 /* assume any amount of incoming bandwidth */,
                                   0 /* assume any amount of outgoing bandwidth */);

        if (client == NULL)
            throw MakeStringException(ROXIE_INTERNAL_ERROR, "An error occurred while trying to create an Enet client.");

        ENetAddress address = { 0 };
        ENetEvent event;
        ENetPeer* peer = { 0 };

        /* Connect to some.server.net:1234. */
        enet_address_set_host(&address, "127.0.0.1");
        address.port = 7777;

        /* Initiate the connection, allocating the two channels 0 and 1. */
        peer = enet_host_connect(client, &address, 2, 0);
        if (peer == NULL) {
          fprintf(stderr,
            "No available peers for initiating an ENet connection.\n");
          exit(EXIT_FAILURE);
        }

        /* Wait up to 5 seconds for the connection attempt to succeed. */
        if (enet_host_service(client, &event, 5000) > 0 &&
          event.type == ENET_EVENT_TYPE_CONNECT) {
          puts("Connection to some.server.net:1234 succeeded.");
        } else {
          /* Either the 5 seconds are up or a disconnect event was */
          /* received. Reset the peer in the event the 5 seconds   */
          /* had run out without any significant event.            */
          enet_peer_reset(peer);
          puts("Connection to some.server.net:1234 failed.");
        }

        // Receive some events
        enet_host_service(client, &event, 5000);

        // Disconnect
        enet_peer_disconnect(peer, 0);

        uint8_t disconnected = false;
        /* Allow up to 3 seconds for the disconnect to succeed
         * and drop any packets received packets.
         */
        while (enet_host_service(client, &event, 3000) > 0) {
            switch (event.type) {
            case ENET_EVENT_TYPE_RECEIVE:
                enet_packet_destroy(event.packet);
                break;
            case ENET_EVENT_TYPE_DISCONNECT:
                puts("Disconnection succeeded.");
                disconnected = true;
                break;
            }
        }

        // Drop connection, since disconnection didn't successed
        if (!disconnected) {
            enet_peer_reset(peer);
        }

    }

    ~CRoxieEnetSendManager()
    {
        // enet_host_destroy(client);
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
    virtual void abortAll(const ServerIdentifier &destNode) override { }
    virtual bool allDone() override { return true; }
};

IMessagePacker *CRoxieEnetSendManager::createMessagePacker(ruid_t ruid, unsigned sequence, const void *messageHeader, unsigned headerSize, const ServerIdentifier &destNode, int queue)
{
    const IpAddress dest = destNode.getIpAddress();
    return ::createMessagePacker(ruid, sequence, messageHeader, headerSize, *this, receiversTable[dest], myIP, getNextMessageSequence(), queue, encrypted);
}

extern UDPLIB_API void setEnetProperties(const IPropertyTree *config)
{
    // MCK TODO ?
}

extern UDPLIB_API IReceiveManager *createEnetReceiveManager(const SocketEndpoint &ep, bool encrypted)
{
    return new CRoxieEnetReceiveManager(ep, encrypted);
}

extern UDPLIB_API ISendManager *createEnetSendManager(unsigned dataPort, unsigned numQueues, const IpAddress &myIP, bool encrypted)
{
    return new CRoxieEnetSendManager(dataPort, numQueues, myIP, encrypted);
}

#else // _USE_ENET

extern UDPLIB_API void setEnetProperties(const IPropertyTree *config)
{
}

extern UDPLIB_API IReceiveManager *createEnetReceiveManager(const SocketEndpoint &ep, bool encrypted)
{
    UNIMPLEMENTED;
}

extern UDPLIB_API ISendManager *createEnetSendManager(unsigned dataPort, unsigned numQueues, const IpAddress &myIP, bool encrypted)
{
    UNIMPLEMENTED;
}
#endif // _USE_ENET
