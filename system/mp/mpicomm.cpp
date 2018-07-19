/*##############################################################################

    HPCC SYSTEMS software Copyright (C) 2012 HPCC Systems®.

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

#define mp_decl DECL_EXPORT

/* TBD
    lost packet disposal
    synchronous send
    connection protocol (HRPC);
    look at all timeouts
*/

#include "platform.h"
#include "portlist.h"
#include <vector>
#include <limits.h>

#include <unistd.h>

#include "mpicomm.hpp"
#include "mpbuff.hpp"
#include "mputil.hpp"
#include "mplog.hpp"

#include "mpi_wrapper.hpp"

class NodeCommunicator: public ICommunicator, public CInterface
{
private:

    Owned<IGroup> group;
    rank_t myrank;
    rank_t commSize;
    MPI::Comm& comm;
public:
    IMPLEMENT_IINTERFACE;

    bool send(CMessageBuffer &mbuf, rank_t dstrank, mptag_t tag, unsigned timeout)
    {
        _TF("send", dstrank, tag, mbuf.getReplyTag(), timeout);
        assertex(dstrank!=RANK_NULL);
        CTimeMon tm(timeout);
        rank_t startrank = dstrank;
        rank_t endrank;
        if (dstrank==RANK_ALL || dstrank==RANK_ALL_OTHER)
        {
            startrank = 0;
            endrank = commSize-1;
        } else if (dstrank==RANK_RANDOM)
        {
            if (commSize>1)
            {
                do
                {
                    startrank = getRandom()%commSize;
                } while (startrank==myrank);
            } else
            {
                assertex(myrank!=0);
                startrank = 0;
            }
            endrank = startrank;
        } else {
            endrank = startrank;
        }
        for (;startrank<=endrank;startrank++)
        {
                if ((startrank==myrank) && (dstrank==RANK_ALL_OTHER))
                    continue;
                unsigned remaining;
                if (tm.timedout(&remaining))
                    return false;
                hpcc_mpi::CommStatus status = hpcc_mpi::sendData(startrank, tag, mbuf, comm, remaining);
                if (status != hpcc_mpi::CommStatus::SUCCESS)
                    return false;
        }
       
        return true;
    }

    bool recv(CMessageBuffer &mbuf, rank_t srcrank, mptag_t tag, rank_t *sender, unsigned timeout=MP_WAIT_FOREVER)
    {
        _TF("recv", srcrank, tag, timeout);
        CTimeMon tm(timeout);
        unsigned remaining;
        bool success = false;

        tm.timedout(&remaining);
        hpcc_mpi::CommStatus status = hpcc_mpi::readData(srcrank, tag, mbuf, comm, remaining);
        success = (status == hpcc_mpi::CommStatus::SUCCESS);
        if (success)
        {
            const SocketEndpoint &ep = getGroup()->queryNode(srcrank).endpoint();
            mbuf.init(ep, tag, mbuf.getReplyTag());
            if (sender)
                *sender = srcrank;
            mbuf.reset();
        }
        return success;
    }
    
    void barrier(void)
    {
        hpcc_mpi::barrier(comm);
    }

    unsigned probe(rank_t srcrank, mptag_t tag, rank_t *sender, unsigned timeout=0)
    {
        _TF("probe", srcrank, tag, timeout);
        if (hpcc_mpi::hasIncomingMessage(srcrank, tag, comm))
        {
            if (sender)
                *sender = srcrank;
        }
        return false;
    }
    
    void flush(mptag_t tag)
    {
        // Handled by MPI
    }

    bool sendRecv(CMessageBuffer &mbuff, rank_t sendrank, mptag_t sendtag, unsigned timeout=MP_WAIT_FOREVER)
    {
        _TF("sendRecv", sendrank, sendtag, timeout);
        //TODO share timeout between send/recv?
        mptag_t replytag = createReplyTag();
        CTimeMon tm(timeout);
        mbuff.setReplyTag(replytag);
        unsigned remaining;
        if (tm.timedout(&remaining))
            return false;
        if (!send(mbuff,sendrank,sendtag,remaining)||tm.timedout(&remaining))
            return false;
        mbuff.clear();
        return recv(mbuff,sendrank,replytag,NULL,remaining);
    }

    bool reply(CMessageBuffer &mbuf, unsigned timeout=MP_WAIT_FOREVER)
    {
        _TF("reply", mbuf.getReplyTag(), timeout);
        mptag_t replytag = mbuf.getReplyTag();
        rank_t dstrank = getGroup()->rank(mbuf.getSender());
        if (dstrank!=RANK_NULL)
        {
            if (send (mbuf, dstrank, replytag,timeout))
            {
                mbuf.setReplyTag(TAG_NULL);
                return true;
            }
            return false;
        }
        return false;
    }

    void cancel(rank_t srcrank, mptag_t tag)
    {
        _TF("cancel", srcrank, tag);
        assertex(srcrank!=RANK_NULL);
        //cancel only recv calls?
        hpcc_mpi::cancelComm(false, srcrank, tag, comm);
    }

    bool verifyConnection(rank_t rank,  unsigned timeout)
    {
        // TODO: revisit to see how MPI behaves here
        return true;
    }

    bool verifyAll(bool duplex, unsigned timeout)
    {
        // TODO: revisit to see how MPI behaves here
        return true;
    }
    
    void disconnect(INode *node)
    {
        UNIMPLEMENTED;
    }

    IGroup &queryGroup()
    {
        return *group;
    }
    
    IGroup *getGroup()
    {
        return group.getLink();
    }

    NodeCommunicator(IGroup *_group, MPI::Comm& _comm): comm(_comm),  group(_group)
    {
        initializeComm(comm);

        commSize = hpcc_mpi::size(comm);
        myrank = hpcc_mpi::rank(comm);

        assertex(getGroup()->ordinality()==commSize);
    }
    
    ~NodeCommunicator()
    {
    }
};

ICommunicator *createMPICommunicator(IGroup *group)
{
    if (group)
        group->Link();
    return new NodeCommunicator(group, MPI::COMM_WORLD);
}

/** MPI framework should be initialized only once. But incase it is initialized
    multiple times we need to keep track of it such that it gets finalized only
    at the last call for finalize
    **/
int mpiInitCounter = 0;
CriticalSection initCounterBlock;


void startMPI()
{
    //Only initialize the framework once
    _TF("startMPI");
    CriticalBlock block(initCounterBlock);
    if (!mpiInitCounter)
        hpcc_mpi::initialize();
    hpcc_mpi::mpiInitializedCheck();
    mpiInitCounter++;
}

void stopMPI()
{
    //Only finalize the framework once when everyone had requested to finalize it.
    _TF("stopMPI");
    CriticalBlock block(initCounterBlock);
    mpiInitCounter--;
    if (mpiInitCounter == 0)
    {
        hpcc_mpi::finalize();
        hpcc_mpi::mpiFinalizedCheck();
    } else
        hpcc_mpi::mpiFinalizedCheck(false);
    assertex(mpiInitCounter>=0);
}


void initializeComm(MPI::Comm& comm)
{
    _TF("initializeComm");
    hpcc_mpi::mpiInitializedCheck();
    hpcc_mpi::setErrorHandler(comm, MPI::ERRORS_THROW_EXCEPTIONS);
}


int getMPIGlobalRank()
{
    _TF("getMPIGlobalRank");
    hpcc_mpi::mpiInitializedCheck();
    return hpcc_mpi::rank(MPI::COMM_WORLD);
}
