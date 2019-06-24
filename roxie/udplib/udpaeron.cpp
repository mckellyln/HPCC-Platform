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
#include <Aeron.h>
extern "C" {
#include "aeronmd.h"
#include "concurrent/aeron_atomic.h"
#include "aeron_driver_context.h"
#include "util/aeron_properties_util.h"
}

struct AeronFragmentHeader  // Info needed for the demux
{
    ruid_t ruid = 0;
    unsigned sequence = 0;
    unsigned datasize = 0;  // Possibly could be unsigned short
    unsigned metasize = 0;  // Possibly could be unsigned short
};

static std::thread aeronDriverThread;
static Semaphore driverStarted;

volatile bool aeronDriverRunning = true;

void stopAeronDriver()
{
    AERON_PUT_ORDERED(aeronDriverRunning, false);
}

void sigint_handler(int signal)
{
    stopAeronDriver();
}

void termination_hook(void *state)
{
    stopAeronDriver();
}

inline bool is_running()
{
    bool result;
    AERON_GET_VOLATILE(result, aeronDriverRunning);
    return result;
}

int startAeronDriver()
{
    aeron_driver_context_t *context = nullptr;
    aeron_driver_t *driver = nullptr;
    try
    {
        if (aeron_driver_context_init(&context) < 0)
            throw makeStringExceptionV(MSGAUD_operator, -1, "AERON: error initializing context (%d) %s", aeron_errcode(), aeron_errmsg());

        context->termination_hook_func = termination_hook;
        context->dirs_delete_on_start = true;
        context->warn_if_dirs_exist = false;
        context->term_buffer_sparse_file = false;
        context->mtu_length=16384;
        context->socket_rcvbuf=2097152;
        context->socket_sndbuf=2097152;
        context->initial_window_length=2097152;

        if (aeron_driver_init(&driver, context) < 0)
            throw makeStringExceptionV(MSGAUD_operator, -1, "AERON: error initializing driver (%d) %s", aeron_errcode(), aeron_errmsg());

        if (aeron_driver_start(driver, true) < 0)
            throw makeStringExceptionV(MSGAUD_operator, -1, "AERON: error starting driver (%d) %s", aeron_errcode(), aeron_errmsg());

        driverStarted.signal();
        while (is_running())
        {
            aeron_driver_main_idle_strategy(driver, aeron_driver_main_do_work(driver));
        }
        aeron_driver_close(driver);
        aeron_driver_context_close(context);
    }
    catch (...)
    {
        aeron_driver_close(driver);
        aeron_driver_context_close(context);
        throw;
    }
    return 0;
}

class CRoxieAeronMessageCollator;

