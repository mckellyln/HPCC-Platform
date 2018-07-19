/*
 * To change this license header, choose License Headers in Project Properties.
 * To change this template file, choose Tools | Templates
 * and open the template in the editor.
 */

#include <mpi/mpi.h>
#include "mpi_wrapper.hpp"
#include <cstdlib>
#include <queue>
#include <typeinfo>

#include "mputil.hpp"

#define WAIT_DELAY 100
#define PROBING_DELAY 100

//----------Functions and Data structures managing communication data related to Send/Recv Communications in orogress-----------//

// Data structure to keep the data relating to send/receive communications
class CommData
{
private:
    bool send;                      // TRUE => relates to a send communication | FALSE => relates to receive communication
    bool locked = false;
    bool cancellationLock = false;
    bool cancellationInProgress = false;
//    CMessageBuffer* data = NULL;              // Data structure which points to the sent/recv buffer
    CriticalSection dataChangeLock;
    void lock(){if (!locked) dataChangeLock.enter(); locked=true;}
    void unlock(){if (locked) dataChangeLock.leave(); locked=false;}
public:    
    bool isSend(){return send;}
    bool isReceive(){return !isSend();}
    bool isEqual(bool _send, int _rank, int _tag, const MPI::Comm& _comm)
    {
        return (send==_send) && (_rank==MPI_ANY_SOURCE || rank==_rank) && (_tag==MPI_ANY_TAG || tag==_tag) && (comm == _comm);
    }

    int rank;                       // source/destination rank of the processor
    int tag;                        // MPI tag information
    MPI::Request request;           // persistent request object to keep track of ongoing MPI call
    const MPI::Comm& comm;                // MPI communicator


    CommData(bool _send, int _rank, int _tag, const MPI::Comm& _comm):
        send(_send), rank(_rank), tag(_tag), comm(_comm){}

    CommData(int _rank, int _tag, const MPI::Comm& _comm): rank(_rank), tag(_tag), comm(_comm), send(false){}

    void notifyCancellation()
    {
        lock();
        cancellationInProgress = true;
        unlock();
    }

    bool lockFromCancellation()         //returns true only if currently some thread is not attempting to cancel
    {
        lock();
        if (cancellationInProgress)
        {
            unlock();
        } else
        {
            cancellationLock = true;
        }
        return !cancellationInProgress;
    }

    void releaseCancellationLock()
    {
        if (cancellationLock)
        {
            cancellationLock = false;
            unlock();
        }//TODO else clause: throw a meaningful error for invalid call for this function
    }

    ~CommData()
    {

    }
};

std::vector<CommData*> asyncCommData; // CommData list to manage while send/recv communication in progress
CriticalSection commDataLock;         // A mutex lock for the index list above

CommData* _popCommData(int index)
{
    _TF("_popCommData", index);
    CommData* ret = NULL;
    assertex(index>=0);
    assertex(index < asyncCommData.size());
    ret = asyncCommData[index];
    asyncCommData.erase(asyncCommData.begin() + index);
    return ret;
}

void addCommData(CommData *commData)
{
    _TF("addCommData", commData->rank, commData->tag);
    CriticalBlock block(commDataLock);

    //TODO Do a cleanup while we are at it
//    int size = asyncCommData.size(); int completed; MPI_Status stat;
//    for(int i=(size-1); i>=0 ; i--)
//    {
//        if (!(asyncCommData[i]->probingProgress))
//        {
//            completed = 0;
//            assertex(asyncCommData[i]->request != NULL);
//            _T("asyncCommData[i]->request="<<*(asyncCommData[i]->request)<<" mem_address="<<asyncCommData[i]->request);
//            _T("send="<<asyncCommData[i]->isSend()<<" rank="<<asyncCommData[i]->rank<<" tag="<<asyncCommData[i]->tag);
//            bool error = (MPI_Test(asyncCommData[i]->request, &completed, &stat)!= MPI_SUCCESS);
//            if (completed || error) //unlikely an error would occur
//            {
//                delete _popCommData(i);
//            }
//        }
//    }

    asyncCommData.push_back(commData);
}

