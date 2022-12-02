/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include <benchmark/benchmark.h>
#include <utility.hpp>

#include <qpl/qpl.h>
#include "../include/cmd_decl.hpp"
#include "dispatcher/hw_dispatcher.hpp"

#if defined( __linux__ )
#include <sys/utsname.h>
#endif
#include <fstream>
#include <memory>
#include <mutex>
#include <stdarg.h>


namespace bench::details
{
//
// Utilities implementations
//
registry_t& get_registry()
{
    static registry_t reg;
    return reg;
}

constexpr const uint64_t poly = 0x04C11DB700000000;

static bool init_hw()
{
    uint32_t size;

    qpl_status status = qpl_get_job_size(qpl_path_hardware, &size);
    if (status != QPL_STS_OK)
        throw std::runtime_error("hw init failed in qpl_get_job_size");

    std::unique_ptr<std::uint8_t[]> job_buffer(new std::uint8_t[size]);

    qpl_job *job = reinterpret_cast<qpl_job*>(job_buffer.get());
    status = qpl_init_job(qpl_path_hardware, job);
    if (status != QPL_STS_OK)
        throw std::runtime_error("hw init failed in qpl_init_job");

    int data = 0;
    job->next_in_ptr  = (std::uint8_t*)&data;
    job->available_in = 4;
    job->op           = qpl_op_crc64;
    job->crc64_poly   = poly;

    status = qpl_submit_job(job);
    if(status != QPL_STS_OK)
        throw std::runtime_error("hw init failed in qpl_submit_job");

    status = qpl_wait_job(job);
    if(status != QPL_STS_OK)
        throw std::runtime_error("hw init failed in qpl_wait_job");

    status = qpl_fini_job(job);
    if (status != QPL_STS_OK)
        throw std::runtime_error("hw init failed in qpl_fini_job");

    return true;
}

static inline int get_num_devices(std::uint32_t numa) noexcept
{
    auto &disp = qpl::ml::dispatcher::hw_dispatcher::get_instance();
    int counter = 0;
    for(auto &device : disp)
    {
        /*
         * the purpose of the check below is to ensure that job would be
         * launched on the device requested by user, meaning
         * if user specified device_numa_id, we check that the program is
         * indeed run on the requested NUMA node
         *
         * explanation regarding (device.numa_id() != (uint64_t)(-1)):
         * accfg_device_get_numa_node() at sources/middle-layer/dispatcher/hw_device.cpp
         * currently returns -1 in case of VM and/or when NUMA is not configured,
         * here is the temporary w/a, so that we don't exit in this case,
         * but just use current device
         *
         * @todo address w/a and remove (device.numa_id() != (uint64_t)(-1)) check
         */
        if(device.numa_id() == numa) {
             counter++;
        }
        else if (((device.numa_id() != (uint64_t)numa)) && (device.numa_id() == (uint64_t)(-1))) {
            counter++;
        }
    }
    return counter;
}

int get_current_numa_accels() noexcept
{
    std::uint32_t tsc_aux = 0;
    __rdtscp(&tsc_aux);
    std::uint32_t numa = static_cast<uint32_t>(tsc_aux >> 12);

    return get_num_devices(numa);
}

const extended_info_t& get_sys_info()
{
    static extended_info_t info;
    static bool is_setup{false};
    static std::mutex guard;

    guard.lock();
    if(!is_setup)
    {
#if defined( __linux__ )
        utsname uname_buf;
        uname(&uname_buf);
        info.host_name = uname_buf.nodename;
        info.kernel    = uname_buf.release;

        std::ifstream info_file("/proc/cpuinfo");
        if(!info_file.is_open())
            throw std::runtime_error("Failed to open /proc/cpuinfo");

        std::string line;
        while (std::getline(info_file, line))
        {
            if (line.empty())
                continue;
            auto del_index = line.find(':');
            if(del_index == std::string::npos)
                continue;
            auto key = line.substr(0, del_index);
            auto val = line.substr(del_index+1);
            trim(key);
            trim(val);

            // Start of descriptor
            if(key == "processor")
                info.cpu_logical_cores++;
            else if(key == "physical id")
                info.cpu_sockets = std::max(info.cpu_sockets, (std::uint32_t)atoi(val.c_str())+1);
            else if(!info.cpu_physical_per_socket && key == "cpu cores")
                info.cpu_physical_per_socket = std::max(info.cpu_physical_per_socket, (std::uint32_t)atoi(val.c_str()));
            else if(!info.cpu_model_name.size() && key == "model name")
                info.cpu_model_name = val;
            else if(!info.cpu_model && key == "model")
                info.cpu_model = atoi(val.c_str());
            else if(!info.cpu_microcode && key == "microcode")
                info.cpu_microcode = strtol(val.c_str(), NULL, 16);
            else if(!info.cpu_stepping && key == "stepping")
                info.cpu_stepping = atoi(val.c_str());
        }

        constexpr std::uint32_t clusters_per_socket = 4; // How to get this dynamically?
        info.cpu_physical_cores       = info.cpu_physical_per_socket*info.cpu_sockets;
        info.cpu_physical_per_cluster = info.cpu_physical_per_socket/clusters_per_socket;

        for(std::uint32_t i = 0; i < info.cpu_sockets; ++i)
        {
            auto devices = get_num_devices(i);
            info.accelerators.total_devices += devices;
            info.accelerators.socket.push_back(devices);
        }

        printf("== Host:   %s\n", info.host_name.c_str());
        printf("== Kernel: %s\n", info.kernel.c_str());
        printf("== CPU:    %s (%d)\n", info.cpu_model_name.c_str(), info.cpu_model);
        printf("  --> Microcode: 0x%x\n", info.cpu_microcode);
        printf("  --> Stepping:  %d\n", info.cpu_stepping);
        printf("  --> Logical:   %d\n", info.cpu_logical_cores);
        printf("  --> Physical:  %d\n", info.cpu_physical_cores);
        printf("  --> Socket:    %d\n", info.cpu_physical_per_socket);
        printf("  --> Cluster:   %d\n", info.cpu_physical_per_cluster);
        printf("== Accelerators: %d\n", info.accelerators.total_devices);
        for(std::uint32_t i = 0; i < info.accelerators.socket.size(); ++i)
        {
            printf("  --> NUMA %d: %d\n", i, info.accelerators.socket[i]);
        }
#endif
        is_setup = true;
    }
    guard.unlock();

    return info;
}
}