class CRoxieAeronReceiveManager : public CInterfaceOf<IReceiveManager>
{
private:
    MapXToMyClass<ruid_t, ruid_t, CRoxieAeronMessageCollator> collators;
    CriticalSection crit;
    std::shared_ptr<aeron::Aeron> aeron;
    std::shared_ptr<aeron::Subscription> loSub;
    std::shared_ptr<aeron::Subscription> hiSub;
    std::shared_ptr<aeron::Subscription> slaSub;
    std::thread receiveThread;
    bool running = true;
    static const int fragmentsLimit = 10;  // MORE - configurable?
    const std::chrono::duration<long, std::milli> idleSleepMs;   // MORE - configurable?
public:
    CRoxieAeronReceiveManager(const SocketEndpoint &myEndpoint, bool useEmbeddedMediaDriver)
    : idleSleepMs(1)
    {
        if (useEmbeddedMediaDriver)  // MORE - need on sender too if we are separating farmers from slaves
        {
            aeronDriverThread = std::thread([]() { startAeronDriver(); });
            driverStarted.wait();
        }
        aeron::Context context;

        context.newSubscriptionHandler(
            [](const std::string& channel, std::int32_t streamId, std::int64_t correlationId)
            {
                DBGLOG("AERON: Subscription: %s %" I64F "d %d", channel.c_str(), correlationId, streamId);
            });
        context.availableImageHandler([](aeron::Image &image)
            {
                DBGLOG("AERON: Available image correlationId=%" I64F "d, sessionId=%d at position %" I64F "d from %s", image.correlationId(), image.sessionId(), image.position(), image.sourceIdentity().c_str());
            });
        context.unavailableImageHandler([](aeron::Image &image)
            {
               DBGLOG("AERON: Unavailable image correlationId=%" I64F "d, sessionId=%d at position %" I64F "d from %s", image.correlationId(), image.sessionId(), image.position(), image.sourceIdentity().c_str());
            });

        aeron = aeron::Aeron::connect(context);
        loSub = addSubscription(myEndpoint, 0);
        hiSub = addSubscription(myEndpoint, 1);
        slaSub = addSubscription(myEndpoint, 2);
        aeron::fragment_handler_t handler = [this](const aeron::AtomicBuffer& buffer, aeron::util::index_t offset, aeron::util::index_t length, const aeron::Header& header)
        {
            // MORE!
            DBGLOG("Message to stream %d from session %d", header.streamId(), header.sessionId());
            collatePacket(buffer.buffer() + offset, length);
        };

        receiveThread = std::thread([this, handler]()
        {
            while (running)
            {
                int fragmentsRead = slaSub->poll(handler, fragmentsLimit);
                if (!fragmentsRead)
                    fragmentsRead = hiSub->poll(handler, fragmentsLimit);
                if (!fragmentsRead)
                    fragmentsRead = loSub->poll(handler, fragmentsLimit);
                if (!fragmentsRead)
                    std::this_thread::sleep_for(idleSleepMs);
            }
        });
    }
    ~CRoxieAeronReceiveManager()
    {
        running = false;
        receiveThread.join();
    }
    virtual IMessageCollator *createMessageCollator(roxiemem::IRowManager *rowManager, ruid_t ruid) override;
    virtual void detachCollator(const IMessageCollator *collator) override;

private:
    void collatePacket( std::uint8_t *buffer, aeron::util::index_t length);

    std::shared_ptr<aeron::Subscription> addSubscription(const SocketEndpoint &myEndpoint, int queue)
    {
        StringBuffer channel("aeron:udp?endpoint=");
        myEndpoint.getIpText(channel);
        channel.append(':').append(myEndpoint.port);
        std::int64_t id = aeron->addSubscription(channel.str(), queue);
        std::shared_ptr<aeron::Subscription> subscription = aeron->findSubscription(id);
        while (!subscription)
        {
            std::this_thread::yield();
            subscription = aeron->findSubscription(id);
        }
        return subscription;
    }
};

class CRoxieAeronSendManager : public CInterfaceOf<ISendManager>
{
    std::shared_ptr<aeron::Aeron> aeron;
    mutable CriticalSection pubTableLock;
    mutable std::map<IpAddress, std::shared_ptr<aeron::Publication>, IpComparator> pubTable;
    unsigned dataPort;
public:
    CRoxieAeronSendManager(unsigned _dataPort) : dataPort(_dataPort)
    {
        aeron::Context context;
        context.newPublicationHandler(
            [](const std::string& channel, std::int32_t streamId, std::int32_t sessionId, std::int64_t correlationId)
            {
                DBGLOG("AERON: Publication %s, correlation %" I64F "d, streamId %d, sessionId %d", channel.c_str(),  correlationId, streamId, sessionId);
            });

        aeron = aeron::Aeron::connect(context);
    }
    virtual IMessagePacker *createMessagePacker(ruid_t id, unsigned sequence, const void *messageHeader, unsigned headerSize, unsigned destNodeIndex, int queue) override;
    virtual bool dataQueued(ruid_t ruid, unsigned sequence, unsigned destNodeIndex) override { return false; }
    virtual bool abortData(ruid_t ruid, unsigned sequence, unsigned destNodeIndex) override { return false; }
    virtual bool allDone() override { return true; }
};

typedef std::array<std::uint8_t, 1024> buffer_t;  // MORE - this is just from the publish example - NOT tuned for any kind of efficiency!