void popCommData(bool send, int rank, int tag, const MPI::Comm& comm, std::vector<CommData*> &matchedResults)
{
   _TF("popCommData", rank, tag);
   CriticalBlock block(commDataLock);
   size_t size = asyncCommData.size();
   for(size_t i=(size-1); i>=0; i--)
   {
       if (asyncCommData[i]->isEqual(send, rank, tag, comm))
           matchedResults.push_back(_popCommData(i));
   }
}

CommData* popCommData(CommData * commData)
{
    _TF("popCommData(CommData * commData)", commData->rank, commData->tag);
    CriticalBlock block(commDataLock);
    size_t size = asyncCommData.size();
    for(size_t i=(size-1); i>=0; i--)
    {
        if (asyncCommData[i]==commData)
        {
            _popCommData(i);
            break;
        }
    }
    return commData;
}

//----------------------------------------------------------------------------------------------------------------------------//

#define TAG_UB 1000000 //INT_MAX //MPI_TAG_UB
int getRank(rank_t sourceRank)
{
    _TF("getRank", sourceRank);
    if (sourceRank == RANK_ALL)
        return MPI::ANY_SOURCE;
    else
        return sourceRank;
}

#define SPECIAL_TAG_BASE (TAG_UB/2) // reserve half range for special tags (inc. reply tags)
int getMPITag(mptag_t mptag)
{
    _TF("getTag", mptag);

    if (mptag == TAG_ALL)
        return MPI::ANY_TAG;
    else
    {
        int tag = (int)mptag;
        if (tag < 0)
        {
            assertex(-tag < TAG_UB-SPECIAL_TAG_BASE); // check not out of range
            return -tag + SPECIAL_TAG_BASE;
        }
        else
        {
            assertex(tag <= TAG_UB);
            return tag;
        }
    }
}

mptag_t getMPTag(int tag)
{
    _TF("getMPTag", tag);
    unsigned returnVal;
    if (tag == MPI::ANY_TAG)
        returnVal = TAG_ALL;
    else
    {
        if (tag > SPECIAL_TAG_BASE)
            returnVal = (mptag_t) -(tag-SPECIAL_TAG_BASE);
        else
            returnVal = tag;
    }
    return (mptag_t)returnVal;
}

MPI::Status waitToComplete( bool& completed, bool& error, bool& canceled, bool& timedout, unsigned timeout, CommData *commData)
{
    _TF("waitToComplete", completed, error, canceled, timeout);
    CTimeMon tm(timeout);
    MPI::Status stat;
    unsigned remaining;
    bool noCancellation;
    while ((noCancellation = commData->lockFromCancellation()) && !(completed || error || (timedout = tm.timedout(&remaining))))
    {
        completed = commData->request.Test(stat);
        commData->releaseCancellationLock();
        usleep(WAIT_DELAY);
    }
    if (completed)
        canceled = stat.Is_cancelled();
    else
        canceled = !noCancellation;

    return stat;
}

MPI::Status hasIncomingData(int sourceRank, int mptag, const MPI::Comm& comm,
        bool& incomingMessage, bool& error, bool& canceled, unsigned timeout, CommData *commData)
{
    _TF("hasIncomingData", sourceRank, mptag, incomingMessage, error, timeout);

    CTimeMon tm(timeout);
    MPI::Status stat;

    int flag;
    unsigned remaining;
    bool noCancellation;

    while ((noCancellation = commData->lockFromCancellation()) && !(incomingMessage || error || (timeout !=0 && tm.timedout(&remaining))))
    {
        incomingMessage = comm.Iprobe(sourceRank, mptag, stat);
        commData->releaseCancellationLock();
        if (timeout == 0) break;
        usleep(PROBING_DELAY);
    }
    canceled = !noCancellation;
    return stat;
}

//----------------------------------------------------------------------------//

/** See mpi_wrapper.hpp header file for function descriptions of the following **/

bool hpcc_mpi::hasIncomingMessage(rank_t &sourceRank, mptag_t &mptag, const MPI::Comm& comm)
{
    _TF("hasIncomingMessage", sourceRank, mptag);
    mpiInitializedCheck();
    bool incomingMessage = false; bool error = false; bool canceled = false;
    int source = getRank(sourceRank);
    int tag = getMPITag(mptag);
    CommData* tmpCommData = new CommData(source, tag, comm);
    MPI::Status stat = hasIncomingData(source, tag, comm, incomingMessage, error, canceled, 0, tmpCommData);
    if (incomingMessage)
    {
        sourceRank = stat.Get_source();
        mptag = getMPTag(stat.Get_tag());
    }

    return incomingMessage;
}

