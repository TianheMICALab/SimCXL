#ifndef __DEV_X86_CXL_TYPE2_ACCEL_HH__
#define __DEV_X86_CXL_TYPE2_ACCEL_HH__

#include <deque>
#include <vector>

#include "base/addr_range.hh"
#include "base/trace.hh"
#include "base/types.hh"
#include "base/statistics.hh"
#include "mem/packet.hh"
#include "mem/packet_access.hh"
#include "mem/port.hh"
#include "params/CXLType2Accel.hh"
#include "dev/pci/device.hh"


namespace gem5
{

class CXLType2Accel : public PciDevice 
{
    private:
        struct item {
            unsigned long phy;
            int num;
            item() : phy(0), num(0) {}
        };

        class TimingDevicePort : public RequestPort {
            public:
                TimingDevicePort(const std::string& _name, CXLType2Accel* _device)
                    : RequestPort(_name), device(_device),
                    retryRespEvent([this]{ sendRetryResp(); }, name())
                { }
            protected:
            CXLType2Accel* device;

            struct TickEvent : public Event
            {
                PacketPtr pkt;
                CXLType2Accel *device;

                TickEvent(CXLType2Accel *_device) : pkt(NULL), device(_device) {}
                const char *description() const { return "Timing CXL tick"; }
                void schedule(PacketPtr _pkt, Tick t);
            };
            EventFunctionWrapper retryRespEvent;
        };

        class DcachePort : public TimingDevicePort {
            public:

                DcachePort(CXLType2Accel *_device)
                    : TimingDevicePort(_device->name() + ".dcache_port", _device),
                    tickEvent(_device)
                {
                cacheBlockMask = ~(device->getCacheLineSize() - 1);
                }

                Addr cacheBlockMask;
            protected:

            /** Snoop a coherence request, we need to check if this causes
            * a wakeup event on a device that is monitoring an address
            */
            virtual void recvTimingSnoopReq(PacketPtr pkt);

            virtual bool recvTimingResp(PacketPtr pkt);

            virtual void recvReqRetry();

            virtual bool isSnooping() const {
                return true;
            }

            struct DTickEvent : public TickEvent
            {
                DTickEvent(CXLType2Accel *_device)
                    : TickEvent(_device) {}
                void process();
                const char *description() const { return "Timing CXLDevice dcache tick"; }
            };

            DTickEvent tickEvent;

        };

        class IcachePort : public TimingDevicePort {
            public:

                IcachePort(CXLType2Accel *_device)
                    : TimingDevicePort(_device->name() + ".icache_port", _device)
                { }

            protected:

                virtual bool recvTimingResp(PacketPtr pkt);

                virtual void recvReqRetry();
        };

        class SplitMainSenderState : public Packet::SenderState {
            public:
                int outstanding;
                PacketPtr fragments[2];

                int
                getPendingFragment()
                {
                    if (fragments[0]) {
                        return 0;
                    } else if (fragments[1]) {
                        return 1;
                    } else {
                        return -1;
                    }
                }
        };

        class SplitFragmentSenderState : public Packet::SenderState {
            public:
                SplitFragmentSenderState(PacketPtr _bigPkt, int _index) :
                    bigPkt(_bigPkt), index(_index)
                {}
                PacketPtr bigPkt;
                int index;

                void
                clearFromParent()
                {
                    SplitMainSenderState * main_send_state =
                        dynamic_cast<SplitMainSenderState *>(bigPkt->senderState);
                    main_send_state->fragments[index] = NULL;
                }
        };

        int lsu_mode;
        int lsu_num;
        int load_store;

        int cur_num;
        int recv_num;
        int put_param_finished;
        int LSU_finished;
        int stage;
        uint8_t* remote_data;

        item paddr;
        Addr cur_paddr;

        DcachePort dcachePort;
        IcachePort icachePort;
        PacketPtr dcache_pkt;
        const unsigned int cacheLineSize;

    protected:
        enum Status
        {
            Idle,
            Running,
            Faulting,
            DTBWaitResponse,
            DcacheRetry,
            DcacheWaitResponse,
            DcacheWaitSwitch,
        };

        enum AccelStatus
        {
            // Status of Accelerator
            Uninitialized,     // Nothing is ready
            Initialized,       // monitorAddr and paramsAddr are valid
            // GettingParams,     // Have issued requests for params
            ExecutingLoop,     // Got all params, actually executing
            Returning          // Returning the result to the CPU thread
        };