class CRoxieAeronMessagePacker : public CInterfaceOf<IMessagePacker>
{
    aeron::Publication *publication;
    unsigned destNodeIndex;
    AeronFragmentHeader package_header;

    MemoryBuffer    metaInfo;
    int             queue;

    MemoryBuffer buffer;
    unsigned lastput = 0;

public:
    CRoxieAeronMessagePacker(aeron::Publication *_publication, ruid_t ruid, unsigned sequence, const void *messageHeader, unsigned headerSize, unsigned _destNodeIndex, int _queue)
    : publication(_publication), destNodeIndex(_destNodeIndex), queue(_queue)
    {
        package_header.ruid = ruid;
        package_header.sequence = sequence;
        // buffer.append(sizeof(package_header), &package_header);  Probably will want to when we recode to go directly to fragment buffers
        buffer.append((unsigned short) headerSize);
        buffer.append(headerSize, messageHeader);
        lastput = headerSize+sizeof(unsigned short);
    }

    virtual void *getBuffer(unsigned len, bool variable) override
    {
        // MORE - work out how to use Aeron buffers properly (and potentially arrays of them) to avoid copies.
        // MORE - start sending out results as we fill each buffer
        if (variable)
        {
            char *ret = (char *) buffer.ensureCapacity(len + sizeof(RecordLengthType));
            return ret + sizeof(RecordLengthType);
        }
        else
        {
            return buffer.ensureCapacity(len);
        }
    }

    virtual void putBuffer(const void *buf, unsigned len, bool variable) override
    {
        if (variable)
        {
            buf = ((char *) buf) - sizeof(RecordLengthType);
            *(RecordLengthType *) buf = len;
            len += sizeof(RecordLengthType);
        }
        buffer.setWritePos(lastput + len);
        lastput += len;
    }

    virtual void sendMetaInfo(const void *buf, unsigned len) override
    {
        metaInfo.append(len, buf);
    }

    virtual void flush() override
    {
        // We need to send a message consisting of:
        // Fragment header - so collator knows who to send result on to (in every fragment)
        // message header - The original roxiepacket, only in first fragment
        // data
        // Meta info - continuation info - in last fragment (potentially spread over more than one fragment)
        // MORE - we need to send fragments as they are ready not build up whole response before sending.
        // MORE - this is not supporting meta yet
        AERON_DECL_ALIGNED(buffer_t aebuffer, 16);
        aeron::concurrent::AtomicBuffer srcBuffer(&aebuffer[0], aebuffer.size());
        unsigned bytesSent = 0;
        while (bytesSent < lastput)
        {
            srcBuffer.putBytes(0, reinterpret_cast<std::uint8_t *>(&package_header), sizeof(package_header));
            unsigned toSend = lastput - bytesSent;
            if (toSend > 1024-sizeof(package_header))
                toSend = 1024-sizeof(package_header);
            srcBuffer.putBytes(sizeof(package_header), buffer.readDirect(toSend), toSend);
            const std::int64_t result = publication->offer(srcBuffer, 0, toSend+sizeof(package_header));
            if (result < 0)
            {
                if (aeron::BACK_PRESSURED == result)
                {
                    std::cout << "Offer failed due to back pressure" << std::endl;
                    sleep(1);
                    continue;  // MORE - is this really right
                }
                else if (aeron::NOT_CONNECTED == result)
                {
                    std::cout << "Offer failed because publisher is not connected to subscriber" << std::endl;
                }
                else if (aeron::ADMIN_ACTION == result)
                {
                    std::cout << "Offer failed because of an administration action in the system" << std::endl;
                }
                else if (aeron::PUBLICATION_CLOSED == result)
                {
                    std::cout << "Offer failed publication is closed" << std::endl;
                }
                else
                {
                    std::cout << "Offer failed due to unknown reason" << result << std::endl;
                }
                UNIMPLEMENTED;  // Probably fail here
            }
            bytesSent += toSend;
        }
    }

    virtual bool dataQueued() override
    {
        return false;
    }

