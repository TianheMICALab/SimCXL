/*
 * Copyright (c) 2011-2013, 2015 ARM Limited
 * All rights reserved
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Copyright (c) 2006 The Regents of The University of Michigan
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * @file
 * Implementation of a memory-mapped bridge that connects a requestor
 * and a responder through a request and response queue.
 */

#include "mem/cxl_bridge.hh"

#include "base/trace.hh"
#include "debug/Bridge.hh"
#include "params/Bridge.hh"
#include "debug/CXLMemCtrl.hh"
#include <iterator>

namespace gem5
{

CXLBridge::BridgeResponsePort::BridgeResponsePort(const std::string& _name,
                                         CXLBridge& _bridge,
                                         BridgeRequestPort& _memSidePort,
                                         Cycles _bridge_lat, Cycles _proto_proc_lat,
                                         int _resp_limit, std::vector<AddrRange> _ranges)
    : ResponsePort(_name), bridge(_bridge),
      memSidePort(_memSidePort), bridge_lat(_bridge_lat),
      proto_proc_lat(_proto_proc_lat),
      ranges(_ranges.begin(), _ranges.end()),
      outstandingResponses(0), retryReq(false), respQueueLimit(_resp_limit),
      sendEvent([this]{ trySendTiming(); }, _name)
{
    for (auto i=ranges.begin(); i!=ranges.end(); i++)
        DPRINTF(CXLMemCtrl, "BridgeResponsePort.ranges = %s\n", i->to_string());

    cxl_range = ranges.back();
    DPRINTF(CXLMemCtrl, "cxl_mem_start = 0x%lx, cxl_mem_end = 0x%lx\n", cxl_range.start(), cxl_range.end());
}

CXLBridge::BridgeRequestPort::BridgeRequestPort(const std::string& _name,
                                           CXLBridge& _bridge,
                                           BridgeResponsePort& _cpuSidePort,
                                           Cycles _bridge_lat, Cycles _proto_proc_lat, int _req_limit)
    : RequestPort(_name), bridge(_bridge),
      cpuSidePort(_cpuSidePort),
      bridge_lat(_bridge_lat), proto_proc_lat(_proto_proc_lat), reqQueueLimit(_req_limit),
      sendEvent([this]{ trySendTiming(); }, _name)
{
}

CXLBridge::CXLBridge(const Params &p)
    : ClockedObject(p),
      cpuSidePort(p.name + ".cpu_side_port", *this, memSidePort,
                ticksToCycles(p.bridge_lat), ticksToCycles(p.proto_proc_lat), p.resp_fifo_depth, p.ranges),
      memSidePort(p.name + ".mem_side_port", *this, cpuSidePort,
                ticksToCycles(p.bridge_lat), ticksToCycles(p.proto_proc_lat), p.req_fifo_depth),      
      stats(*this)
{
}

CXLBridge::CXLBridgeStats::CXLBridgeStats(CXLBridge &_bridge)
    : statistics::Group(&_bridge),