bool cancelComm(CommData* commData)
{
    bool ret = true;
    if (commData)
    {
        MPI::Status stat;
        //TODO: try catch for error
        bool completed;
        completed = commData->request.Test(stat);
        if (!completed)
        {
            commData->request.Cancel();
            commData->request.Free();
        }
    }

    return ret;
}

bool hpcc_mpi::cancelComm(bool send, rank_t rank, mptag_t mptag, const MPI::Comm& comm)
{
    _TF("cancelComm", send, rank, mptag);
    mpiInitializedCheck();
    int r = getRank(rank);
    int tag = getMPITag(mptag);
    std::vector<CommData*> commDataList;
    popCommData(send, r, tag, comm, commDataList);
    bool success = true;
    for (auto &commData: commDataList)
    {
        commData->notifyCancellation();              // In case sendData and readData functions still in progress we want to let
                                                                    // them know that we are about to screw up their plans
        success = success && cancelComm(commData);   // If we managed to cancel everything then the cancellation was successful
        usleep(WAIT_DELAY);                                 // Wait for a short while for active threads to exit send/recv function
        delete commData;
    }
    return success;
}

rank_t hpcc_mpi::rank(const MPI::Comm& comm)
{
    _TF("rank");
    mpiInitializedCheck();
    return comm.Get_rank();
}

rank_t hpcc_mpi::size(const MPI::Comm& comm)
{
    _TF("size");
    mpiInitializedCheck();
    return comm.Get_size();
}

#define MPI_BUFFER_SIZE 16777216        // 16MB
void* mpi_buffer_data;

void hpcc_mpi::initialize()
{
    _TF("initialize");
    mpiInitializedCheck(false);
    int required = MPI_THREAD_MULTIPLE;
    int provided = MPI::Init_thread(required);
    assertex(provided == required);
    mpi_buffer_data = malloc(MPI_BUFFER_SIZE);
    MPI::Attach_buffer(mpi_buffer_data, MPI_BUFFER_SIZE);
#ifdef DEBUG
    global_proc_rank = MPI::COMM_WORLD.Get_rank();
#endif
}

void hpcc_mpi::setErrorHandler(MPI::Comm& comm, MPI::Errhandler handler)
{
    mpiInitializedCheck();
    comm.Set_errhandler(handler);
}

void hpcc_mpi::finalize()
{
    _TF("finalize");
    mpiInitializedCheck();
    int size;
    void *ptr;

    size = MPI::Detach_buffer(ptr);
    free(mpi_buffer_data);
    MPI::Finalize();
}

void pushMPTag(CMessageBuffer &mbuf, mptag_t tag)
{
    serializeMPtag(mbuf, tag);
}

mptag_t popMPTag(CMessageBuffer &mbuf)
{
    size_t currentPos = mbuf.getPos();
    size_t newLength = mbuf.length() - sizeof(mptag_t);
    mbuf.reset(newLength);
    mptag_t replyTag;
    deserializeMPtag(mbuf, replyTag);
    mbuf.setLength(newLength);
    if (currentPos > newLength)
    {
        currentPos = newLength;
    }
    mbuf.reset(currentPos);
    return replyTag;
}

