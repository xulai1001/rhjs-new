#ifndef _LIB_HAMMER
#define _LIB_HAMMER

/*
    libhammer - viktor's rowhammer toolset, compact version
    
    repo: https://github.com/xulai1001/rhjs-new

    ISLAB 2018-2021
*/

#include "iostream"
#include "sstream"
#include "cstdio"
#include "cstdlib"
#include "cstring"
#include "vector"
#include "map"
#include "set"
#include "string"
#include "memory"
#include "algorithm"

extern "C" {
    #include "unistd.h"
    #include "errno.h"
    #include "stdint.h"
    #include "fcntl.h"
    #include "sys/mman.h"
    #include "time.h"
    #include "linux/types.h"
    #include "sys/stat.h"
    #include "sys/sysinfo.h"
    #include "sys/ipc.h"
    #include "sys/shm.h"
    #include "sys/wait.h"
    #include "sys/types.h"
    #include "sched.h"
    #include "sigsegv.h"
}

using namespace std;

//---------------------------------
// constants
#define PAGE_SIZE 0x1000
#define PAGE_MASK 0xfff
#define PAGE_SHIFT 12
#define PAGE_FLAG 0
#define HUGE_SIZE 0x200000ull
#define HUGE_MASK 0x1fffffull
#define HUGE_SHIFT 21
#define HUGE_FLAG 0x40000
#define PFN_MASK ((1ull << 55) - 1)
#define ALLOC_SIZE PAGE_SIZE
#define ROW_CONFLICT_THRESHOLD 233

//---------------------------------
// typedefs
typedef uint64_t clk_t;

struct myclock {
    // clock_gettime vars
    clk_t ns;
    int type;
    struct timespec t0, t1;
    // rdtsc vars
    clk_t ticks, r0, r1;
};

//---------------------------------
// utils
#ifndef ASSERT
    #define ASSERT(line) if (!(line)) { fprintf(stderr, #line, strerror(errno)); exit(-1); }
#endif

//-- ASM.H --
#define HAMMER(a, b) __asm__ __volatile__( \
    "movq (%0), %%rax \n\t" \
    "movq (%1), %%rax \n\t" \
    "clflush (%0) \n\t" \
    "clflush (%1) \n\t" \
    : \
    :"r"(a), "r"(b) \
    :"rax", "memory")

// r/w barrier
#define MFENCE __asm__ __volatile__(\
    "mfence \n\t" \
    : : :"memory")

// read barrier. (in this way?)
#define LFENCE __asm__ __volatile__(\
    "lfence \n\t" \
    : : :"memory")

// read access (64-bit)
#define MOV(a) __asm__ __volatile__( \
    "movq (%0), %%rax \n\t" \
    : :"r"(a):"rax")

#define MOV2(a, b) __asm__ __volatile__( \
    "movq (%0), %%rax \n\t" \
    "movq (%1), %%rax \n\t" \
    : :"r"(a), "r"(b):"rax")

#define MOV4(a, b, c, d) __asm__ __volatile__( \
    "movq (%0), %%rax \n\t" \
    "movq (%1), %%rax \n\t" \
    "movq (%2), %%rax \n\t" \
    "movq (%3), %%rax \n\t" \
    : :"r"(a), "r"(b), "r"(c), "r"(d):"rax")

#define CLFLUSH(a) __asm__ __volatile__(\
    "clflush (%0) \n\t" \
    : :"r"(a))

#define CLFLUSH2(a, b) __asm__ __volatile__(\
    "clflush (%0) \n\t" \
    "clflush (%1) \n\t" \
    : :"r"(a), "r"(b))

#define PREFETCH_L2(a) __asm__ __volatile__(\
    "prefetcht1 (%0) \n\t" \
    : :"r"(a))

#define PREFETCH_L3(a) __asm__ __volatile__(\
    "prefetcht2 (%0) \n\t" \
    "prefetchnta (%0) \n\t" \
    : :"r"(a))

//-- TIMING.H -- 
#define START_CLOCK(cl, tp) cl.type = tp; clock_gettime(cl.type, &cl.t0)
#define END_CLOCK(cl) clock_gettime(cl.type, &cl.t1); \
    cl.ns = (cl.t1.tv_sec - cl.t0.tv_sec) * 1000000000ul + (cl.t1.tv_nsec - cl.t0.tv_nsec)

// intel guide
// use cpuid as barrier before, cpuid will clobber(use) all registers
// shl/or joins eax/edx to 64-bit long
// no output vars
#define START_TSC(cl) __asm__ __volatile__ ( \
   "cpuid \n\
    rdtsc \n\
    shlq $32, %%rdx \n\
    orq %%rdx, %%rax" \
    : "=a"(cl.r0) \
    : \
    : "%rbx", "%rcx", "%rdx")

// shl/or joins eax/edx, then mov to var
// use cpuid as barrier after, will clobber all registers
// no output vars
// =g lets gcc decide howto deal with var
#define END_TSC(cl) __asm__ __volatile__ ( \
   "rdtscp \n\
    shlq $32, %%rdx \n\
    orq %%rdx, %%rax \n\
    movq %%rax, %0 \n\
    cpuid" \
    : "=g"(cl.r1) \
    : \
    : "%rax", "%rbx", "%rcx", "%rdx"); \
    cl.ticks = cl.r1 - cl.r0

