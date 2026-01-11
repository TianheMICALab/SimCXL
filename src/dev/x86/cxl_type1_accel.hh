#ifndef __DEV_X86_CXL_TYPE1_ACCEL_HH__
#define __DEV_X86_CXL_TYPE1_ACCEL_HH__

#include <vector>

#include "base/addr_range.hh"
#include "base/trace.hh"
#include "base/types.hh"
#include "base/statistics.hh"
#include "mem/packet.hh"
#include "mem/packet_access.hh"
#include "params/CXLType1Accel.hh"
#include "dev/pci/device.hh"


namespace gem5
{

class CXLType1Accel : public PciDevice 
{
    private:
        struct item {
            unsigned long phy;
            int num;
            item() : phy(0), num(0) {}
        };

        class TimingDevicePort : public RequestPort {
            public:
                TimingDevicePort(const std::string& _name, CXLType1Accel* _device)
                    : RequestPort(_name), device(_device),
                    retryRespEvent([this]{ sendRetryResp(); }, name())
                { }
            protected:
            CXLType1Accel* device;

            struct TickEvent : public Event
            {
                PacketPtr pkt;
                CXLType1Accel *device;

                TickEvent(CXLType1Accel *_device) : pkt(NULL), device(_device) {}
                const char *description() const { return "Timing CXL tick"; }
                void schedule(PacketPtr _pkt, Tick t);
            };
            EventFunctionWrapper retryRespEvent;
        };

        class DcachePort : public TimingDevicePort {
            public:

                DcachePort(CXLType1Accel *_device)
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
                DTickEvent(CXLType1Accel *_device)
                    : TickEvent(_device) {}
                void process();
                const char *description() const { return "Timing CXLDevice dcache tick"; }
            };

            DTickEvent tickEvent;

        };

        class IcachePort : public TimingDevicePort {
            public:

                IcachePort(CXLType1Accel *_device)
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

        struct CXLStats : public statistics::Group
        {
            CXLStats(CXLType1Accel &_device);
    
            statistics::Scalar totalLoadLatency;
        };
    
        CXLStats stats;

    public:
        inline unsigned int getCacheLineSize() const { return cacheLineSize; }

        Port &getPort(const std::string &if_name, PortID idx=InvalidPortID) override;
        AddrRangeList getAddrRanges() const override;

        Addr getPhyAddr(int index);

        Tick read(PacketPtr pkt) override;
        Tick write(PacketPtr pkt) override;

        void runLSU();
        void stage1();
        void stage2();

        void accessMemory(Addr paddr, int size, bool read, uint8_t *data);
        void sendData(const RequestPtr &req, uint8_t *data, bool read);
        PacketPtr buildPacket(const RequestPtr &req, bool read);
        bool handleReadPacket(PacketPtr pkt);
        bool handleWritePacket();
        void completeDataAccess(PacketPtr pkt);

        using Params = CXLType1AccelParams;
        CXLType1Accel(const Params &p);

    private:
        MemberEventWrapper<&CXLType1Accel::runLSU> runEvent;
        MemberEventWrapper<&CXLType1Accel::stage2> runStage2;
        void recvData(PacketPtr pkt);
        void LSUFinish();
};

} // namespace gem5

#endif // __DEV_X86_CXL_TYPE1_ACCEL_HH__