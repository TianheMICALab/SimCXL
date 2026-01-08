from m5.params import *
from m5.objects.PciDevice import *
from m5.objects.XBar import CXLMemBar


class CXLMemCtrl(PciDevice):
    type = 'CXLMemCtrl'
    cxx_header = "dev/x86/cxl_mem_ctrl.hh"
    cxx_class = 'gem5::CXLMemCtrl'

    cxl_rsp_port = ResponsePort(
        "This port sends responses to and receives requests from the Host"
    )
    mem_req_port = RequestPort(
        "This port sends requests to and receives responses from the back-end memory media"
    )

    rsp_size = Param.Unsigned(48, "The number of responses to buffer")
    req_size = Param.Unsigned(48, "The number of requests to buffer")
    
    proto_proc_lat = Param.Latency("15ns", "Latency of the CXL controller processing CXL.mem sub-protocol packets")
    cxl_mem_range = Param.AddrRange("2GB", "CXL expander memory range that can be identified as system memory")

    VendorID = 0x8086
    DeviceID = 0X7890
    Command = 0x0
    Status = 0x280
    Revision = 0x0
    ClassCode = 0x05
    SubClassCode = 0x00
    ProgIF = 0x00
    InterruptLine = 0x1f
    InterruptPin = 0x01

    # Primary
    BAR0 = PciMemBar(size='2GB')
    BAR1 = PciMemUpperBar()
    BAR2 = PciMemBar(size="2MiB")
    BAR3 = PciMemUpperBar()
    BAR4 = PciMemBar(size="512KiB")
    BAR5 = PciMemUpperBar()

    def connectMemory(self, cxl_mem_range, cxl_dram):
        self.cxl_mem_range = cxl_mem_range
        self.BAR0.size = cxl_dram.get_size_str()
        self.cxl_mem_bus = CXLMemBar()
        self.cxl_mem_bus.cpu_side_ports = self.mem_req_port
        for _, port in cxl_dram.get_mem_ports():
            self.cxl_mem_bus.mem_side_ports = port

    def configCXL(self, proc_lat, queue_size):
        self.proto_proc_lat = proc_lat
        self.rsp_size = queue_size
        self.req_size = queue_size

class CXLType1Accel(PciDevice):
    type = 'CXLType1Accel'
    cxx_header = "dev/x86/cxl_type1_accel.hh"
    cxx_class = 'gem5::CXLType1Accel'

class CXLType2Accel(PciDevice):
    type = 'CXLType2Accel'
    cxx_header = "dev/x86/cxl_type2_accel.hh"
    cxx_class = 'gem5::CXLType2Accel'