    virtual unsigned size() const override
    {
        return lastput;
    }
};

// NOTE - copied some code from local collator for now, but expect to replace with something that does not copy data so much
// once POC is working and I understand Aeron better. If can't then probably should common up.

class CAeronMessageUnpackCursor : public CInterfaceOf<IMessageUnpackCursor>
{
    void *data;
    unsigned datalen;
    unsigned pos;
    Linked<roxiemem::IRowManager> rowManager;
public:
    CAeronMessageUnpackCursor(roxiemem::IRowManager *_rowManager, void *_data, unsigned _datalen)
        : rowManager(_rowManager)
    {
        datalen = _datalen;
        data = _data;
        pos = 0;
    }

    ~CAeronMessageUnpackCursor()
    {
    }

    virtual bool atEOF() const
    {
        return datalen==pos;
    }

    virtual bool isSerialized() const
    {
        // NOTE: tempting to think that we could avoid serializing in localSlave case, but have to be careful about the lifespan of the rowManager...
        return true;
    }

    virtual const void * getNext(int length)
    {
        if (pos==datalen)
            return NULL;
        assertex(pos + length <= datalen);
        void * cur = ((char *) data) + pos;
        pos += length;
        void * ret = rowManager->allocate(length, 0);
        memcpy(ret, cur, length);
        //No need for finalize since only contains plain data.
        return ret;
    }
};

class CAeronMessageResult : implements IMessageResult, public CInterface
{
    void *data;
    void *meta;
    void *header;
    unsigned datalen, metalen, headerlen;
    unsigned pos;
public:
    IMPLEMENT_IINTERFACE;
    CAeronMessageResult(void *_data, unsigned _datalen, void *_meta, unsigned _metalen, void *_header, unsigned _headerlen)
    {
        datalen = _datalen;
        metalen = _metalen;
        headerlen = _headerlen;
        data = _data;
        meta = _meta;
        header = _header;
        pos = 0;
    }

    ~CAeronMessageResult()
    {
        free(data);
        free(meta);
        free(header);
    }

    virtual IMessageUnpackCursor *getCursor(roxiemem::IRowManager *rowMgr) const
    {
        return new CAeronMessageUnpackCursor(rowMgr, data, datalen);
    }

    virtual const void *getMessageHeader(unsigned &length) const
    {
        length = headerlen;
        return header;
    }

    virtual const void *getMessageMetadata(unsigned &length) const
    {
        length = metalen;
        return meta;
    }

    virtual void discard() const
    {
    }

};

class CRoxieAeronMessageCollator : public CInterfaceOf<IMessageCollator>
{
private:
    InterruptableSemaphore sem;
    ruid_t ruid = 0;
    unsigned totalBytesReceived = 0;
    QueueOf<IMessageResult, false> pending;
    CriticalSection crit;
public:
    CRoxieAeronMessageCollator(ruid_t _ruid) : ruid(_ruid) {}
    virtual IMessageResult *getNextResult(unsigned time_out, bool &anyActivity) override;
    virtual void interrupt(IException *E = NULL) override;
    virtual ruid_t queryRUID() const override { return ruid; }
    virtual unsigned queryBytesReceived() const override { return totalBytesReceived; }

    void enqueueMessage(bool outOfBand, void *data, unsigned datalen, void *meta, unsigned metalen, void *header, unsigned headerlen);
};

IMessageResult *CRoxieAeronMessageCollator::getNextResult(unsigned time_out, bool &anyActivity)
{
    anyActivity = false;
    if (!sem.wait(time_out))
        return NULL;
    anyActivity = true;
    CriticalBlock c(crit);
    return pending.dequeue();
}

void CRoxieAeronMessageCollator::interrupt(IException *E)
{
    sem.interrupt(E);
}

