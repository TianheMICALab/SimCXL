#include <cstring>
#include <random>

#include "base/trace.hh"
#include "dev/x86/cxl_type2_accel.hh"
#include "debug/CXLMemCtrl.hh"
#include "debug/CXLType1Accel.hh"

namespace gem5
{

CXLType2Accel::CXLResponsePort::CXLResponsePort(const std::string& _name,
                                        CXLType2Accel& _ctrl,
                                        CXLRequestPort& _memReqPort,
                                        Cycles _protoProcLat, int _resp_limit,
                                        AddrRange _devMemRange)
    : ResponsePort(_name), ctrl(_ctrl),
    memReqPort(_memReqPort), protoProcLat(_protoProcLat),
    devMemRange(_devMemRange), outstandingResponses(0), 
    retryReq(false), respQueueLimit(_resp_limit),
    sendEvent([this]{ trySendTiming(); }, _name)
{
}

CXLType2Accel::CXLRequestPort::CXLRequestPort(const std::string& _name,
                                    CXLType2Accel& _ctrl,
                                    CXLResponsePort& _cxlRspPort,
                                    Cycles _protoProcLat, int _req_limit)
    : RequestPort(_name), ctrl(_ctrl),
    cxlRspPort(_cxlRspPort),
    protoProcLat(_protoProcLat), reqQueueLimit(_req_limit),
    sendEvent([this]{ trySendTiming(); }, _name)
{
}

CXLType2Accel::CXLType2Accel(const Params &p)
    : PciDevice(p),
    lsu_mode(p.lsu_mode),
    lsu_num(p.lsu_num),
    load_store(p.load_store),
    cur_num(0),
    recv_num(0),
    put_param_finished(0),
    LSU_finished(0),
    stage(3),
    remote_data(nullptr),
    cur_paddr(0),
    dcachePort(this),
    icachePort(this),
    cacheLineSize(p.cacheline_size),
    accelStatus(Uninitialized),
    cxlRspPort(p.name + ".cxl_rsp_port", *this, memReqPort,
            ticksToCycles(p.proto_proc_lat), p.rsp_size, p.cxl_mem_range),
    memReqPort(p.name + ".mem_req_port", *this, cxlRspPort,
            ticksToCycles(p.proto_proc_lat), p.req_size),
    preRspTick(0),
    stats(*this),
    runEvent(*this),
    runStage2(*this)
    {
        remote_data = new uint8_t[64]; // cacheline size is 64 bytes
        DPRINTF(CXLMemCtrl, "BAR0_addr:0x%lx, BAR0_size:0x%lx\n",
            p.BAR0->addr(), p.BAR0->size());
    }

CXLType2Accel::CXLStats::CXLStats(CXLType2Accel &_ctrl)
    : statistics::Group(&_ctrl),