        Status status;
        AccelStatus accelStatus;
        Tick first_issue_time = -1;

    protected:

        /**
        * A deferred packet stores a packet along with its scheduled
        * transmission time
        */
        class DeferredPacket
        {
        public:
            const Tick tick;
            const PacketPtr pkt;
            /** When did pkt enter the transmitList */
            const Tick entryTime;
            DeferredPacket(PacketPtr _pkt, Tick _tick) : 
                tick(_tick), pkt(_pkt),
                entryTime(curTick())
            { }
        };

        // Forward declaration to allow the response port to have a pointer
        class CXLRequestPort;

        /**
        * The port on the side that receives requests and sends
        * responses. The response port also has a buffer for the
        * responses not yet sent.
        */
        class CXLResponsePort : public ResponsePort
        {
            private:

                /** The CXLType2Accel to which this port belongs. */
                CXLType2Accel& ctrl;

                /**
                * Request port on which CXLType2Accel sends requests to the back-end memory media.
                */
                CXLRequestPort& memReqPort;

                /** Latency in protocol processing by CXLType2Accel. */
                const Cycles protoProcLat;

                /** Address ranges to pass through the CXLType2Accel */
                const AddrRange devMemRange;

                /**
                * Response packet queue. Response packets are held in this
                * queue for a specified delay to model the processing delay
                * of the CXLType2Accel.
                */
                std::deque<DeferredPacket> transmitList;

                /** Counter to track the outstanding responses. */
                unsigned int outstandingResponses;

                /** If we should send a retry when space becomes available. */
                bool retryReq;

                /** Max queue size for reserved responses. */
                unsigned int respQueueLimit;

                /**
                * Upstream caches need this packet until true is returned, so
                * hold it for deletion until a subsequent call
                */
                std::unique_ptr<Packet> pendingDelete;

                /**
                * Is this side blocked from accepting new response packets.
                *
                * @return true if the reserved space has reached the set limit
                */
                bool respQueueFull() const;

                /**
                * Handle send event, scheduled when the packet at the head of
                * the response queue is ready to transmit (for timing
                * accesses only).
                */
                void trySendTiming();

                /** Send event for the response queue. */
                EventFunctionWrapper sendEvent;

            public:
                /**
                * Constructor for the CXLResponsePort.
                *
                * @param _name the port name including the owner
                * @param _ctrl the structural owner
                * @param _memReqPort the request port of CXLType2Accel
                * @param _protoProcLat the delay in cycles from receiving to sending
                * @param _resp_limit the size of the response queue
                * @param _devMemRange the address range of the CXLType2Accel
                */
                CXLResponsePort(const std::string& _name, CXLType2Accel& _ctrl,
                                CXLRequestPort& _memReqPort, Cycles _protoProcLat,
                                int _resp_limit, AddrRange _devMemRange);

                /**
                * Queue a response packet to be sent out later and also schedule
                * a send if necessary.
                *
                * @param pkt a response to send out after a delay
                * @param when tick when response packet should be sent
                */
                void schedTimingResp(PacketPtr pkt, Tick when);

                /**
                * Retry any stalled request that we have failed to accept at
                * an earlier point in time. This call will do nothing if no
                * request is waiting.
                */
                void retryStalledReq();

            // protected:
                /** When receiving a timing request from the Host,
                    pass it to the back-end memory media. */
                bool recvTimingReq(PacketPtr pkt) override;

                /** When receiving a retry request from the Host,
                    pass it to the back-end memory media. */
                void recvRespRetry() override;

                /** When receiving an Atomic request from the Host,
                    pass it to the back-end memory media. */
                Tick recvAtomic(PacketPtr pkt) override;

                Tick recvAtomicBackdoor(
                    PacketPtr pkt, MemBackdoorPtr &backdoor) override;

                void recvMemBackdoorReq(
                    const MemBackdoorReq &req, MemBackdoorPtr &backdoor) override {};

                void recvFunctional(PacketPtr pkt) override {};

                /** When receiving a address range request the Host,
                    pass it to the back-end memory media. */
                AddrRangeList getAddrRanges() const override;

                Cycles processCXLMem(PacketPtr ptk);
        };