void CRoxieAeronMessageCollator::enqueueMessage(bool outOfBand, void *data, unsigned datalen, void *meta, unsigned metalen, void *header, unsigned headerlen)
{
    // NOTE - as coded this takes ownership of incoming data (which may not be correct!)
    CriticalBlock c(crit);
    if (outOfBand)
        pending.enqueueHead(new CAeronMessageResult(data, datalen, meta, metalen, header, headerlen));
    else
        pending.enqueue(new CAeronMessageResult(data, datalen, meta, metalen, header, headerlen));
    sem.signal();
    totalBytesReceived += datalen + metalen + headerlen;
}


IMessageCollator *CRoxieAeronReceiveManager::createMessageCollator(roxiemem::IRowManager *rowManager, ruid_t ruid)
{
    CRoxieAeronMessageCollator *collator = new CRoxieAeronMessageCollator(ruid);
    CriticalBlock b(crit);
    collators.setValue(ruid, collator);
    return collator;
}

void CRoxieAeronReceiveManager::collatePacket( std::uint8_t *buffer, aeron::util::index_t length)
{
    const AeronFragmentHeader *fragHeader = reinterpret_cast<const AeronFragmentHeader *>(buffer);
    buffer += sizeof(AeronFragmentHeader);
    length -= sizeof(AeronFragmentHeader);
    std::uint16_t packHeaderSize = *(std::uint16_t *) buffer;
    buffer += sizeof(std::uint16_t);
    length -= sizeof(std::uint16_t);
    const void *packHeader = buffer;
    buffer += packHeaderSize;
    length -= packHeaderSize;
    DBGLOG("ruid = %u", fragHeader->ruid);
    CriticalBlock b(crit);
    CRoxieAeronMessageCollator *collator = collators.getValue(fragHeader->ruid);
    if (!collator)
        collator = collators.getValue(RUID_DISCARD);
    // MORE - for now it's just one message, but this will change once fragmented!

    if (collator)
    {
        // MORE - out-of-band? meta? header?
        collator->enqueueMessage(false,
                                 memcpy(malloc(length), buffer, length), length,
                                 nullptr, 0,
                                 memcpy(malloc(packHeaderSize), packHeader, packHeaderSize), packHeaderSize);
    }
}

void CRoxieAeronReceiveManager::detachCollator(const IMessageCollator *collator)
{
    ruid_t id = collator->queryRUID();
    CriticalBlock b(crit);
    collators.setValue(id, NULL);
}

IMessagePacker *CRoxieAeronSendManager::createMessagePacker(ruid_t id, unsigned sequence, const void *messageHeader, unsigned headerSize, unsigned destNodeIndex, int queue)
{
    const IpAddress &dest = getNodeAddress(destNodeIndex);
    std::shared_ptr<aeron::Publication> publication;
    bool isNew = false;
    {
        CriticalBlock b(pubTableLock);
        if (unlikely(pubTable.find(dest) == pubTable.end()))
        {
            StringBuffer channel("aeron:udp?endpoint=");
            dest.getIpText(channel);
            channel.append(':').append(dataPort);
            std::int64_t id = aeron->addPublication(channel.str(), queue);
            publication = aeron->findPublication(id);
            // wait for the publication to be valid
            while (!publication)
            {
                std::this_thread::yield();
                publication = aeron->findPublication(id);
            }
            pubTable[dest] = publication;
            isNew = true;
        }
        else
            publication = pubTable[dest];
    }
    if (isNew)
    {
        // Wait for up to 5 seconds to connect to a subscriber
        unsigned start = msTick();
        while (!publication->isConnected())
        {
            Sleep(10);
            if (msTick()-start > 5000)
                UNIMPLEMENTED;
        }
    }
    return new CRoxieAeronMessagePacker(publication.get(), id, sequence, messageHeader, headerSize, destNodeIndex, queue);
}


extern UDPLIB_API IReceiveManager *createAeronReceiveManager(const SocketEndpoint &ep, bool useEmbeddedMediaDriver)
{
    return new CRoxieAeronReceiveManager(ep, useEmbeddedMediaDriver);
}

extern UDPLIB_API ISendManager *createAeronSendManager(unsigned dataPort)
{
    return new CRoxieAeronSendManager(dataPort);
}