// lightweight rdtsc
// reg is a 32-bit register var, used to store delta ticks
#define START_TSC_LT register int _tsc asm ("r12"); \
                     __asm__ __volatile__ ( \
    "lfence \n\t" \
    "rdtsc \n\t" \
    "movl %%eax, %0" \
    :"=r"(_tsc)::"%rax", "%rdx")

#define END_TSC_LT __asm__ __volatile__ (\
    "lfence \n\t" \
    "rdtsc \n\t" \
    "subl %0, %%eax \n\t" \
    "movl %%eax, %0" \
    :"=r"(_tsc)::"%rax", "%rdx")

//---------------------------------
// classes
class Page
{
public:
    char *ptr;
    shared_ptr<char> v;
    uint64_t p, shmid;
    bool locked;
    static uint64_t shm_index, release_count;

private:
    // invoked by shared_ptr deleter
    static void _release(char *p);

public:
    Page() : p(0), shmid(0), locked(false) {}
    ~Page();

    void acquire();
    void acquire_shared(uint64_t sid=0);
    void reset();       // explicitly release a page
    void lock();
    void unlock();
    bool operator<(Page &b);
    bool operator==(Page &b);
    string inspect();

    template <typename T>
    T & get(size_t x)
    {
        return *(T *)(v.get()+x);
    }

    void fill(uint8_t x=0xff)
    {
        memset(v.get(), x, PAGE_SIZE);
    }

    void wrap()
    {
        v = shared_ptr<char>(ptr, this->_release);
        //v.reset(ptr, this->_release);
    }

    vector<int> check_bug(uint8_t good=0xff);
};

class AddrMap
{
public:
    map<void *, uint64_t> v2p_map;
    map<uint64_t, void *> p2v_map;
    map<uint64_t, Page> page_map;
public:

    void clear() { v2p_map.clear(); p2v_map.clear(); page_map.clear(); }
    void add(Page &pg);
    void add(vector<Page> &pageset);
    void add_pagemap(vector<Page> &pageset);    // use with caution. for this will add reference to shared_ptr

    void *p2v(uint64_t p)
    {
        uint64_t offset = p & 0xfff;
        uint64_t base = p - offset;
        void *ret = 0;
        if (p2v_map.count(base)) ret = p2v_map[base] + offset;
       // cout << "p2v " << hex << p << " -> " << ret << endl;
        return ret;
    }

    uint64_t v2p(void *v)
    {
        uint64_t offset = (uint64_t)v & 0xfff;
        uint64_t base = v - offset, ret = 0;
        if (v2p_map.count((void *)base)) ret = v2p_map[(void *)base] + offset;
       // cout << "v2p " << hex << v << " -> " << ret << endl;
        return ret;
    }

    bool has_pa(uint64_t pa)
    {
        return (bool)p2v_map.count(pa);
    }
    bool has_va(void *va)
    {
        return (bool)v2p_map.count(va);
    }
};

// defined in utils.cpp
struct ImageFile {
    string name;
    size_t sz;
    char *image;
};

class DiskUsage
{
public:
    myclock clk, clk_total;
    uint64_t value, usage;
    string disk;

private:
    void get_diskstat();

public:
    void init(string d);
    void update();
};

struct HammerResult
{
    uint64_t base, offset, p, q;
    unsigned value, flip_to, flips_page, flips_row;

    HammerResult() {};
    HammerResult(const string& s);
    void print_header();
    void print();
};

struct BinaryInfo
{
    string path;
    uint64_t offset;
    uint8_t orig, target, flip_to;
};

//---------------------------------
// externs
#ifndef _LIBHAMMER_EXTERNS
extern int fd_pagemap;
extern clk_t clk_freq;
extern AddrMap addrmap;
extern map<int, vector<HammerResult> > result_pool;
#endif

//---------------------------------
// functions
uint64_t v2p(void *v);
// hammer function. returns operation time (ticks)
uint64_t hammer_loop(void *va, void *vb, int n, int delay);
uint64_t hammer_loop_mfence(void *va, void *vb, int n, int delay);
// equivalent to usenix 16 obf latency routine
uint64_t hammer_latency(void *va, void *vb);
int is_conflict(void *va, void *vb);
// baseline test
clk_t clock_overhead(int type);
clk_t tsc_overhead(void);
clk_t tsc_measure_freq(void);
clk_t tsc_to_ns(clk_t ticks);
// get mem/cpu info
uint64_t get_mem_size();
uint64_t get_meminfo(const string &key);
uint64_t get_cached_mem();
uint64_t get_available_mem();
void set_cpu_affinity(int x);
string run_cmd(const char *cmd);
string get_cpu_model();
void continuation(void *a, void *b, void *c);
uint64_t get_binary_pa(const string &path, uint64_t offset=0);
// unused! //void waylaying();
void relocate_fadvise(const string& path);
uint64_t to_mb(uint64_t b);
// unused! //bool is_paddr_available(uint64_t pa);
uint64_t v2p_once(void *v);
// sending ctrl-c to child threads
void interrupt(int sig);
void interrupt_child(int sig);
// unused! //ImageFile do_chasing(const string &path, uint64_t addr=0);

// page allocation
vector<Page> allocate_mb(int mb);
vector<Page> get_contiguous_aligned_page(vector<Page> & pageset);
vector<Page> allocate_cap(int pageset_mb);
void release_pageset(vector<Page> & pageset);
void load_hammer_result(const string& fname);
vector<HammerResult> find_template(const BinaryInfo &info);
vector<HammerResult> find_flips(uint64_t p, uint64_t q);

#endif
