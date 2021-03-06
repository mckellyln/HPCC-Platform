/*##############################################################################

    HPCC SYSTEMS software Copyright (C) 2017 HPCC Systems®.

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

#include "Status.hpp"


Status::Status(const Status &source, const std::string &nodeId) : m_highestMsgLevel(statusMsg::info)
{
    for (auto &msgIt: source.m_messages)
    {
        if (msgIt.second.nodeId == nodeId)
            addMsg(msgIt.second);
    }
}


void Status::addMsg(const statusMsg &msg)
{
    addMsg(msg.msgLevel, msg.nodeId, msg.attribute, msg.msg);
}


void Status::addMsg(enum statusMsg::msgLevel level, const std::string &nodeId, const std::string &name, const std::string &msg)
{
    statusMsg statusMsg(level, nodeId, name, msg);
    m_messages.insert({level, statusMsg });
    if (level > m_highestMsgLevel)
        m_highestMsgLevel = level;
}


void Status::addUniqueMsg(enum statusMsg::msgLevel level, const std::string &nodeId, const std::string &name, const std::string &msg)
{
    bool duplicateFound = false;
    auto msgRange = m_messages.equal_range(level);
    for (auto msgIt = msgRange.first; msgIt != msgRange.second && !duplicateFound; ++msgIt)
    {
        duplicateFound = (msgIt->second.nodeId == nodeId) && (msgIt->second.attribute == name) && (msgIt->second.msg == msg);
    }

    if (!duplicateFound)
        addMsg(level, nodeId, name, msg);
}


std::vector<statusMsg> Status::getMessages(enum statusMsg::msgLevel level, bool andBelow, const std::string &nodeId, const std::string &attribute) const
{
    std::vector<statusMsg> msgs;
    enum statusMsg::msgLevel finalLevel = andBelow ? statusMsg::info : level;
    for (int curLvl = level; curLvl >= finalLevel; --curLvl)
    {
        auto msgRange = m_messages.equal_range(static_cast<enum statusMsg::msgLevel>(curLvl));
        for (auto msgIt = msgRange.first; msgIt != msgRange.second; ++msgIt)
        {
            if ((nodeId.empty() || nodeId == msgIt->second.nodeId) && (attribute.empty() || attribute == msgIt->second.attribute))
            {
                msgs.emplace_back(msgIt->second);
            }
        }
    }
    return msgs;
}


std::string Status::getStatusTypeString(enum statusMsg::msgLevel status) const
{
    std::string result = "Not found";
    switch (status)
    {
        case statusMsg::info:    result = "Info";     break;
        case statusMsg::change:  result = "Change";   break;
        case statusMsg::warning: result = "Warning";  break;
        case statusMsg::error:   result = "Error";    break;
        case statusMsg::fatal:   result = "Fatal";    break;
    }
    return result;
}


enum statusMsg::msgLevel Status::getMsgLevelFromString(const std::string &status) const
{
    enum statusMsg::msgLevel lvl;
    if (status == "info")          { lvl = statusMsg::info;     }
    else if (status == "warning")  { lvl = statusMsg::warning;  }
    else if (status == "error")    { lvl = statusMsg::error;    }
    else if (status == "fatal")    { lvl = statusMsg::fatal;    }
    else                           { lvl = statusMsg::fatal;    }

    return lvl;
}


void Status::add(const std::vector<statusMsg> msgs)
{
    for (auto msgIt = msgs.begin(); msgIt != msgs.end(); ++msgIt)
    {
        m_messages.insert({ (*msgIt).msgLevel, *msgIt });
    }
}
