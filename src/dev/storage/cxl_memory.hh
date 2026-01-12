#include "base/addr_range.hh"
#include "base/trace.hh"
#include "base/types.hh"
#include "mem/packet.hh"
#include "mem/packet_access.hh"
#include "params/CXLMemory.hh"
#include "dev/pci/device.hh"

namespace gem5
{
    class CXLMemory : public PciDevice {
        private:
        class Memory {
            private:
            AddrRange range;
            uint8_t* pmemAddr = nullptr;
            bool inAddrMap = true;
            const std::string _name = "CXLMemory::Memory";
            CXLMemory& owner;

            public:
            Memory(const AddrRange& range, CXLMemory& owner);
            inline uint8_t* toHostAddr(Addr addr) const { 
                return pmemAddr + addr - range.start();
            }
            const std::string& name() const { return _name; }
            uint64_t size() const { return range.size(); }
            Addr start() const { return range.start(); }
            bool isInAddrMap() const { return inAddrMap; }
            void access(PacketPtr pkt);
            Memory(const Memory& other) = delete;
            Memory& operator=(const Memory& other) = delete;
            ~Memory() { delete pmemAddr; }
        };

        Memory _mem;
        Tick _medium_access_lat;
        Tick _device_proto_proc_lat;

        public:
        Tick read(PacketPtr pkt) override;
        Tick write(PacketPtr pkt) override;

        AddrRangeList getAddrRanges() const override;

        Tick process_cxl_mem(PacketPtr ptk);
        using Param = CXLMemoryParams;
        CXLMemory(const Param &p);
    };
} // namespace gem5
