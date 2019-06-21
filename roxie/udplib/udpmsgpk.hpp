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

#include "roxiemem.hpp"

class PackageSequencer;
typedef std::queue<PackageSequencer*> seq_map_que;
typedef std::queue<void*> ptr_que;

typedef unsigned __int64 PUID;
typedef MapXToMyClass<PUID, PUID, PackageSequencer> msg_map;

class CMessageCollator : public CInterfaceOf<IMessageCollator>
{
private:
    seq_map_que         queue;
    msg_map             mapping;
    bool                activity;
    bool                memLimitExceeded;
    CriticalSection     queueCrit;
    CriticalSection     mapCrit;
    InterruptableSemaphore sem;
    Linked<roxiemem::IRowManager> rowMgr;
    ruid_t ruid;
    unsigned totalBytesReceived;

public:
    CMessageCollator(roxiemem::IRowManager *_rowMgr, unsigned _ruid);
    virtual ~CMessageCollator();

    virtual ruid_t queryRUID() const override
    {
        return ruid;
    }

    virtual unsigned queryBytesReceived() const override;
    virtual IMessageResult *getNextResult(unsigned time_out, bool &anyActivity) override;
    virtual void interrupt(IException *E) override;

    bool add_package(roxiemem::DataBuffer *dataBuff);
};