        /**
        * Port on the side that forwards requests to and receives 
        * responses from back-end memory media. The request port 
        * has a buffer for the requests not yet sent.
        */
        class CXLRequestPort : public RequestPort
        {
            private:
                /** The CXLType2Accel to which this port belongs. */
                CXLType2Accel& ctrl;

                /**
                * The response port on the other side of the CXLType2Accel.
                */
                CXLResponsePort& cxlRspPort;

                /** Latency in protocol processing by CXLType2Accel. */
                const Cycles protoProcLat;

                /**
                * Request packet queue. Request packets are held in this
                * queue for a specified delay to model the processing delay
                * of the CXLType2Accel.
                */
                std::deque<DeferredPacket> transmitList;

                /** Max queue size for request packets */
                const unsigned int reqQueueLimit;

                /**
                * Handle send event, scheduled when the packet at the head of
                * the outbound queue is ready to transmit (for timing
                * accesses only).
                */
                void trySendTiming();

                /** Send event for the request queue. */
                EventFunctionWrapper sendEvent;

            public:
                /**
                * Constructor for the CXLRequestPort.
                *
                * @param _name the port name including the owner
                * @param _ctrl the structural owner
                * @param _cxlRspPort the response port of CXLType2Accel
                * @param _protoProcLat the delay in cycles from receiving to sending
                * @param _req_limit the size of the request queue
                */
                CXLRequestPort(const std::string& _name, CXLType2Accel& _ctrl,
                                CXLResponsePort& _cxlRspPort, Cycles _protoProcLat,
                                int _req_limit);

                /**
                * Is this side blocked from accepting new request packets.
                *
                * @return true if the occupied space has reached the set limit
                */
                bool reqQueueFull() const;

                /**
                * Queue a request packet to be sent out later and also schedule
                * a send if necessary.
                *
                * @param pkt a request to send out after a delay
                * @param when tick when response packet should be sent
                */
                void schedTimingReq(PacketPtr pkt, Tick when);

            protected:
                /** When receiving a timing request from the back-end memory media,
                    pass it to the Host. */
                bool recvTimingResp(PacketPtr pkt) override;

                /** When receiving a retry request from the back-end memory media,
                    pass it to the Host. */
                void recvReqRetry() override;
        };

        /** Response port of the CXLType2Accel. */
        CXLResponsePort cxlRspPort;

        /** Request port of the CXLType2Accel. */
        CXLRequestPort memReqPort;

        Tick preRspTick = -1;
    
        struct CXLStats : public statistics::Group
        {
            CXLStats(CXLType2Accel &_ctrl);
    
            statistics::Scalar totalLoadLatency;
            statistics::Scalar reqQueFullEvents;
            statistics::Scalar reqRetryCounts;
            statistics::Scalar rspQueFullEvents;
            statistics::Scalar reqSendFaild;
            statistics::Scalar rspSendFaild;
            statistics::Scalar reqSendSucceed;
            statistics::Scalar rspSendSucceed;
            statistics::Distribution reqQueueLenDist;
            statistics::Distribution rspQueueLenDist;
            statistics::Distribution rspOutStandDist;
            statistics::Distribution reqQueueLatDist;
            statistics::Distribution rspQueueLatDist;
            statistics::Distribution memToCXLCtrlRsp;
        };
    
        CXLStats stats;

    public:
        inline unsigned int getCacheLineSize() const { return cacheLineSize; }

        Port &getPort(const std::string &if_name, PortID idx=InvalidPortID) override;
        AddrRangeList getAddrRanges() const override;

        Addr getPhyAddr(int index);

        Tick read(PacketPtr pkt) override;
        Tick write(PacketPtr pkt) override;

        void init() override;

        void runLSU();
        void stage1();
        void stage2();

        void accessMemory(Addr paddr, int size, bool read, uint8_t *data);
        void sendData(const RequestPtr &req, uint8_t *data, bool read);
        PacketPtr buildPacket(const RequestPtr &req, bool read);
        bool handleReadPacket(PacketPtr pkt);
        bool handleWritePacket();
        void completeDataAccess(PacketPtr pkt);

        using Params = CXLType2AccelParams;
        CXLType2Accel(const Params &p);

    private:
        MemberEventWrapper<&CXLType2Accel::runLSU> runEvent;
        MemberEventWrapper<&CXLType2Accel::stage2> runStage2;
        void recvData(PacketPtr pkt);
        void LSUFinish();
};

} // namespace gem5

#endif // __DEV_X86_CXL_TYPE2_ACCEL_HH__