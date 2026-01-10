#include <cstring>
#include <random>

#include "base/trace.hh"
#include "dev/x86/cxl_type1_accel.hh"
#include "debug/CXLType1Accel.hh"

namespace gem5
{

CXLType1Accel::CXLType1Accel(const Params &p)
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
    stats(*this),
    runEvent(*this),
    runStage2(*this)
    { 
        remote_data = new uint8_t[64]; // cacheline size is 64 bytes
    }

CXLType1Accel::CXLStats::CXLStats(CXLType1Accel &_device)
    : statistics::Group(&_device),

      ADD_STAT(totalLoadLatency, statistics::units::Tick::get(),
               "total lsu load latency")
{ }


void
CXLType1Accel::TimingDevicePort::TickEvent::schedule(PacketPtr _pkt, Tick t)
{
    pkt = _pkt;
    device->schedule(this, t);
}

void
CXLType1Accel::DcachePort::recvTimingSnoopReq(PacketPtr pkt)
{
    DPRINTF(CXLType1Accel, "%s received atomic snoop pkt for addr:%#x %s\n",
            __func__, pkt->getAddr(), pkt->cmdString());
}

bool
CXLType1Accel::DcachePort::recvTimingResp(PacketPtr pkt)
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
CXLType1Accel::recvData(PacketPtr pkt)
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
CXLType1Accel::LSUFinish()
{
    accelStatus = Returning;
    stats.totalLoadLatency = clockEdge() - first_issue_time;
    LSU_finished = 1;
}

bool
CXLType1Accel::IcachePort::recvTimingResp(PacketPtr pkt)
{
    DPRINTF(CXLType1Accel, "Received fetch response %#x\n", pkt->getAddr());
    return true;
}

void
CXLType1Accel::IcachePort::recvReqRetry()
{
    DPRINTF(CXLType1Accel, "Received retry req\n");
}

void
CXLType1Accel::DcachePort::DTickEvent::process()
{
    device->completeDataAccess(pkt);
}

void
CXLType1Accel::DcachePort::recvReqRetry()
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
CXLType1Accel::getPort(const std::string &if_name, PortID idx)
{
    if (if_name == "dma") {
        return dmaPort;
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

AddrRangeList
CXLType1Accel::getAddrRanges() const 
{
    return PciDevice::getAddrRanges();
}

Addr
CXLType1Accel::getPhyAddr(int index)
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
CXLType1Accel::read(PacketPtr pkt)
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
CXLType1Accel::write(PacketPtr pkt)
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
CXLType1Accel::runLSU()
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
CXLType1Accel::stage1()
{
    stage = 1;
    cur_paddr = this->getPhyAddr(cur_num);
    this->schedule(runStage2, this->nextCycle());
}

void
CXLType1Accel::stage2()
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
CXLType1Accel::accessMemory(Addr paddr, int size, bool read, uint8_t *data)
{
    RequestPtr req = std::make_shared<Request>(paddr, size, 0, 0);
    this->sendData(req, data, read);
}

void
CXLType1Accel::sendData(const RequestPtr &req, uint8_t *data, bool read)
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
CXLType1Accel::buildPacket(const RequestPtr &req, bool read)
{
    return read ? Packet::createRead(req) : Packet::createWrite(req);
}

bool
CXLType1Accel::handleReadPacket(PacketPtr pkt)
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
CXLType1Accel::handleWritePacket()
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
CXLType1Accel::completeDataAccess(PacketPtr pkt)
{
    DPRINTF(CXLType1Accel, "completeDataAccess: %s\n", pkt->cmdString());
}

} // namespace gem5