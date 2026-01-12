# Copyright (c) 2021 The Regents of the University of California
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

"""

This script shows an example of running a CXL type 3 memory expander simulation
using the gem5 library with the Classic memory system. It defaults to simulating
a CXL ASIC Device.
This simulation boots Ubuntu 18.04 using KVM CPU cores (switching from Atomic/KVM).
The simulation then switches to TIMING/O3 CPU core to run the benchmark.

Usage
-----

```
scons build/X86/gem5.opt -j21
./build/X86/gem5.opt configs/example/gem5_library/x86-cxl-run.py
```
"""
import argparse
import m5
from gem5.utils.requires import requires
from gem5.components.boards.x86_board import X86Board
from gem5.components.memory.single_channel import DIMM_DDR5_4400, SingleChannelDDR4_3200
from gem5.components.processors.simple_switchable_processor import SimpleSwitchableProcessor
from gem5.components.processors.cpu_types import CPUTypes
from gem5.components.cachehierarchies.classic.private_l1_private_l2_shared_l3_cache_hierarchy import (
    PrivateL1PrivateL2SharedL3CacheHierarchy,
)
from gem5.isas import ISA
from gem5.simulate.simulator import Simulator
from gem5.simulate.exit_event import ExitEvent
from gem5.resources.resource import DiskImageResource, KernelResource

# Check ensures the gem5 binary is compiled to X86.
requires(isa_required=ISA.X86)

# Argument Parsing
parser = argparse.ArgumentParser(description='CXL system parameters.')
parser.add_argument('--is_asic', action='store', type=str, nargs='?', 
                    choices=['True', 'False'], default='True', 
                    help='Choose to simulate CXL ASIC Device or FPGA Device.')
test_choices = [
    'lmbench_cxl.sh', 'lmbench_dram.sh', 
    'merci_dram.sh', 'merci_cxl.sh', 'merci_dram+cxl.sh',
    'stream_dram.sh', 'stream_cxl.sh'
]
parser.add_argument('--test_cmd', type=str, choices=test_choices, 
                    default='lmbench_cxl.sh', help='Choose a test to run.')

parser.add_argument('--num_cpus', type=int, default=1, help='Number of CPUs')
parser.add_argument('--cpu_type', type=str, choices=['TIMING', 'O3'], 
                    default='TIMING', help='CPU type')
parser.add_argument('--cxl_mem_type', type=str, choices=['Simple', 'DRAM'], 
                    default='DRAM', help='CXL memory type')

args = parser.parse_args()

# Setup Classic MESI Three Level Cache Hierarchy
cache_hierarchy = PrivateL1PrivateL2SharedL3CacheHierarchy(
    l1d_size="48kB",
    l1d_assoc=6,
    l1i_size="32kB",
    l1i_assoc=8,
    l2_size="2MB",
    l2_assoc=16,
    l3_size="96MB",
    l3_assoc=48,
)

# Setup system memory and CXL memory
memory = DIMM_DDR5_4400(size="3GB")
cxl_dram = DIMM_DDR5_4400(size="8GB")
if args.is_asic == 'False':
    cxl_dram = SingleChannelDDR4_3200(size="8GB")

# Setup Processor
# Using KVM for fast boot, then switching to Timing/O3
# If KVM cannot be used, you can boot from ATOMIC.
processor = SimpleSwitchableProcessor(
    starting_core_type=CPUTypes.KVM,
    switch_core_type=CPUTypes.O3 if args.cpu_type == 'O3' else CPUTypes.TIMING,
    isa=ISA.X86,
    num_cores=args.num_cpus,
)

# Here we tell the KVM CPU (the starting CPU) not to use perf.
for proc in processor.start:
    proc.core.usePerf = False

# Here we setup the board and CXL device memory size. The X86Board allows for Full-System X86 simulations.
board = X86Board(
    clk_freq="2.4GHz",
    processor=processor,
    memory=memory,
    cache_hierarchy=cache_hierarchy,
    cxl_memory=cxl_dram,
    is_asic=(args.is_asic == 'True')
)

# Here we set the Full System workload.
# The `set_kernel_disk_workload` function for the X86Board takes a kernel, a
# disk image, and, optionally, a command to run.

# This is the command to run after the system has booted. The first `m5 exit`
# will stop the simulation so we can switch the CPU cores from KVM/ATOMIC to 
# TIMING/O3 and continue the simulation to run the command. After simulation
# has ended you may inspect `m5out/board.pc.com_1.device` to see the echo
# output.
command = (
    "m5 exit;"
    + "numactl -H;"
    + "m5 resetstats;"
    # + "/home/cxl_benchmark/" + args.test_cmd + ";"
    + "numactl -N 0 -m 1 /home/test_code/simple_test;"
)

# Please modify the paths of kernel and disk_image according to the location of your files.
board.set_kernel_disk_workload(
    kernel=KernelResource(local_path='/home/xxx/code/fs_image/vmlinux'),
    disk_image=DiskImageResource(local_path='/home/xxx/code/fs_image/parsec.img'),
    readfile_contents=command,
)

simulator = Simulator(
    board=board,
    on_exit_event={
        ExitEvent.EXIT: (func() for func in [processor.switch])
    },
)

print("Running the simulation Classic MESI Three Level protocol...")
print("Using KVM cpu for boot")

m5.stats.reset()

simulator.run()
