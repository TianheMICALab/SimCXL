# CXL-DMSim

CXL-DMSim is a full-system CXL disaggregated memory simulator based on gem5.

## Requirements

To build CXL-DMSim, you need to satisfy the dependencies of gem5 version 23.10. We recommend running on Ubuntu 20.04 or 22.04.

- gcc 9.4.0+
- SCons 3.0+
- Python 3.6+

The main website can be found at [gem5.org](http://www.gem5.org/).

A good starting point is [Introduction to gem5](http://www.gem5.org/Introduction), and for more information about building the simulator and getting started, please see the [Documentation](http://www.gem5.org/Documentation) and [Tutorials](http://www.gem5.org/Tutorials).

## Setup on Ubuntu 20.04 or 22.04

```bash
sudo apt install build-essential git m4 scons zlib1g zlib1g-dev \
    libprotobuf-dev protobuf-compiler libprotoc-dev libgoogle-perftools-dev \
    python3-dev python-is-python3 libboost-all-dev pkg-config libpng-dev
```

## Building CXL-DMSim

1. Once you have successfully installed all of the necessary dependencies, you can clone the CXL-DMSim repository to begin working with it.

```bash
git clone https://github.com/ferry-hhh/CXL-DMSim.git
```

2. When building CXL-DMSim, there are multiple binary types that can be created. Just like in gem5, the options are opt, fast, and debug. In general, the opt version is the best in most circumstances. 

   Additionally, CXL-DMSim currently only supports modeling of the x86 architecture.

   Type the following command to build the simulator:

```bash
scons build/X86/gem5.opt -j`nproc`
```

Note that the `-j` flag is optional and allows for parallelization of compilation with `nproc` specifying the number of threads. A single-threaded compilation from scratch can take up to 2 hours on some systems. We therefore strongly advise allocating more threads if possible.

## Using CXL-DMSim

CXL-DMSim is a full-system simulator based on gem5. To start the full system, two files are required: a Linux Kernel and a Disk Image.

- **Linux Kernel**: In full-system mode, gem5 boots the operating system by loading a compiled Linux Kernel, which includes necessary resource management modules and device drivers. We provide a [Linux Kernel](https://drive.google.com/drive/folders/1sxZBsedT19ntJdzN8MTkcbczMGXNXgrM?usp=sharing) file with added drivers for CXL devices, version 5.4.49.
- **Disk Image**: In full-system mode, the simulator relies on the Disk Image to run the simulation programs. Users can copy the executable programs to be tested into the Disk Image for execution after the full system starts. We provide a [Disk Image](https://drive.google.com/drive/folders/1sxZBsedT19ntJdzN8MTkcbczMGXNXgrM?usp=sharing) that includes the LMbench, Stream, Viper, and Meric test programs.

Both of these files can be created or compiled following the [gem5 documentation](https://www.gem5.org/documentation/general_docs/fullsystem/disks), or downloaded from the [gem5 resources repository](https://resources.gem5.org/).

1. Please check that the paths for `Kernel` and `disk_image` in `configs/example/gem5_library/x86-cxl-run.py` correspond to the paths on your computer.

2. Enter the following command to start the full system. By default, it allocates CXL-ASIC Device memory to run the lmbench latency benchmark test. The results of the program execution are saved by default in `m5out/board.pc.com_1.device`.

```bash
build/X86/gem5.opt configs/example/gem5_library/x86-cxl-run.py
```

For more configurable system component parameters, please refer to the [gem5 standard library](https://www.gem5.org/documentation/gem5-stdlib/overview). You can also use `x86-cxl-run.py` as a reference to customize your system configuration file.

3. Install `m5term`.

   `m5term` allows users to connect to the simulated terminal provided by the full system. Follow these steps:

```bash
cd util/term
make
sudo install -o root -m 555 m5term /usr/local/bin
```

4. After starting the full system, enter the terminal with the following command:

```bash
m5term localhost 3456
```

## Managing CXL Memory in the Full System

CXL Device is recognized as a CPU-less NUMA node in the system. We provide two ways to manage CXL memory allocation.

1. **Via numactl**

   Users can allocate CXL memory using standard numactl commands, for example:

   ```bash
   numactl --membind=1 --cpunodebind=0 ./lat_mem_rd -t -N 2 1024 64
   ```

   This command will run the `lat_mem_rd` program on CPU0, with memory allocation bound to NUMA node 1, which is the CXL memory in the system. You can also use options like `interleave` and `preferred` to mix DRAM and CXL memory.

2. **Via mmap/memkind**

   The CXL device driver provides an mmap system call to allocate CXL memory.

## Citation
If you find CXL-DMSim useful for your own work, please cite our paper as follows.
```tex
@ARTICLE{11153390,
  author={Wang, Yanjing and Wu, Lizhou and Hong, Wentao and Ou, Yang and Wang, Zicong and Gao, Sunfeng and Zhang, Jie and Ma, Sheng and Dong, Dezun and Qi, Xingyun and Lai, Mingche and Xiao, Nong},
  journal={IEEE Transactions on Computer-Aided Design of Integrated Circuits and Systems}, 
  title={CXL-DMSim: A Full-System CXL Disaggregated Memory Simulator With Comprehensive Silicon Validation}, 
  year={2025},
  volume={},
  number={},
  pages={1-1},
  doi={10.1109/TCAD.2025.3607145},
  url={https://ieeexplore.ieee.org/document/11153390}
}
```
