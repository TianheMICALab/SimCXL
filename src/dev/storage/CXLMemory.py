from m5.params import *
from m5.objects.PciDevice import *


class CXLMemory(PciDevice):
    type = 'CXLMemory'
    cxx_header = "dev/storage/cxl_memory.hh"
    cxx_class = 'gem5::CXLMemory'
    medium_access_lat = Param.Latency('50ns', "Latency of accessing the memory medium of the CXL expander backend")
    device_proto_proc_lat = Param.Latency('15ns', "Latency of the CXL controller processing CXL.mem sub-protocol packets")

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
    BAR0 = PciMemBar(size='4GB')
    BAR1 = PciMemUpperBar()