hpcc_mpi::CommStatus hpcc_mpi::sendData(rank_t dstRank, mptag_t mptag, CMessageBuffer &mbuf, const MPI::Comm& comm, unsigned timeout)
{
    _TF("sendData", dstRank, mptag, mbuf.getReplyTag(), timeout);
    mpiInitializedCheck();
    CTimeMon tm(timeout);
    unsigned remaining;
    int target = getRank(dstRank); int tag = getMPITag(mptag);
//    mptag_t t = getTag(tag);
//    assertex(t==tag);
//    _T("Rank="<<target<<" Tag="<<tag);
    CommData* commData = new CommData(true, target, tag, comm);
    bool timedout = false; bool error = false; bool canceled = false; bool completed;
    bool bufferedSendComplete = false;
    addCommData(commData); //So that it can be cancelled from outside

    //TODO: find a better way to send the reply tag
    pushMPTag(mbuf, mbuf.getReplyTag());
    while(!bufferedSendComplete)
    {
        try
        {
            bool notCanceled = commData->lockFromCancellation();
            if (notCanceled)
            {
                commData->request  = comm.Ibsend(mbuf.bufferBase(), mbuf.length(), MPI_BYTE, target, tag);
                commData->releaseCancellationLock();
            }
            bufferedSendComplete = true;
            //remove reply tag from the buffer
            popMPTag(mbuf);
            canceled = !notCanceled;
        } catch (MPI::Exception &e)
        {
            commData->releaseCancellationLock();
            if (e.Get_error_class() == MPI::ERR_BUFFER)
                usleep(WAIT_DELAY); //retry after giving some time for the buffers to clear up
            else
            {
                _T("Error occured while trying to do Ibsend code="<<e.Get_error_code()<<" string="<<e.Get_error_string());
                throw e;
            }
        }
    }

    timedout = tm.timedout(&remaining);

    if (!error && timedout)
    {
        popCommData(commData);
        cancelComm(commData);
    }

    hpcc_mpi::CommStatus status =
            error ? hpcc_mpi::CommStatus::ERROR
                    : (timedout?    hpcc_mpi::CommStatus::TIMEDOUT
                                    :canceled?  hpcc_mpi::CommStatus::CANCELED
                                            :hpcc_mpi::CommStatus::SUCCESS);
    return status;
}

hpcc_mpi::CommStatus hpcc_mpi::readData(rank_t &sourceRank, mptag_t &mptag, CMessageBuffer &mbuf, const MPI::Comm& comm, unsigned timeout)
{
    _TF("readData", sourceRank, mptag, timeout);
    mpiInitializedCheck();
    CTimeMon tm(timeout);
    unsigned remaining;
    bool incomingMessage = false; bool error = false; bool completed = false; bool canceled = false; bool timedout = false;

    tm.timedout(&remaining);
    int source = getRank(sourceRank);
    int tag = getMPITag(mptag);

    CommData* commData = new CommData(source, tag, comm);
    addCommData(commData); //So that it can be cancelled from outside

    MPI::Status stat = hasIncomingData(source, tag, comm, incomingMessage, error, canceled, remaining, commData);

    timedout = tm.timedout(&remaining);
    if (incomingMessage && !canceled)
    {
        int size = stat.Get_count(MPI_BYTE);
        assertex(size>0);
        mbuf.setLength(size);
        bool notCanceled = commData->lockFromCancellation();
        if (notCanceled)
        {
            commData->request  = comm.Irecv(mbuf.bufferBase(), mbuf.length(), MPI_BYTE, source, tag);
            commData->releaseCancellationLock();
        }
        canceled = !notCanceled;
        tm.timedout(&remaining);

        MPI::Status stat = waitToComplete(completed, error, canceled, timedout, remaining, commData);
        if (!canceled)
        {   //if it was canceled by another thread commData would have cleanedup after itself so nothing to do here.
            if (!error && completed)
            {
                bool noCancellation = commData->lockFromCancellation();
                if (noCancellation)
                {
                    sourceRank = stat.Get_source();
                    mptag = getMPTag(stat.Get_tag());
                    //pop the reply tag
                    mbuf.setReplyTag(popMPTag(mbuf));
                    commData->releaseCancellationLock();
                }
                canceled = !noCancellation;
            } else if (timedout)
            {
                cancelComm(commData);
                canceled = true;
            }
        }

    }
    if (!canceled)
    {
        popCommData(commData);
        delete commData;
    }
    hpcc_mpi::CommStatus status =
            error ? hpcc_mpi::CommStatus::ERROR
                    : (completed? (canceled? hpcc_mpi::CommStatus::CANCELED
                                             : hpcc_mpi::CommStatus::SUCCESS)
                                  : hpcc_mpi::CommStatus::TIMEDOUT);
    return status;
}

void hpcc_mpi::barrier(const MPI::Comm& comm)
{
    _TF("barrier");
    mpiInitializedCheck();
    comm.Barrier();
}

void hpcc_mpi::mpiInitializedCheck(bool isInitialized) {
    assert(MPI::Is_initialized() == isInitialized);
}

void hpcc_mpi::mpiFinalizedCheck(bool isFinalized){
    assert(MPI::Is_finalized() == isFinalized);
}