      ADD_STAT(totalLoadLatency, statistics::units::Tick::get(),
               "total lsu load latency"),
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
      ADD_STAT(rspQueueLatDist, "Response queue latency distribution (Tick)"),
      ADD_STAT(memToCXLCtrlRsp, "Distribution of the time intervals between "
               "consecutive mem responses from the memory media to the CXLCtrl (Cycle)")
{ 
    reqQueueLenDist
        .init(0, 49, 10)
        .flags(statistics::nozero);
    rspQueueLenDist
        .init(0, 49, 10)
        .flags(statistics::nozero);
    rspOutStandDist
        .init(0, 49, 10)
        .flags(statistics::nozero);
    reqQueueLatDist
        .init(12000, 41999, 1000)
        .flags(statistics::nozero);
    rspQueueLatDist
        .init(12000, 41999, 1000)
        .flags(statistics::nozero);
    memToCXLCtrlRsp
        .init(0, 299, 10)
        .flags(statistics::nozero);
}


void
CXLType2Accel::TimingDevicePort::TickEvent::schedule(PacketPtr _pkt, Tick t)
{
    pkt = _pkt;
    device->schedule(this, t);
}

void
CXLType2Accel::DcachePort::recvTimingSnoopReq(PacketPtr pkt)
{
    DPRINTF(CXLType1Accel, "%s received atomic snoop pkt for addr:%#x %s\n",
            __func__, pkt->getAddr(), pkt->cmdString());
}

bool
CXLType2Accel::DcachePort::recvTimingResp(PacketPtr pkt)
{
    DPRINTF(CXLType1Accel, "Received load/store response %#x\n", pkt->getAddr());

    if (device->accelStatus == ExecutingLoop) {
        device->recvData(pkt);
    } else if (device->accelStatus == Returning) {
        // No need to process
    } else {
        panic("Got a memory response at a bad time");
    }

    // delete pkt->req;
    delete pkt;
    return true;
}

void
CXLType2Accel::recvData(PacketPtr pkt)
{
    if (pkt->isRead()) {
        pkt->writeData(remote_data);
        DPRINTF(CXLType1Accel, "Received LOAD data for addr %#x, recv_num: %d\n", 
                pkt->getAddr(), recv_num);
    } else if (pkt->isWrite()) {
        DPRINTF(CXLType1Accel, "Confirmed STORE completion for addr %#x, recv_num: %d\n", 
                pkt->getAddr(), recv_num);
    } else {
        panic("Unexpected packet type: %s", pkt->cmdString());
    }

    stage = 3;
    recv_num++;
    if (recv_num >= paddr.num) {
        DPRINTF(CXLType1Accel, "All LSU responses have been received!\n");
        LSUFinish();
    }
}

void
CXLType2Accel::LSUFinish()
{
    accelStatus = Returning;
    stats.totalLoadLatency = clockEdge() - first_issue_time;
    LSU_finished = 1;
}

bool
CXLType2Accel::IcachePort::recvTimingResp(PacketPtr pkt)
{
    DPRINTF(CXLType1Accel, "Received fetch response %#x\n", pkt->getAddr());
    return true;
}

void
CXLType2Accel::IcachePort::recvReqRetry()
{
    DPRINTF(CXLType1Accel, "Received retry req\n");
}

void
CXLType2Accel::DcachePort::DTickEvent::process()
{
    device->completeDataAccess(pkt);
}

void
CXLType2Accel::DcachePort::recvReqRetry()
{
    // we shouldn't get a retry unless we have a packet that we're
    // waiting to transmit
    assert(device->dcache_pkt != NULL);
    assert(device->status == DcacheRetry);
    PacketPtr tmp = device->dcache_pkt;
    if (tmp->senderState) {
        // This is a packet from a split access.
        SplitFragmentSenderState * send_state =
            dynamic_cast<SplitFragmentSenderState *>(tmp->senderState);
        assert(send_state);
        PacketPtr big_pkt = send_state->bigPkt;

        SplitMainSenderState * main_send_state =
            dynamic_cast<SplitMainSenderState *>(big_pkt->senderState);
        assert(main_send_state);

        if (sendTimingReq(tmp)) {
            // If we were able to send without retrying, record that fact
            // and try sending the other fragment.
            send_state->clearFromParent();
            int other_index = main_send_state->getPendingFragment();
            if (other_index > 0) {
                tmp = main_send_state->fragments[other_index];
                device->dcache_pkt = tmp;
                if ((big_pkt->isRead() && device->handleReadPacket(tmp)) ||
                        (big_pkt->isWrite() && device->handleWritePacket())) {
                    main_send_state->fragments[other_index] = NULL;
                }
            } else {
                device->status = DcacheWaitResponse;
                // memory system takes ownership of packet
                device->dcache_pkt = NULL;
            }
        }
    } else if (sendTimingReq(tmp)) {
        DPRINTF(CXLType1Accel, "request LOAD for addr %#x, cur_num: %d\n", device->cur_paddr, device->cur_num);
        device->status = DcacheWaitResponse;
        // memory system takes ownership of packet
        device->dcache_pkt = NULL;
        device->cur_num++;
        if (device->cur_num < device->paddr.num)
            device->stage1();
        else 
            DPRINTF(CXLType1Accel, "All LSU requests have been issued!\n");
    } else {
        DPRINTF(CXLType1Accel, "Received retry req-3\n");
    }
}

Port &
CXLType2Accel::getPort(const std::string &if_name, PortID idx)
{
    if (if_name == "dma") {
        return dmaPort;
    } else if (if_name == "cxl_rsp_port") {
        return cxlRspPort;
    } else if (if_name == "mem_req_port") {
        return memReqPort;
    } else if (if_name == "dcache_port") {
        return dcachePort;
    } else if (if_name == "icache_port") {
        return icachePort;
    }
    else {
        return PioDevice::getPort(if_name, idx);
        fatal("%s does not have any master port named %s\n", name(), if_name);
    }
}

void
CXLType2Accel::init()
{
    if (!cxlRspPort.isConnected() || !memReqPort.isConnected()
         || !pioPort.isConnected() || !dcachePort.isConnected())
        panic("CXL port of %s not connected to anything!", name());

    pioPort.sendRangeChange();
    cxlRspPort.sendRangeChange();
}

AddrRangeList
CXLType2Accel::getAddrRanges() const
{
    DPRINTF(CXLMemCtrl, "PIO base AddrRanges:\n");
    AddrRangeList ranges = PciDevice::getAddrRanges();
    for (const auto &r : ranges) {
        DPRINTF(CXLMemCtrl,
                "  range [%#lx - %#lx) size %#lx\n",
                r.start(), r.end(), r.size());
    }
    return ranges;
}

Addr
CXLType2Accel::getPhyAddr(int index)
{
    Addr phy_addr = 0;
    thread_local static std::mt19937 generator(std::random_device{}());
    std::uniform_int_distribution<size_t> distribution(0, 63);
    switch(lsu_mode) {
        case 1:
            // single-point access, return the next address
            phy_addr = paddr.phy + cacheLineSize;
            // phy_addr = 0xb6900000;
            break;
        case 2:
            // sequential access
            phy_addr = paddr.phy + index * cacheLineSize;
            break;
        case 3:
            // random access
            phy_addr = paddr.phy + distribution(generator) * cacheLineSize;
            break;
        default:
            phy_addr = paddr.phy;
    }
    return phy_addr;
}

Tick
CXLType2Accel::read(PacketPtr pkt)
{
    DPRINTF(CXLType1Accel, "read address : (%lx, %lx)\n", pkt->getAddr(),
            pkt->getSize());
    if(LSU_finished == 0){
        // haven't done
        pkt->setRaw(0);
    }else if(LSU_finished == 1){
        // done
        pkt->setRaw(12);
    }

    if (pkt->needsResponse()) {
        pkt->makeResponse();
    }
    return pioDelay;
}

Tick
CXLType2Accel::write(PacketPtr pkt)
{
    DPRINTF(CXLType1Accel, "write address : (%lx, %lx)\n", pkt->getAddr(),
            pkt->getSize());
    if(paddr.phy == 0) {
        pkt->writeData((uint8_t*)&paddr.phy);
        paddr.phy = 0xb4900000;
        DPRINTF(CXLType1Accel, "get paddr phy: %d\n", paddr.phy);
    } else if(paddr.num == 0) {
        pkt->writeData((uint8_t*)&paddr.num);
        paddr.num = lsu_num; // Manually control the number of tests
        DPRINTF(CXLType1Accel, "get paddr num: %d\n", paddr.num);
    } else if(put_param_finished == 0){
        pkt->writeData((uint8_t*)&put_param_finished);
        if(put_param_finished == 1) {
            accelStatus = Initialized;
            schedule(runEvent, clockEdge(Cycles(1)));   // perform LSU
        }
    }

    if (pkt->needsResponse()) {
        pkt->makeResponse();
    }
    return pioDelay;
}

void
CXLType2Accel::runLSU()
{
    assert(accelStatus == Initialized);
    accelStatus = ExecutingLoop;

    // start compute
    assert(stage == 3 && cur_num < paddr.num);
    if (first_issue_time == -1) {
        first_issue_time = clockEdge();
    }
    stage1();
}

void
CXLType2Accel::stage1()
{
    stage = 1;
    cur_paddr = this->getPhyAddr(cur_num);
    this->schedule(runStage2, this->nextCycle());
}

void
CXLType2Accel::stage2()
{
    stage = 2;
    if (load_store == 1) { // load
        uint8_t *temp = new uint8_t[64];
        this->accessMemory(cur_paddr, 64, 1, temp);
    } else if (load_store == 2) { // store
        uint8_t *temp = new uint8_t[64];
        std::memset(temp, 0, 64);
        this->accessMemory(cur_paddr, 64, 0, temp);
    } else {
        panic("Invalid load/store mode\n");
    }
}

void
CXLType2Accel::accessMemory(Addr paddr, int size, bool read, uint8_t *data)
{
    RequestPtr req = std::make_shared<Request>(paddr, size, 0, 0);
    this->sendData(req, data, read);
}

void
CXLType2Accel::sendData(const RequestPtr &req, uint8_t *data, bool read)
{
    PacketPtr pkt = buildPacket(req, read);
    pkt->dataDynamic<uint8_t>(data);

    if (req->getFlags().isSet(Request::NO_ACCESS)) {
        assert(!dcache_pkt);
        pkt->makeResponse();
    } else if (read) {
        handleReadPacket(pkt);
    } else {
        bool do_access = true;  // flag to suppress cache access
        if (do_access) {
            dcache_pkt = pkt;
            handleWritePacket();
            do_access = false;
        } else {
            status = DcacheWaitResponse;
        }
    }
}

PacketPtr
CXLType2Accel::buildPacket(const RequestPtr &req, bool read)
{
    return read ? Packet::createRead(req) : Packet::createWrite(req);
}

bool
CXLType2Accel::handleReadPacket(PacketPtr pkt)
{
    if (!dcachePort.sendTimingReq(pkt)) {
        DPRINTF(CXLType1Accel, "Failed to send LOAD request, curr_num: %d\n", cur_num);
        status = DcacheRetry;
        dcache_pkt = pkt;
    } else {
        DPRINTF(CXLType1Accel, "request LOAD for addr %#x, cur_num: %d\n", cur_paddr, cur_num);
        cur_num++;
        if (cur_num < paddr.num)
            stage1();
        else 
            DPRINTF(CXLType1Accel, "All LSU requests have been issued!\n");

        status = DcacheWaitResponse;
        // memory system takes ownership of packet
        dcache_pkt = NULL;
    }
    return dcache_pkt == NULL;
}

bool
CXLType2Accel::handleWritePacket()
{
    if (!dcachePort.sendTimingReq(dcache_pkt)) {
        DPRINTF(CXLType1Accel, "Failed to send STORE request, curr_num: %d\n", cur_num);
        status = DcacheRetry;
    } else {
        DPRINTF(CXLType1Accel, "request STORE for addr %#x, cur_num: %d\n", cur_paddr, cur_num);
        cur_num++;
        if (cur_num < paddr.num)
            stage1();
        else 
            DPRINTF(CXLType1Accel, "All LSU requests have been issued!\n");

        status = DcacheWaitResponse;
        // memory system takes ownership of packet
        dcache_pkt = NULL;
    }
    return dcache_pkt == NULL;
}

void
CXLType2Accel::completeDataAccess(PacketPtr pkt)
{
    DPRINTF(CXLType1Accel, "completeDataAccess: %s\n", pkt->cmdString());
}


bool
CXLType2Accel::CXLResponsePort::respQueueFull() const
{
    if (outstandingResponses == respQueueLimit) {
        ctrl.stats.rspQueFullEvents++;
        return true;
    } else {
        return false;
    }
}

bool
CXLType2Accel::CXLRequestPort::reqQueueFull() const
{
    if (transmitList.size() == reqQueueLimit) {
        ctrl.stats.reqQueFullEvents++;
        return true;
    } else {
        return false;
    }
}

bool
CXLType2Accel::CXLRequestPort::recvTimingResp(PacketPtr pkt)
{
    // all checks are done when the request is accepted on the response
    // side, so we are guaranteed to have space for the response
    DPRINTF(CXLMemCtrl, "recvTimingResp: %s addr 0x%x\n",
            pkt->cmdString(), pkt->getAddr());

    DPRINTF(CXLMemCtrl, "Request queue size: %d\n", transmitList.size());

    if (ctrl.preRspTick == -1) {
        ctrl.preRspTick = ctrl.clockEdge();
    } else {
        ctrl.stats.memToCXLCtrlRsp.sample(
            ctrl.ticksToCycles(ctrl.clockEdge() - ctrl.preRspTick));
        ctrl.preRspTick = ctrl.clockEdge();
    }

    // technically the packet only reaches us after the header delay,
    // and typically we also need to deserialise any payload
    Tick receive_delay = pkt->headerDelay + pkt->payloadDelay;
    pkt->headerDelay = pkt->payloadDelay = 0;

    cxlRspPort.schedTimingResp(pkt, ctrl.clockEdge(protoProcLat) +
                              receive_delay);

    return true;
}

bool
CXLType2Accel::CXLResponsePort::recvTimingReq(PacketPtr pkt)
{
    DPRINTF(CXLMemCtrl, "recvTimingReq: %s addr 0x%x\n",
            pkt->cmdString(), pkt->getAddr());

    panic_if(pkt->cacheResponding(), "Should not see packets where cache "
             "is responding");

    if (retryReq)
        return false;

    DPRINTF(CXLMemCtrl, "Response queue size: %d outresp: %d\n",
            transmitList.size(), outstandingResponses);

    // if the request queue is full then there is no hope
    if (memReqPort.reqQueueFull()) {
        DPRINTF(CXLMemCtrl, "Request queue full\n");
        retryReq = true;
    } else {
        // look at the response queue if we expect to see a response
        bool expects_response = pkt->needsResponse();
        if (expects_response) {
            if (respQueueFull()) {
                DPRINTF(CXLMemCtrl, "Response queue full\n");
                retryReq = true;
            } else {
                // ok to send the request with space for the response
                DPRINTF(CXLMemCtrl, "Reserving space for response\n");
                assert(outstandingResponses != respQueueLimit);
                ++outstandingResponses;

                // no need to set retryReq to false as this is already the
                // case
                ctrl.stats.rspOutStandDist.sample(outstandingResponses);
            }
        }

        if (!retryReq) {
            Tick receive_delay = pkt->headerDelay + pkt->payloadDelay;
            pkt->headerDelay = pkt->payloadDelay = 0;

            memReqPort.schedTimingReq(pkt, ctrl.clockEdge(protoProcLat) +
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
CXLType2Accel::CXLResponsePort::retryStalledReq()
{
    if (retryReq) {
        DPRINTF(CXLMemCtrl, "Request waiting for retry, now retrying\n");
        retryReq = false;
        sendRetryReq();
        ctrl.stats.reqRetryCounts++;
    }
}

void
CXLType2Accel::CXLRequestPort::schedTimingReq(PacketPtr pkt, Tick when)
{
    // If we're about to put this packet at the head of the queue, we
    // need to schedule an event to do the transmit.  Otherwise there
    // should already be an event scheduled for sending the head
    // packet.
    if (transmitList.empty()) {
        ctrl.schedule(sendEvent, when);
    }

    assert(transmitList.size() != reqQueueLimit);

    transmitList.emplace_back(pkt, when);

    ctrl.stats.reqQueueLenDist.sample(transmitList.size());
}

void
CXLType2Accel::CXLResponsePort::schedTimingResp(PacketPtr pkt, Tick when)
{
    if (transmitList.empty()) {
        ctrl.schedule(sendEvent, when);
    }

    transmitList.emplace_back(pkt, when);

    ctrl.stats.rspQueueLenDist.sample(transmitList.size());
}

void
CXLType2Accel::CXLRequestPort::trySendTiming()
{
    assert(!transmitList.empty());

    DeferredPacket req = transmitList.front();

    assert(req.tick <= curTick());

    PacketPtr pkt = req.pkt;

    DPRINTF(CXLMemCtrl, "trySend request addr 0x%x, queue size %d\n",
            pkt->getAddr(), transmitList.size());

    if (sendTimingReq(pkt)) {
        // send successful
        ctrl.stats.reqSendSucceed++;
        ctrl.stats.reqQueueLatDist.sample(curTick() - req.entryTime);

        transmitList.pop_front();

        ctrl.stats.reqQueueLenDist.sample(transmitList.size());
        DPRINTF(CXLMemCtrl, "trySend request successful\n");

        // If there are more packets to send, schedule event to try again.
        if (!transmitList.empty()) {
            DeferredPacket next_req = transmitList.front();
            DPRINTF(CXLMemCtrl, "Scheduling next send\n");
            ctrl.schedule(sendEvent, std::max(next_req.tick,
                                                ctrl.clockEdge()));
        }

        // if we have stalled a request due to a full request queue,
        // then send a retry at this point, also note that if the
        // request we stalled was waiting for the response queue
        // rather than the request queue we might stall it again
        cxlRspPort.retryStalledReq();
    } else {
        ctrl.stats.reqSendFaild++;
    }

    // if the send failed, then we try again once we receive a retry,
    // and therefore there is no need to take any action
}

void
CXLType2Accel::CXLResponsePort::trySendTiming()
{
    assert(!transmitList.empty());

    DeferredPacket resp = transmitList.front();

    assert(resp.tick <= curTick());

    PacketPtr pkt = resp.pkt;

    DPRINTF(CXLMemCtrl, "trySend response addr 0x%x, outstanding %d\n",
            pkt->getAddr(), outstandingResponses);

    if (sendTimingResp(pkt)) {
        // send successful
        ctrl.stats.rspSendSucceed++;
        ctrl.stats.rspQueueLatDist.sample(curTick() - resp.entryTime);

        transmitList.pop_front();

        ctrl.stats.rspQueueLenDist.sample(transmitList.size());
        DPRINTF(CXLMemCtrl, "trySend response successful\n");

        assert(outstandingResponses != 0);
        --outstandingResponses;

        ctrl.stats.rspOutStandDist.sample(outstandingResponses);

        // If there are more packets to send, schedule event to try again.
        if (!transmitList.empty()) {
            DeferredPacket next_resp = transmitList.front();
            DPRINTF(CXLMemCtrl, "Scheduling next send\n");
            ctrl.schedule(sendEvent, std::max(next_resp.tick,
                                                ctrl.clockEdge()));
        }

        // if there is space in the request queue and we were stalling
        // a request, it will definitely be possible to accept it now
        // since there is guaranteed space in the response queue
        if (!memReqPort.reqQueueFull() && retryReq) {
            DPRINTF(CXLMemCtrl, "Request waiting for retry, now retrying\n");
            retryReq = false;
            sendRetryReq();
            ctrl.stats.reqRetryCounts++;
        }
    } else {
        ctrl.stats.rspSendFaild++;
    }

    // if the send failed, then we try again once we receive a retry,
    // and therefore there is no need to take any action
}

void
CXLType2Accel::CXLRequestPort::recvReqRetry()
{
    trySendTiming();
}

void
CXLType2Accel::CXLResponsePort::recvRespRetry()
{
    trySendTiming();
}

Tick
CXLType2Accel::CXLResponsePort::recvAtomic(PacketPtr pkt)
{
    DPRINTF(CXLMemCtrl, "CXLMemCtrl recvAtomic: %s AddrRange: %s\n",
            pkt->cmdString(), pkt->getAddrRange().to_string());
    panic_if(pkt->cacheResponding(), "Should not see packets where cache "
             "is responding");
    
    Cycles delay = processCXLMem(pkt);

    Tick access_delay = memReqPort.sendAtomic(pkt);

    DPRINTF(CXLMemCtrl, "access_delay=%ld, proto_proc_lat=%ld, total=%ld\n",
            access_delay, delay, delay * ctrl.clockPeriod() + access_delay);
    return delay * ctrl.clockPeriod() + access_delay;
}

Tick
CXLType2Accel::CXLResponsePort::recvAtomicBackdoor(
    PacketPtr pkt, MemBackdoorPtr &backdoor)
{
    Cycles delay = processCXLMem(pkt);

    return delay * ctrl.clockPeriod() + memReqPort.sendAtomicBackdoor(
        pkt, backdoor);
}

Cycles
CXLType2Accel::CXLResponsePort::processCXLMem(PacketPtr pkt) {
    if (pkt->cxl_cmd == MemCmd::M2SReq) {
        assert(pkt->isRead());
    } else if (pkt->cxl_cmd == MemCmd::M2SRwD) {
        assert(pkt->isWrite());
    }
    return protoProcLat + protoProcLat;
}

AddrRangeList
CXLType2Accel::CXLResponsePort::getAddrRanges() const
{
    AddrRangeList ranges;
    ranges.push_back(devMemRange);
    DPRINTF(CXLMemCtrl, "CXLResponsePort base AddrRanges:\n");
    for (const auto &r : ranges) {
        DPRINTF(CXLMemCtrl,
                "  range [%#lx - %#lx) size %#lx\n",
                r.start(), r.end(), r.size());
    }

    DPRINTF(CXLMemCtrl,
            "CXLResponsePort adds devMemRange [%#lx - %#lx) size %#lx\n",
            devMemRange.start(), devMemRange.end(), devMemRange.size());
    return ranges;
}

} // namespace gem5