      ADD_STAT(reqQueFullEvents, statistics::units::Count::get(),
               "Number of times the request queue has become full"),
      ADD_STAT(reqRetryCounts, statistics::units::Count::get(),
               "Number of times the request was sent for retry"),
      ADD_STAT(rspQueFullEvents, statistics::units::Count::get(),
               "Number of times the response queue has become full"),
      ADD_STAT(reqSendFaild, statistics::units::Count::get(),
               "Number of times the request send failed"),
      ADD_STAT(rspSendFaild, statistics::units::Count::get(),
               "Number of times the response send failed"),
      ADD_STAT(reqSendSucceed, statistics::units::Count::get(),
               "Number of times the request send succeeded"),
      ADD_STAT(rspSendSucceed, statistics::units::Count::get(),
               "Number of times the response send succeeded"),
      ADD_STAT(reqQueueLenDist, "Request queue length distribution (Count)"),
      ADD_STAT(rspQueueLenDist, "Response queue length distribution (Count)"),
      ADD_STAT(rspOutStandDist, "outstandingResponses distribution (Count)"),
      ADD_STAT(reqQueueLatDist, "Response queue latency distribution (Tick)"),
      ADD_STAT(rspQueueLatDist, "Response queue latency distribution (Tick)")
{
    reqQueueLenDist
        .init(0, 129, 10)
        .flags(statistics::nozero);
    rspQueueLenDist
        .init(0, 129, 10)
        .flags(statistics::nozero);
    rspOutStandDist
        .init(0, 129, 10)
        .flags(statistics::nozero);
    reqQueueLatDist
        .init(62000, 119999, 1000)
        .flags(statistics::nozero);
    rspQueueLatDist
        .init(62000, 119999, 1000)
        .flags(statistics::nozero);
}

Port &
CXLBridge::getPort(const std::string &if_name, PortID idx)
{
    if (if_name == "mem_side_port")
        return memSidePort;
    else if (if_name == "cpu_side_port")
        return cpuSidePort;
    else
        // pass it along to our super class
        return ClockedObject::getPort(if_name, idx);
}

void
CXLBridge::init()
{
    // make sure both sides are connected and have the same block size
    if (!cpuSidePort.isConnected() || !memSidePort.isConnected())
        fatal("Both ports of a bridge must be connected.\n");

    // notify the request side  of our address ranges
    cpuSidePort.sendRangeChange();
}

bool
CXLBridge::BridgeResponsePort::respQueueFull() const
{
    if (outstandingResponses == respQueueLimit) {
        bridge.stats.rspQueFullEvents++;
        return true;
    } else {
        return false;
    }
}

bool
CXLBridge::BridgeRequestPort::reqQueueFull() const
{
    if (transmitList.size() == reqQueueLimit) {
        bridge.stats.reqQueFullEvents++;
        return true;
    } else {
        return false;
    }
}

bool
CXLBridge::BridgeRequestPort::recvTimingResp(PacketPtr pkt)
{
    // all checks are done when the request is accepted on the response
    // side, so we are guaranteed to have space for the response
    DPRINTF(Bridge, "recvTimingResp: %s addr 0x%x\n",
            pkt->cmdString(), pkt->getAddr());

    DPRINTF(Bridge, "Request queue size: %d\n", transmitList.size());

    // technically the packet only reaches us after the header delay,
    // and typically we also need to deserialise any payload (unless
    // the two sides of the bridge are synchronous)
    Tick receive_delay = pkt->headerDelay + pkt->payloadDelay;
    pkt->headerDelay = pkt->payloadDelay = 0;
    auto total_delay = bridge_lat;
    if (pkt->getAddr() >= cpuSidePort.cxl_range.start() && pkt->getAddr() < cpuSidePort.cxl_range.end()) {
        total_delay = bridge_lat + proto_proc_lat;
        if (pkt->cxl_cmd == MemCmd::S2MDRS) {
            assert(pkt->isRead());
        }
        else if(pkt->cxl_cmd == MemCmd::S2MNDR) {
            assert(pkt->isWrite());
        }
        else
            DPRINTF(CXLMemCtrl, "the cmd of packet is %s, not a read or write.\n", pkt->cmd.toString());
        DPRINTF(CXLMemCtrl, "recvTimingResp: %s addr 0x%x, when tick%ld\n", 
            pkt->cmdString(), pkt->getAddr(), bridge.clockEdge(total_delay) + receive_delay);
    }
    cpuSidePort.schedTimingResp(pkt, bridge.clockEdge(total_delay) +
                              receive_delay);

    return true;
}

bool
CXLBridge::BridgeResponsePort::recvTimingReq(PacketPtr pkt)
{
    DPRINTF(Bridge, "recvTimingReq: %s addr 0x%x\n",
            pkt->cmdString(), pkt->getAddr());

    panic_if(pkt->cacheResponding(), "Should not see packets where cache "
             "is responding");

    // we should not get a new request after committing to retry the
    // current one, but unfortunately the CPU violates this rule, so
    // simply ignore it for now
    if (retryReq)
        return false;

    DPRINTF(Bridge, "Response queue size: %d outresp: %d\n",
            transmitList.size(), outstandingResponses);

    // if the request queue is full then there is no hope
    if (memSidePort.reqQueueFull()) {
        DPRINTF(Bridge, "Request queue full\n");
        retryReq = true;
    } else {
        // look at the response queue if we expect to see a response
        bool expects_response = pkt->needsResponse();
        if (expects_response) {
            if (respQueueFull()) {
                DPRINTF(Bridge, "Response queue full\n");
                retryReq = true;
            } else {
                // ok to send the request with space for the response
                DPRINTF(Bridge, "Reserving space for response\n");
                assert(outstandingResponses != respQueueLimit);
                ++outstandingResponses;

                // no need to set retryReq to false as this is already the
                // case
                bridge.stats.rspOutStandDist.sample(outstandingResponses);
            }
        }

        if (!retryReq) {
            // technically the packet only reaches us after the header
            // bridge_lat, and typically we also need to deserialise any
            // payload (unless the two sides of the bridge are
            // synchronous)
            Tick receive_delay = pkt->headerDelay + pkt->payloadDelay;
            pkt->headerDelay = pkt->payloadDelay = 0;
            auto total_delay = bridge_lat;
            if (pkt->getAddr() >= cxl_range.start() && pkt->getAddr() < cxl_range.end()) {
                total_delay = bridge_lat + proto_proc_lat;
                if (pkt->isRead())
                    pkt->cxl_cmd = MemCmd::M2SReq;
                else if(pkt->isWrite())
                    pkt->cxl_cmd = MemCmd::M2SRwD;
                else
                    DPRINTF(CXLMemCtrl, "the cmd of packet is %s, not a read or write.\n", pkt->cmd.toString());
                DPRINTF(CXLMemCtrl, "recvTimingReq: %s addr 0x%x, when tick%ld\n", 
                    pkt->cmdString(), pkt->getAddr(), bridge.clockEdge(total_delay) + receive_delay);
            }
            memSidePort.schedTimingReq(pkt, bridge.clockEdge(total_delay) +
                                      receive_delay);
        }
    }

    // remember that we are now stalling a packet and that we have to
    // tell the sending requestor to retry once space becomes available,
    // we make no distinction whether the stalling is due to the
    // request queue or response queue being full
    return !retryReq;
}

void
CXLBridge::BridgeResponsePort::retryStalledReq()
{
    if (retryReq) {
        DPRINTF(Bridge, "Request waiting for retry, now retrying\n");
        retryReq = false;
        sendRetryReq();
        bridge.stats.reqRetryCounts++;
    }
}

void
CXLBridge::BridgeRequestPort::schedTimingReq(PacketPtr pkt, Tick when)
{
    // If we're about to put this packet at the head of the queue, we
    // need to schedule an event to do the transmit.  Otherwise there
    // should already be an event scheduled for sending the head
    // packet.
    if (transmitList.empty()) {
        bridge.schedule(sendEvent, when);
    }

    assert(transmitList.size() != reqQueueLimit);

    transmitList.emplace_back(pkt, when);

    bridge.stats.reqQueueLenDist.sample(transmitList.size());
}


void
CXLBridge::BridgeResponsePort::schedTimingResp(PacketPtr pkt, Tick when)
{
    // If we're about to put this packet at the head of the queue, we
    // need to schedule an event to do the transmit.  Otherwise there
    // should already be an event scheduled for sending the head
    // packet.
    if (transmitList.empty()) {
        bridge.schedule(sendEvent, when);
    }

    transmitList.emplace_back(pkt, when);

    bridge.stats.rspQueueLenDist.sample(transmitList.size());
}

void
CXLBridge::BridgeRequestPort::trySendTiming()
{
    assert(!transmitList.empty());

    DeferredPacket req = transmitList.front();

    assert(req.tick <= curTick());

    PacketPtr pkt = req.pkt;

    DPRINTF(Bridge, "trySend request addr 0x%x, queue size %d\n",
            pkt->getAddr(), transmitList.size());

    if (sendTimingReq(pkt)) {
        // send successful
        bridge.stats.reqSendSucceed++;

        transmitList.pop_front();

        bridge.stats.reqQueueLenDist.sample(transmitList.size());
        DPRINTF(Bridge, "trySend request successful\n");

        // If there are more packets to send, schedule event to try again.
        if (!transmitList.empty()) {
            DeferredPacket next_req = transmitList.front();
            DPRINTF(Bridge, "Scheduling next send\n");
            bridge.schedule(sendEvent, std::max(next_req.tick,
                                                bridge.clockEdge()));
        }

        // if we have stalled a request due to a full request queue,
        // then send a retry at this point, also note that if the
        // request we stalled was waiting for the response queue
        // rather than the request queue we might stall it again
        cpuSidePort.retryStalledReq();
    } else {
        bridge.stats.reqSendFaild++;
    }

    // if the send failed, then we try again once we receive a retry,
    // and therefore there is no need to take any action
}

void
CXLBridge::BridgeResponsePort::trySendTiming()
{
    assert(!transmitList.empty());

    DeferredPacket resp = transmitList.front();

    assert(resp.tick <= curTick());

    PacketPtr pkt = resp.pkt;

    DPRINTF(Bridge, "trySend response addr 0x%x, outstanding %d\n",
            pkt->getAddr(), outstandingResponses);

    if (sendTimingResp(pkt)) {
        // send successful
        bridge.stats.rspSendSucceed++;

        transmitList.pop_front();

        bridge.stats.rspQueueLenDist.sample(transmitList.size());
        DPRINTF(Bridge, "trySend response successful\n");

        assert(outstandingResponses != 0);
        --outstandingResponses;

        bridge.stats.rspOutStandDist.sample(outstandingResponses);

        // If there are more packets to send, schedule event to try again.
        if (!transmitList.empty()) {
            DeferredPacket next_resp = transmitList.front();
            DPRINTF(Bridge, "Scheduling next send\n");
            bridge.schedule(sendEvent, std::max(next_resp.tick,
                                                bridge.clockEdge()));
        }

        // if there is space in the request queue and we were stalling
        // a request, it will definitely be possible to accept it now
        // since there is guaranteed space in the response queue
        if (!memSidePort.reqQueueFull() && retryReq) {
            DPRINTF(Bridge, "Request waiting for retry, now retrying\n");
            retryReq = false;
            sendRetryReq();
            bridge.stats.reqRetryCounts++;
        }
    } else {
        bridge.stats.rspSendFaild++;
    }

    // if the send failed, then we try again once we receive a retry,
    // and therefore there is no need to take any action
}

void
CXLBridge::BridgeRequestPort::recvReqRetry()
{
    trySendTiming();
}

void
CXLBridge::BridgeResponsePort::recvRespRetry()
{
    trySendTiming();
}

Tick
CXLBridge::BridgeResponsePort::recvAtomic(PacketPtr pkt)
{
    panic_if(pkt->cacheResponding(), "Should not see packets where cache "
             "is responding");
    if (pkt->getAddr() >= cxl_range.start() && pkt->getAddr() < cxl_range.end()) {
        DPRINTF(CXLMemCtrl, "the cmd of pkt is %s, addrRange is %s.\n",
            pkt->cmd.toString(), pkt->getAddrRange().to_string());
        if (pkt->isRead())
            pkt->cxl_cmd = MemCmd::M2SReq;
        else if(pkt->isWrite())
            pkt->cxl_cmd = MemCmd::M2SRwD;
        else
            DPRINTF(CXLMemCtrl, "the cmd of packet is %s, not a read or write.\n", pkt->cmd.toString());
        Tick access_delay = memSidePort.sendAtomic(pkt);
        Tick total_delay = (bridge_lat + proto_proc_lat) * bridge.clockPeriod() + access_delay;
        return total_delay;
    }
    else {
        return bridge_lat * bridge.clockPeriod() + memSidePort.sendAtomic(pkt);
    }
}

Tick
CXLBridge::BridgeResponsePort::recvAtomicBackdoor(
    PacketPtr pkt, MemBackdoorPtr &backdoor)
{
    return bridge_lat * bridge.clockPeriod() + memSidePort.sendAtomicBackdoor(
        pkt, backdoor);
}

void
CXLBridge::BridgeResponsePort::recvFunctional(PacketPtr pkt)
{
    pkt->pushLabel(name());

    // check the response queue
    for (auto i = transmitList.begin();  i != transmitList.end(); ++i) {
        if (pkt->trySatisfyFunctional((*i).pkt)) {
            pkt->makeResponse();
            return;
        }
    }

    // also check the request port's request queue
    if (memSidePort.trySatisfyFunctional(pkt)) {
        return;
    }

    pkt->popLabel();

    // fall through if pkt still not satisfied
    memSidePort.sendFunctional(pkt);
}

void
CXLBridge::BridgeResponsePort::recvMemBackdoorReq(
    const MemBackdoorReq &req, MemBackdoorPtr &backdoor)
{
    memSidePort.sendMemBackdoorReq(req, backdoor);
}

bool
CXLBridge::BridgeRequestPort::trySatisfyFunctional(PacketPtr pkt)
{
    bool found = false;
    auto i = transmitList.begin();

    while (i != transmitList.end() && !found) {
        if (pkt->trySatisfyFunctional((*i).pkt)) {
            pkt->makeResponse();
            found = true;
        }
        ++i;
    }

    return found;
}

AddrRangeList
CXLBridge::BridgeResponsePort::getAddrRanges() const
{
    return ranges;
}

} // namespace gem5