//
// GBench command line extension
//
namespace bench::cmd
{
BM_DEFINE_string(block_size, "-1");
BM_DEFINE_int32(queue_size, 0);
BM_DEFINE_int32(batch_size, 0);
BM_DEFINE_int32(threads, 0);
BM_DEFINE_int32(node, -1);
BM_DEFINE_string(dataset, "");
BM_DEFINE_string(in_mem, "llc");
BM_DEFINE_string(out_mem, "cс_ram");
BM_DEFINE_bool(full_time, false);
BM_DEFINE_bool(no_hw, false);

BM_DEFINE_double(canned_part, -1);
BM_DEFINE_bool(canned_regen, false);

static void print_help()
{
    fprintf(stdout,
            "Common arguments:\n"
            "benchmark [--dataset=<path>]            - path to generic dataset\n"
            "          [--block_size=<size>]         - process input data by blocks\n"
            "          [--queue_size=<size>]         - amount of tasks for single device\n"
            "          [--batch_size=<size>]         - amount of operations in a single batch\n"
            "          [--threads=<num>]             - number of threads for asynchronous measurements\n"
            "          [--node=<num>]                - force specific numa node for the task\n"
            "          [--in_mem=<location>]         - input memory location: cache, llc (default), ram.\n"
            "          [--out_mem=<location>]        - output memory location: cache_ram (default), ram\n"
            "          [--full_time]                 - measure library specific task initialization and destruction\n"
            "          [--no_hw]                     - run only software implementations\n"

            "\nCompression/decompression arguments:\n"
            "benchmark [--canned_part=<num>]         - amount of data used for tables generation:\n"
            "                                          0 - full file; (0-1) - portion of file. [1-N] - number of blocks\n"
            "          [--canned_regen]              - regen tables for each part\n"

            "\nDefault benchmark arguments:\n");
}

static void parse_local(int* argc, char** argv)
{
    for(int i = 1; argc && i < *argc; ++i)
    {
        if(benchmark::ParseStringFlag(argv[i],  "dataset",      &FLAGS_dataset) ||
           benchmark::ParseStringFlag(argv[i],  "block_size",   &FLAGS_block_size) ||
           benchmark::ParseInt32Flag(argv[i],   "threads",      &FLAGS_threads) ||
           benchmark::ParseInt32Flag(argv[i],   "node",         &FLAGS_node) ||
           benchmark::ParseBoolFlag(argv[i],    "full_time",    &FLAGS_full_time) ||
           benchmark::ParseInt32Flag(argv[i],   "queue_size",   &FLAGS_queue_size) ||
           benchmark::ParseInt32Flag(argv[i],   "batch_size",   &FLAGS_batch_size) ||
           benchmark::ParseBoolFlag(argv[i],    "no_hw",        &FLAGS_no_hw) ||
           benchmark::ParseStringFlag(argv[i],  "in_mem",       &FLAGS_in_mem) ||
           benchmark::ParseStringFlag(argv[i],  "out_mem",      &FLAGS_out_mem) ||

           benchmark::ParseDoubleFlag(argv[i],  "canned_part",  &FLAGS_canned_part) ||
           benchmark::ParseBoolFlag(argv[i],    "canned_regen", &FLAGS_canned_regen))
        {
            for(int j = i; j != *argc - 1; ++j)
                argv[j] = argv[j + 1];

            --(*argc);
            --i;
        }
        else if (benchmark::IsFlag(argv[i], "help"))
            print_help();
    }
}

std::int32_t get_block_size()
{
    static std::int32_t block_size = -1;
    if(block_size < 0)
    {
        auto str = FLAGS_block_size;
        std::transform(str.begin(), str.end(), str.begin(), ::toupper);

        std::int32_t mult = 1;
        if((str.size() > 2 && str.find("KB", str.size()-2) == str.size()-2) || (str.size() > 1 && str.find("K", str.size()-1) == str.size()-1))
            mult = 1024;
        else if((str.size() > 2 && str.find("MB", str.size()-2) == str.size()-2) || (str.size() > 1 && str.find("M", str.size()-1) == str.size()-1))
            mult = 1024*1024;

        block_size = std::atoi(str.c_str());
        if(block_size == 0 && str != "0")
            throw std::runtime_error("invalid block size format");
        block_size *= mult;
    }
    return block_size;
}

mem_loc_e get_in_mem()
{
    static mem_loc_e mem = (mem_loc_e)-1;
    if((std::int32_t)mem < 0)
    {
        auto str = FLAGS_in_mem;
        std::transform(str.begin(), str.end(), str.begin(), ::tolower);
        if(str == "cache")
            mem = mem_loc_e::cache;
        else if(str == "llc")
            mem = mem_loc_e::llc;
        else if(str == "ram")
            mem = mem_loc_e::ram;
        else if(str == "pmem")
            mem = mem_loc_e::pmem;
        else
            throw std::runtime_error("invalid input memory location");
    }
    return mem;
}

mem_loc_e get_out_mem()
{
    static mem_loc_e mem = (mem_loc_e)-1;
    if((std::int32_t)mem < 0)
    {
        auto str = FLAGS_out_mem;
        std::transform(str.begin(), str.end(), str.begin(), ::tolower);
        if(str == "ram")
            mem = mem_loc_e::ram;
        else if(str == "pmem")
            mem = mem_loc_e::pmem;
        else if(str == "cс_ram")
            mem = mem_loc_e::cc_ram;
        else if(str == "сс_pmem")
            mem = mem_loc_e::cc_pmem;
        else
            throw std::runtime_error("invalid output memory location");
    }
    return mem;
}
}

namespace bench
{
std::string format(const char *format, ...) noexcept
{
    std::string out;
    size_t      size;

    va_list argptr1, argptr2;
    va_start(argptr1, format);
    va_copy(argptr2, argptr1);
    size = vsnprintf(NULL, 0, format, argptr1);
    va_end(argptr1);

    out.resize(size+1);
    vsnprintf(out.data(), out.size(), format, argptr2);
    va_end(argptr2);
    out.resize(out.size()-1);

    return out;
}
}

//
// Main
//

int main(int argc, char** argv)
{
    bench::cmd::parse_local(&argc, argv);
    ::benchmark::Initialize(&argc, argv);
    if (::benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;
    bench::details::get_sys_info();

    if(!bench::cmd::FLAGS_no_hw)
        bench::details::init_hw();

    auto &registry = bench::details::get_registry();
    for(auto &reg : registry)
        reg();
    ::benchmark::RunSpecifiedBenchmarks();
    ::benchmark::Shutdown();
    return 0;
}
