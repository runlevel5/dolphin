// A test program for Dolphin's CPU cores. It runs many instructions on pseudo-random and edge
// case inputs, and reports a hash of every result, XER, CR and FPSCR value for each group of
// tests, so that running it on two CPU cores and comparing the reports finds any difference.
//
// Reports go through OSReport, which Dolphin replaces with a printf to its log in debug mode.

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef int s32;
typedef unsigned long long u64;

// noipa: Dolphin reads the arguments, so the compiler must not assume that this function doesn't.
void OSReport(const char* format, ...) __attribute__((noipa, used));
void OSReport(const char* format, ...)
{
  asm volatile("" : : : "memory");
}

static u32 rng_state = 0x12345678;
static u32 Random(void)
{
  u32 x = rng_state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  return rng_state = x;
}

// Edge cases half of the time, random values otherwise.
static u32 Value(void)
{
  static const u32 edge[] = {0,      1,      2,       0x7FFFFFFF, 0x80000000, 0xFFFFFFFF,
                             0xFFFFFFFE, 0x8000, 0x7FFF, 0xFFFF,     0x10000,    31,
                             32,     33,     0x80000001, 0x55555555};
  const u32 r = Random();
  return (r & 1) ? Random() : edge[(r >> 1) % (sizeof(edge) / sizeof(edge[0]))];
}

static u32 hash;
// Each instruction of a group also has its own hash, so that a difference names the instruction.
#define MAX_OPS 64
static u32 op_hashes[MAX_OPS];
static const char* op_names[MAX_OPS];
static u32 op_index, op_count;

static void Begin(void)
{
  hash = 0x811C9DC5;
  for (int i = 0; i < MAX_OPS; i++)
    op_hashes[i] = 0x811C9DC5;
  op_count = 0;
}
static void Mix(u32 value)
{
  hash ^= value;
  hash *= 0x01000193;
  hash ^= hash >> 15;
}
static void Mix64(u64 value)
{
  Mix((u32)(value >> 32));
  Mix((u32)value);
}
static void BeginIteration(void)
{
  op_index = 0;
}
static void Record(const char* name, u32 a, u32 b, u32 c)
{
  u32 h = op_hashes[op_index];
  const u32 values[3] = {a, b, c};
  for (int i = 0; i < 3; i++)
  {
    h ^= values[i];
    h *= 0x01000193;
    h ^= h >> 15;
  }
  op_hashes[op_index] = h;
  op_names[op_index] = name;
  op_index++;
  if (op_index > op_count)
    op_count = op_index;
  Mix(a);
  Mix(b);
  Mix(c);
}
static void Record64(const char* name, u64 a, u64 b, u32 c)
{
  Record(name, (u32)(a >> 32) ^ (u32)a * 3, (u32)(b >> 32) ^ (u32)b * 5, c);
  Mix64(a);
  Mix64(b);
}
static void End(const char* name)
{
  OSReport("TEST %s %08x\n", name, hash);
  for (u32 i = 0; i < op_count; i++)
  {
    // The mnemonic, up to the first space.
    char mnemonic[16];
    u32 length = 0;
    while (length < sizeof(mnemonic) - 1 && op_names[i][length] && op_names[i][length] != ' ')
    {
      mnemonic[length] = op_names[i][length];
      length++;
    }
    mnemonic[length] = 0;
    OSReport("TEST %s/%02u/%s %08x\n", name, i, mnemonic, op_hashes[i]);
  }
}

#define ITERATIONS 2000

// Integer instructions, with XER and CR captured afterwards.
#define OP2(insn)                                                                                  \
  do                                                                                               \
  {                                                                                                \
    u32 r_, x_, c_;                                                                                \
    asm volatile("mtxer %5\n\t" insn " %0,%3,%4\n\tmfxer %1\n\tmfcr %2"                            \
                 : "=&r"(r_), "=&r"(x_), "=&r"(c_)                                                 \
                 : "r"(a), "r"(b), "r"(xer)                                                        \
                 : "cr0", "xer");                                                                  \
    Record(insn, r_, x_, c_ >> 28);                                                                \
  } while (0)
#define OP1(insn)                                                                                  \
  do                                                                                               \
  {                                                                                                \
    u32 r_, x_, c_;                                                                                \
    asm volatile("mtxer %4\n\t" insn " %0,%3\n\tmfxer %1\n\tmfcr %2"                               \
                 : "=&r"(r_), "=&r"(x_), "=&r"(c_)                                                 \
                 : "r"(a), "r"(xer)                                                                \
                 : "cr0", "xer");                                                                  \
    Record(insn, r_, x_, c_ >> 28);                                                                \
  } while (0)
#define OPI(insn, imm) OP1(insn " %0,%3," #imm "\n\t#")
#define RLWIMI(sh, mb, me)                                                                         \
  do                                                                                               \
  {                                                                                                \
    u32 r_ = b, c_;                                                                                \
    asm volatile("rlwimi. %0,%2," #sh "," #mb "," #me "\n\tmfcr %1"                                \
                 : "+r"(r_), "=&r"(c_)                                                             \
                 : "r"(a)                                                                          \
                 : "cr0");                                                                         \
    Record("rlwimi.", r_, c_ >> 28, 0);                                                            \
  } while (0)

static void TestIntegerArithmetic(void)
{
  Begin();
  for (int i = 0; i < ITERATIONS; i++)
  {
    BeginIteration();
    const u32 a = Value(), b = Value();
    const u32 xer = Random() & 0xE000007F;  // SO, OV, CA and the string length
    OP2("add");
    OP2("add.");
    OP2("addo");
    OP2("addo.");
    OP2("addc");
    OP2("addc.");
    OP2("addco");
    OP2("adde");
    OP2("adde.");
    OP2("addeo.");
    OP2("subf");
    OP2("subf.");
    OP2("subfo.");
    OP2("subfc");
    OP2("subfco.");
    OP2("subfe");
    OP2("subfe.");
    OP2("subfeo");
    OP2("mullw");
    OP2("mullw.");
    OP2("mullwo.");
    OP2("mulhw");
    OP2("mulhw.");
    OP2("mulhwu");
    OP2("mulhwu.");
    OP2("divw");
    OP2("divw.");
    OP2("divwo.");
    OP2("divwu");
    OP2("divwu.");
    OP2("divwuo.");
    OP1("neg");
    OP1("neg.");
    OP1("nego.");
    OP1("addze");
    OP1("addze.");
    OP1("addzeo");
    OP1("addme");
    OP1("addme.");
    OP1("subfze.");
    OP1("subfme.");
    OP1("subfmeo");
    OPI("addi", -5);
    OPI("addis", 0x1234);
    OPI("addic", -1);
    OPI("addic.", 100);
    OPI("subfic", 7);
    OPI("subfic", -32768);
    OPI("mulli", -3);
    OPI("mulli", 32767);
  }
  End("integer-arithmetic");
}

static void TestIntegerLogic(void)
{
  Begin();
  for (int i = 0; i < ITERATIONS; i++)
  {
    BeginIteration();
    const u32 a = Value(), b = Value();
    const u32 xer = Random() & 0xE000007F;
    OP2("and");
    OP2("and.");
    OP2("andc.");
    OP2("or");
    OP2("or.");
    OP2("orc.");
    OP2("xor.");
    OP2("nand.");
    OP2("nor.");
    OP2("eqv.");
    OP2("slw");
    OP2("slw.");
    OP2("srw.");
    OP2("sraw");
    OP2("sraw.");
    OP1("extsb.");
    OP1("extsh.");
    OP1("cntlzw");
    OP1("cntlzw.");
    OPI("andi.", 0xF0F0);
    OPI("andis.", 0x8001);
    OPI("ori", 0x8000);
    OPI("oris", 0xFFFF);
    OPI("xori", 0x1234);
    OPI("xoris", 0x8000);
    OPI("srawi", 0);
    OPI("srawi.", 5);
    OPI("srawi.", 31);
  }
  End("integer-logic");
}

#define RLWINM(sh, mb, me) OP1("rlwinm. %0,%3," #sh "," #mb "," #me "\n\t#")
#define RLWNM(mb, me) OP2("rlwnm. %0,%3,%4," #mb "," #me "\n\t#")

static void TestRotates(void)
{
  Begin();
  for (int i = 0; i < ITERATIONS; i++)
  {
    BeginIteration();
    const u32 a = Value(), b = Value();
    const u32 xer = 0;
    RLWINM(0, 0, 31);
    RLWINM(1, 0, 30);
    RLWINM(31, 1, 31);
    RLWINM(16, 16, 31);
    RLWINM(8, 24, 7);  // Wrapping mask
    RLWINM(5, 3, 3);
    RLWINM(0, 31, 0);
    RLWINM(27, 5, 29);
    RLWNM(0, 31);
    RLWNM(4, 27);
    RLWNM(28, 3);
    RLWIMI(0, 0, 31);
    RLWIMI(8, 16, 23);
    RLWIMI(24, 28, 3);
    RLWIMI(31, 0, 0);
  }
  End("rotates");
}

#define CMP(insn)                                                                                  \
  do                                                                                               \
  {                                                                                                \
    u32 c_;                                                                                        \
    asm volatile("mtcrf 0xff,%3\n\tmtxer %4\n\t" insn "\n\tmfcr %0"                                \
                 : "=&r"(c_)                                                                       \
                 : "r"(a), "r"(b), "r"(cr), "r"(xer)                                               \
                 : "cr0", "cr1", "cr2", "cr3", "cr4", "cr5", "cr6", "cr7", "xer");                 \
    Record(insn, c_, 0, 0);                                                                        \
  } while (0)

static void TestCompareAndConditionRegister(void)
{
  Begin();
  for (int i = 0; i < ITERATIONS; i++)
  {
    BeginIteration();
    const u32 a = Value(), b = Value();
    const u32 cr = Random();
    const u32 xer = Random() & 0xE0000000;
    CMP("cmpw 3,%1,%2");
    CMP("cmplw 5,%1,%2");
    CMP("cmpwi 1,%1,-1");
    CMP("cmplwi 7,%1,100");
    CMP("cmpw 0,%1,%2");
    CMP("crand 1,2,31");
    CMP("cror 4,9,30");
    CMP("crxor 5,5,5");
    CMP("crnand 6,7,8");
    CMP("crnor 31,0,15");
    CMP("creqv 12,12,12");
    CMP("crandc 20,21,22");
    CMP("crorc 25,26,27");
    CMP("mcrf 2,6");
    CMP("mcrxr 4");
    CMP("mtcrf 0x24,%1");
    CMP("mtcrf 0x81,%2");
  }
  End("compare-cr");
}

static u8 memory[256] __attribute__((aligned(32)));

static void FillMemory(void)
{
  for (u32 i = 0; i < sizeof(memory); i += 4)
    *(u32*)&memory[i] = Random();
}

static void MixMemory(void)
{
  for (u32 i = 0; i < sizeof(memory); i += 4)
    Mix(*(u32*)&memory[i]);
}

#define LOAD(insn, offset)                                                                         \
  do                                                                                               \
  {                                                                                                \
    u32 r_;                                                                                        \
    asm volatile(insn " %0," #offset "(%1)" : "=r"(r_) : "b"(base) : "memory");                    \
    Record(insn, r_, 0, 0);                                                                        \
  } while (0)
#define LOADX(insn)                                                                                \
  do                                                                                               \
  {                                                                                                \
    u32 r_;                                                                                        \
    asm volatile(insn " %0,%1,%2" : "=r"(r_) : "b"(memory), "r"(index) : "memory");                \
    Record(insn, r_, 0, 0);                                                                        \
  } while (0)
#define LOADU(insn, offset)                                                                        \
  do                                                                                               \
  {                                                                                                \
    u32 r_;                                                                                        \
    u8* p_ = base;                                                                                 \
    asm volatile(insn " %0," #offset "(%1)" : "=r"(r_), "+b"(p_) : : "memory");                    \
    Record(insn, r_, (u32)(p_ - memory), 0);                                                       \
  } while (0)
#define LOADUX(insn)                                                                               \
  do                                                                                               \
  {                                                                                                \
    u32 r_;                                                                                        \
    u8* p_ = memory;                                                                               \
    asm volatile(insn " %0,%1,%2" : "=&r"(r_), "+b"(p_) : "r"(index) : "memory");                  \
    Record(insn, r_, (u32)(p_ - memory), 0);                                                       \
  } while (0)
#define STOREU(insn, offset)                                                                       \
  do                                                                                               \
  {                                                                                                \
    u8* p_ = base;                                                                                 \
    asm volatile(insn " %1," #offset "(%0)" : "+b"(p_) : "r"(value) : "memory");                   \
    Record(insn, (u32)(p_ - memory), 0, 0);                                                        \
  } while (0)
#define STOREUX(insn)                                                                              \
  do                                                                                               \
  {                                                                                                \
    u8* p_ = memory;                                                                               \
    asm volatile(insn " %1,%0,%2" : "+b"(p_) : "r"(value), "r"(index) : "memory");                 \
    Record(insn, (u32)(p_ - memory), 0, 0);                                                        \
  } while (0)
#define STORE(insn, offset)                                                                        \
  asm volatile(insn " %0," #offset "(%1)" : : "r"(value), "b"(base) : "memory")
#define STOREX(insn) asm volatile(insn " %0,%1,%2" : : "r"(value), "b"(memory), "r"(index) : "memory")

static void TestLoadsAndStores(void)
{
  Begin();
  for (int i = 0; i < ITERATIONS; i++)
  {
    BeginIteration();
    FillMemory();
    u8* const base = &memory[64 + (Random() % 64)];
    const u32 index = Random() % 200;
    const u32 value = Value();

    LOAD("lbz", 0);
    LOAD("lbz", -1);
    LOAD("lhz", 3);
    LOAD("lha", 6);
    LOAD("lha", -7);
    LOAD("lwz", 0);
    LOAD("lwz", 13);
    LOADX("lbzx");
    LOADX("lhzx");
    LOADX("lhax");
    LOADX("lwzx");
    LOADX("lhbrx");
    LOADX("lwbrx");
    LOADU("lbzu", 1);
    LOADU("lhzu", -2);
    LOADU("lhau", 4);
    LOADU("lwzu", 8);
    LOADUX("lbzux");
    LOADUX("lhzux");
    LOADUX("lhaux");
    LOADUX("lwzux");

    STORE("stb", 0);
    STORE("sth", 5);
    STORE("stw", -9);
    STOREX("stbx");
    STOREX("sthx");
    STOREX("stwx");
    STOREX("sthbrx");
    STOREX("stwbrx");
    STOREU("stbu", 3);
    STOREU("sthu", -6);
    STOREU("stwu", 10);
    STOREUX("stbux");
    STOREUX("sthux");
    STOREUX("stwux");
    MixMemory();

    // Load and store multiple words and strings, on aligned addresses.
    u32 words[4];
    asm volatile("lmw 28,0(%4)\n\tmr %0,28\n\tmr %1,29\n\tmr %2,30\n\tmr %3,31"
                 : "=r"(words[0]), "=r"(words[1]), "=r"(words[2]), "=r"(words[3])
                 : "b"(&memory[16])
                 : "r28", "r29", "r30", "r31", "memory");
    for (int w = 0; w < 4; w++)
      Mix(words[w]);
    asm volatile("mr 28,%0\n\tmr 29,%1\n\tmr 30,%0\n\tmr 31,%1\n\tstmw 28,0(%2)"
                 :
                 : "r"(value), "r"(index), "b"(&memory[96])
                 : "r28", "r29", "r30", "r31", "memory");
    asm volatile("lswi 28,%4,13\n\tmr %0,28\n\tmr %1,29\n\tmr %2,30\n\tmr %3,31"
                 : "=r"(words[0]), "=r"(words[1]), "=r"(words[2]), "=r"(words[3])
                 : "b"(&memory[33])
                 : "r28", "r29", "r30", "r31", "memory");
    for (int w = 0; w < 4; w++)
      Mix(words[w]);
    asm volatile("mr 28,%0\n\tmr 29,%1\n\tstswi 28,%2,7" : : "r"(value), "r"(index), "b"(&memory[150])
                 : "r28", "r29", "memory");

    // Zero a cache block.
    asm volatile("dcbz 0,%0" : : "b"(&memory[128 + 32 * (Random() % 4)]) : "memory");
    MixMemory();
  }
  End("loads-stores");
}

// Accesses to MMIO, which the JIT's fastmem can't do: the first one faults, and later ones take
// the slow path that the fault handler patched in.
static void TestMMIO(void)
{
  Begin();
  // VI top field base (left), which reads back what was written to it.
  u8* const base = (u8*)0xCC00201C;
  const u32 saved = *(volatile u32*)base;
  for (int i = 0; i < 100; i++)
  {
    BeginIteration();
    u32 value = Random() & 0x00FFFE00;
    STORE("stw", 0);
    LOAD("lwz", 0);
    LOAD("lhz", 0);
    LOAD("lhz", 2);
    LOAD("lha", 2);
    value = Random() & 0xFE00;
    STORE("sth", 2);
    LOAD("lwz", 0);
  }
  *(volatile u32*)base = saved;
  End("mmio");
}

static u32 Fibonacci(u32 n)
{
  return n < 2 ? n : Fibonacci(n - 1) + Fibonacci(n - 2);
}

static u32 F0(u32 x) { return x + 1; }
static u32 F1(u32 x) { return x * 3; }
static u32 F2(u32 x) { return x ^ 0x5A5A; }
static u32 F3(u32 x) { return x >> 3; }
static u32 (*const functions[])(u32) = {F0, F1, F2, F3};

static void TestBranches(void)
{
  Begin();

  // Calls and returns through LR.
  Mix(Fibonacci(22));

  // Indirect calls through CTR.
  u32 x = 1;
  for (int i = 0; i < 10000; i++)
    x = functions[Random() % 4](x);
  Mix(x);

  // A jump table.
  for (int i = 0; i < 10000; i++)
  {
    switch (Random() % 8)
    {
    case 0: x += 7; break;
    case 1: x ^= 0x1234; break;
    case 2: x *= 5; break;
    case 3: x -= 3; break;
    case 4: x = (x << 3) | (x >> 29); break;
    case 5: x += x >> 7; break;
    case 6: x = ~x; break;
    default: x++; break;
    }
  }
  Mix(x);

  // Counter loops, and conditional branches decrementing CTR.
  for (int i = 0; i < 100; i++)
  {
    u32 count = 0, n = Random() % 100 + 1;
    asm volatile("mtctr %1\n1:\taddi %0,%0,1\n\tbdnz 1b" : "+r"(count) : "r"(n) : "ctr");
    Mix(count);
    count = 0;
    asm volatile("mtctr %1\n1:\taddi %0,%0,1\n\tcmpwi %0,50\n\tbdnzf 2,1b"
                 : "+r"(count)
                 : "r"(n)
                 : "ctr", "cr0");
    Mix(count);
    u32 ctr;
    asm volatile("mtctr %1\n1:\taddi %1,%1,0\n\tbdz 2f\n\tb 1b\n2:\tmfctr %0"
                 : "=r"(ctr)
                 : "r"(n)
                 : "ctr");
    Mix(ctr);
  }

  // A long loop, over many timing slices.
  u32 sum = 0;
  for (u32 i = 0; i < 20000000; i++)
    sum += i ^ (sum >> 3);
  Mix(sum);

  End("branches");
}

// More guest registers than the JIT has host registers for, in one block, with branches out of
// it and within it, so that registers are evicted and flushed on conditional exits.
// The last word is the loop count.
static u32 pressure[29];

static void TestRegisterPressure(void)
{
  Begin();
  for (int round = 0; round < 200; round++)
  {
    for (int i = 0; i < 28; i++)
      pressure[i] = Value();
    pressure[28] = Random() % 64 + 1;
    register u32* p asm("r3") = pressure;
    asm volatile(
        "lwz 0,112(3)\n\t"
        "mtctr 0\n\t"
        "lwz 4,0(3)\n\t"
        "lwz 5,4(3)\n\t"
        "lwz 6,8(3)\n\t"
        "lwz 7,12(3)\n\t"
        "lwz 8,16(3)\n\t"
        "lwz 9,20(3)\n\t"
        "lwz 10,24(3)\n\t"
        "lwz 11,28(3)\n\t"
        "lwz 12,32(3)\n\t"
        "lwz 14,36(3)\n\t"
        "lwz 15,40(3)\n\t"
        "lwz 16,44(3)\n\t"
        "lwz 17,48(3)\n\t"
        "lwz 18,52(3)\n\t"
        "lwz 19,56(3)\n\t"
        "lwz 20,60(3)\n\t"
        "lwz 21,64(3)\n\t"
        "lwz 22,68(3)\n\t"
        "lwz 23,72(3)\n\t"
        "lwz 24,76(3)\n\t"
        "lwz 25,80(3)\n\t"
        "lwz 26,84(3)\n\t"
        "lwz 27,88(3)\n\t"
        "lwz 28,92(3)\n\t"
        "lwz 29,96(3)\n\t"
        "lwz 30,100(3)\n\t"
        "lwz 31,104(3)\n\t"
        "lwz 0,108(3)\n\t"
        "1:\n\t"
        "add 4,4,5\n\t"
        "rotlwi 4,4,1\n\t"
        "add 5,5,6\n\t"
        "add 6,6,7\n\t"
        "add 7,7,8\n\t"
        "cmpwi 7,0\n\t"
        "blt 2f\n\t"
        "xori 8,8,0x5a5a\n\t"
        "2:\n\t"
        "add 8,8,9\n\t"
        "rotlwi 8,8,29\n\t"
        "add 9,9,10\n\t"
        "add 10,10,11\n\t"
        "add 11,11,12\n\t"
        "add 12,12,14\n\t"
        "rotlwi 12,12,26\n\t"
        "add 14,14,15\n\t"
        "add 15,15,16\n\t"
        "add 16,16,17\n\t"
        "add 17,17,18\n\t"
        "rotlwi 17,17,23\n\t"
        "cmpwi 17,0\n\t"
        "blt 2f\n\t"
        "xori 18,18,0x5a5a\n\t"
        "2:\n\t"
        "add 18,18,19\n\t"
        "add 19,19,20\n\t"
        "add 20,20,21\n\t"
        "add 21,21,22\n\t"
        "rotlwi 21,21,20\n\t"
        "add 22,22,23\n\t"
        "add 23,23,24\n\t"
        "add 24,24,25\n\t"
        "add 25,25,26\n\t"
        "rotlwi 25,25,17\n\t"
        "add 26,26,27\n\t"
        "cmpwi 26,0\n\t"
        "blt 2f\n\t"
        "xori 27,27,0x5a5a\n\t"
        "2:\n\t"
        "add 27,27,28\n\t"
        "add 28,28,29\n\t"
        "add 29,29,30\n\t"
        "rotlwi 29,29,14\n\t"
        "add 30,30,31\n\t"
        "add 31,31,0\n\t"
        "add 0,0,4\n\t"
        "bdnz 1b\n\t"
        "stw 4,0(3)\n\t"
        "stw 5,4(3)\n\t"
        "stw 6,8(3)\n\t"
        "stw 7,12(3)\n\t"
        "stw 8,16(3)\n\t"
        "stw 9,20(3)\n\t"
        "stw 10,24(3)\n\t"
        "stw 11,28(3)\n\t"
        "stw 12,32(3)\n\t"
        "stw 14,36(3)\n\t"
        "stw 15,40(3)\n\t"
        "stw 16,44(3)\n\t"
        "stw 17,48(3)\n\t"
        "stw 18,52(3)\n\t"
        "stw 19,56(3)\n\t"
        "stw 20,60(3)\n\t"
        "stw 21,64(3)\n\t"
        "stw 22,68(3)\n\t"
        "stw 23,72(3)\n\t"
        "stw 24,76(3)\n\t"
        "stw 25,80(3)\n\t"
        "stw 26,84(3)\n\t"
        "stw 27,88(3)\n\t"
        "stw 28,92(3)\n\t"
        "stw 29,96(3)\n\t"
        "stw 30,100(3)\n\t"
        "stw 31,104(3)\n\t"
        "stw 0,108(3)"
        : "+r"(p)
        :
        : "r0", "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "r12", "r14", "r15", "r16",
          "r17", "r18", "r19", "r20", "r21", "r22", "r23", "r24", "r25", "r26", "r27", "r28",
          "r29", "r30", "r31", "ctr", "cr0", "memory");
    for (int i = 0; i < 28; i++)
      Mix(pressure[i]);
  }
  End("registers");
}

// The JITs don't update the FPSCR's status bits, as the interpreter does, so cputest-jit.elf only
// compares the bits that they keep: the control bits, and FPRF if it's enabled.
#ifndef FPSCR_MASK
#define FPSCR_MASK 0xFFFFFFFFu
#endif

static u64 Bits(double d)
{
  union
  {
    double d;
    u64 u;
  } x = {d};
  return x.u;
}

static double Double(u64 u)
{
  union
  {
    u64 u;
    double d;
  } x = {u};
  return x.d;
}

static double FloatValue(void)
{
  static const u64 edge[] = {
      0x0000000000000000, 0x8000000000000000, 0x3FF0000000000000, 0xBFF0000000000000,
      0x7FF0000000000000, 0xFFF0000000000000, 0x7FF8000000000000, 0x7FF4000000000000,  // NaNs
      0x0000000000000001, 0x000FFFFFFFFFFFFF, 0x8008000000000000,                      // Denormal
      0x7FEFFFFFFFFFFFFF, 0x0010000000000000, 0x3810000000000000, 0x36A0000000000000,  // Single limits
      0x47EFFFFFE0000000, 0x41E0000000000000, 0xC1E0000000200000, 0x3FE0000000000000,
  };
  const u32 r = Random();
  if (r & 1)
    return Double(edge[(r >> 1) % (sizeof(edge) / sizeof(edge[0]))]);
  // Random sign and mantissa, with exponents around the single and double ranges.
  const u64 exponent = (r >> 1) % 4 == 0 ? (Random() % 2048) : 1023 - 140 + (Random() % 280);
  return Double(((u64)(Random() & 0x80000000) << 32) | (exponent << 52) |
                (((u64)Random() << 32 | Random()) & 0x000FFFFFFFFFFFFF));
}

// Sets the FPSCR to fpscr, runs the instruction, and captures the result, FPSCR and CR1.
#define FOP(insn, ...)                                                                             \
  do                                                                                               \
  {                                                                                                \
    double r_, s_;                                                                                 \
    u32 c_;                                                                                        \
    asm volatile("mtfsf 0xff,%3\n\t" insn "\n\tmffs %1\n\tmfcr %2"                                 \
                 : "=&f"(r_), "=&f"(s_), "=&r"(c_)                                                 \
                 : "f"(fpscr), "f"(a), "f"(b), "f"(c)                                              \
                 : "cr1", "cr2");                                                                  \
    Record64(insn, Bits(r_), Bits(s_) & FPSCR_MASK, (c_ >> 20) & 0xFFF);                                        \
  } while (0)

static void TestFloatingPoint(u32 non_ieee)
{
  Begin();
  for (u32 rounding = 0; rounding < 4; rounding++)
  {
    const double fpscr = Double(rounding | non_ieee);
    for (int i = 0; i < ITERATIONS; i++)
    {
      BeginIteration();
      const double a = FloatValue(), b = FloatValue(), c = FloatValue();
      FOP("fadd %0,%4,%5");
      FOP("fadd. %0,%4,%5");
      FOP("fsub %0,%4,%5");
      FOP("fmul %0,%4,%6");
      FOP("fmul. %0,%4,%6");
      FOP("fdiv %0,%4,%5");
      FOP("fdiv. %0,%4,%5");
      FOP("fadds %0,%4,%5");
      FOP("fsubs %0,%4,%5");
      FOP("fmuls %0,%4,%6");
      FOP("fdivs %0,%4,%5");
      FOP("fmadd %0,%4,%6,%5");
      FOP("fmsub %0,%4,%6,%5");
      FOP("fnmadd %0,%4,%6,%5");
      FOP("fnmsub. %0,%4,%6,%5");
      FOP("fmadds %0,%4,%6,%5");
      FOP("fmsubs %0,%4,%6,%5");
      FOP("fnmadds %0,%4,%6,%5");
      FOP("fnmsubs %0,%4,%6,%5");
      FOP("frsp %0,%4");
      FOP("frsp. %0,%4");
      FOP("fctiw %0,%4");
      FOP("fctiwz %0,%4");
      FOP("fres %0,%4");
      FOP("frsqrte %0,%4");
      FOP("fsel %0,%4,%6,%5");
      FOP("fabs %0,%4");
      FOP("fnabs %0,%4");
      FOP("fneg %0,%4");
      FOP("fmr %0,%4");
      FOP("fcmpu 2,%4,%5\n\tfmr %0,%4");
      FOP("fcmpo 2,%4,%5\n\tfmr %0,%4");
      FOP("mtfsb1 3\n\tmtfsb0 22\n\tmtfsfi 6,9\n\tfmr %0,%4");
    }
  }
  End(non_ieee ? "floating-point-ni" : "floating-point");
}

// Single precision fused multiply-adds on inputs whose results are ties when rounded from double
// to single precision, which the interpreter corrects (see NI_madd_msub), and on NaNs for frC whose
// payloads change when frC is rounded to 25 bits.
static float FloatBits(u32 u);

static void TestFusedMultiplyAddCases(void)
{
  static const u32 ties[][3] = {
      // a, b, c
      {0x42480000, 0x1b1c72a0, 0xbc88cc38},  // From Mario Strikers Charged
      {0xc1b4fd47, 0x29ff8325, 0xbf019c77},
      {0xc2118627, 0xb47a1e02, 0xc13b95b2},
      {0xbc790257, 0xaabfede6, 0xc3471e58},
      {0xbd881ae9, 0x2a9fca37, 0xc289d2bd},
      {0x4280dc53, 0xb3d01405, 0x416a9cb9},
  };
  static const u64 nans[] = {
      0x7FF800000FFFFFFF, 0x7FF4000008000000, 0xFFF000000FFFFFFF, 0x7FF0000000000001,
  };
  Begin();
  const double fpscr = Double(0);
  for (u32 i = 0; i < sizeof(ties) / sizeof(ties[0]); i++)
  {
    BeginIteration();
    const double a = FloatBits(ties[i][0]), b = FloatBits(ties[i][1]), c = FloatBits(ties[i][2]);
    FOP("fmadds %0,%4,%6,%5");
    FOP("fmsubs %0,%4,%6,%5");
    FOP("fnmadds %0,%4,%6,%5");
    FOP("fnmsubs %0,%4,%6,%5");
  }
  for (u32 i = 0; i < 2 * sizeof(nans) / sizeof(nans[0]); i++)
  {
    BeginIteration();
    const double a = (i & 1) ? 1.0 : 0.0, b = 2.0, c = Double(nans[i / 2]);
    FOP("fmadds %0,%4,%6,%5");
    FOP("fnmsubs %0,%4,%6,%5");
    FOP("fmuls %0,%4,%6");
  }
  End("fma-cases");
}

// Floating-point loads and stores, including stores of doubles too small for normal singles,
// which the store converts specially (see ConvertToSingle).
static void TestFloatLoadsAndStores(void)
{
  static u64 buffer[8] __attribute__((aligned(32)));
  Begin();
  for (int i = 0; i < ITERATIONS; i++)
  {
    BeginIteration();
    for (int j = 0; j < 8; j++)
      buffer[j] = ((u64)Random() << 32) | Random();
    // Exponents from 860 to 910 are around the smallest singles.
    double value = FloatValue();
    if (Random() & 1)
      value = Double((Bits(value) & 0x800FFFFFFFFFFFFFULL) | ((u64)(860 + Random() % 50) << 52));
    const u32 index = 8 * (Random() % 4);
    u8* const base = (u8*)buffer;
    double s0, s1, d0, d1;
    u8* p = base;
    asm volatile("lfs %0,4(%5)\n\tlfsx %1,%5,%6\n\tlfd %2,8(%5)\n\tlfdux %3,%4,%6"
                 : "=&f"(s0), "=&f"(s1), "=&f"(d0), "=&f"(d1), "+b"(p)
                 : "b"(base), "r"(index)
                 : "memory");
    Record64("lfs", Bits(s0), Bits(s1), 0);
    Record64("lfd", Bits(d0), Bits(d1), (u32)(p - base));
    p = base + 32;
    asm volatile("stfs %1,0(%2)\n\tstfsu %1,4(%0)\n\tstfd %1,8(%2)\n\tstfdx %1,%2,%3\n\t"
                 "stfiwx %1,%0,%3"
                 : "+b"(p)
                 : "f"(value), "b"(base + 16), "r"(index)
                 : "memory");
    Record("stfs", (u32)(p - base), 0, 0);
    for (int j = 0; j < 8; j++)
      Mix64(buffer[j]);
  }
  End("float-loads-stores");
}

#define PSOP(insn)                                                                                 \
  do                                                                                               \
  {                                                                                                \
    double r_, s_;                                                                                 \
    u32 c_;                                                                                        \
    double out[2];                                                                                 \
    asm volatile("mtfsf 0xff,%3\n\tpsq_l 1,0(%4),0,0\n\tpsq_l 2,8(%4),0,0\n\tpsq_l 3,16(%4),0,0\n\t" \
                 insn "\n\tmffs %1\n\tmfcr %2\n\tstfd %0,0(%5)\n\tps_merge11 %0,%0,%0\n\tstfd %0,8(%5)" \
                 : "=&f"(r_), "=&f"(s_), "=&r"(c_)                                                 \
                 : "f"(fpscr), "b"(inputs), "b"(out)                                               \
                 : "fr1", "fr2", "fr3", "cr1", "cr2", "memory");                                   \
    Record64(insn, Bits(out[0]), Bits(out[1]), (c_ >> 20) & 0xFFF);                                \
    Mix64(Bits(s_) & FPSCR_MASK);                                                                       \
  } while (0)

static float FloatBits(u32 u)
{
  union
  {
    u32 u;
    float f;
  } x = {u};
  return x.f;
}

static float SingleValue(void)
{
  static const u32 edge[] = {0x00000000, 0x80000000, 0x3F800000, 0xBF800000, 0x7F800000,
                             0xFF800000, 0x7FC00000, 0x7FA00000, 0x00000001, 0x007FFFFF,
                             0x7F7FFFFF, 0x00800000, 0x3F000000, 0x4B000000};
  const u32 r = Random();
  if (r & 1)
    return FloatBits(edge[(r >> 1) % (sizeof(edge) / sizeof(edge[0]))]);
  return FloatBits((Random() & 0x80000000) | ((80 + Random() % 100) << 23) |
                   (Random() & 0x007FFFFF));
}

static void TestPairedSingles(void)
{
  Begin();
  float inputs[6] __attribute__((aligned(8)));
  for (u32 rounding = 0; rounding < 4; rounding++)
  {
    const double fpscr = Double(rounding);
    for (int i = 0; i < ITERATIONS; i++)
    {
      BeginIteration();
      for (int j = 0; j < 6; j++)
        inputs[j] = SingleValue();
      PSOP("ps_add %0,1,2");
      PSOP("ps_sub %0,1,2");
      PSOP("ps_mul %0,1,3");
      PSOP("ps_div %0,1,2");
      PSOP("ps_madd %0,1,3,2");
      PSOP("ps_msub %0,1,3,2");
      PSOP("ps_nmadd %0,1,3,2");
      PSOP("ps_nmsub %0,1,3,2");
      PSOP("ps_sum0 %0,1,3,2");
      PSOP("ps_sum1 %0,1,3,2");
      PSOP("ps_muls0 %0,1,3");
      PSOP("ps_muls1 %0,1,3");
      PSOP("ps_madds0 %0,1,3,2");
      PSOP("ps_madds1 %0,1,3,2");
      PSOP("ps_merge00 %0,1,2");
      PSOP("ps_merge01 %0,1,2");
      PSOP("ps_merge10 %0,1,2");
      PSOP("ps_merge11 %0,1,2");
      PSOP("ps_res %0,1");
      PSOP("ps_rsqrte %0,1");
      PSOP("ps_sel %0,1,3,2");
      PSOP("ps_neg %0,1");
      PSOP("ps_abs %0,1");
      PSOP("ps_nabs %0,1");
      PSOP("ps_mr %0,1");
      PSOP("ps_cmpu0 2,1,2\n\tps_mr %0,1");
      PSOP("ps_cmpo1 2,1,2\n\tps_mr %0,1");
      PSOP("fadd %0,1,2");
      PSOP("fmuls %0,1,3");
    }
  }
  End("paired-singles");
}

// Paired-single fused multiply-adds and multiplies on the cases of TestFusedMultiplyAddCases, in
// both halves, with the halves made from doubles, so that frC can be a NaN that changes when it's
// rounded to 25 bits.
#define PSOPD(insn)                                                                                \
  do                                                                                               \
  {                                                                                                \
    double r_, s_;                                                                                 \
    u32 c_;                                                                                        \
    double out[2];                                                                                 \
    asm volatile("mtfsf 0xff,%3\n\tlfd 1,0(%4)\n\tlfd 4,8(%4)\n\tps_merge00 1,1,4\n\t"             \
                 "lfd 2,16(%4)\n\tlfd 4,24(%4)\n\tps_merge00 2,2,4\n\t"                            \
                 "lfd 3,32(%4)\n\tlfd 4,40(%4)\n\tps_merge00 3,3,4\n\t" insn                       \
                 "\n\tmffs %1\n\tmfcr %2\n\tstfd %0,0(%5)\n\tps_merge11 %0,%0,%0\n\tstfd %0,8(%5)" \
                 : "=&f"(r_), "=&f"(s_), "=&r"(c_)                                                 \
                 : "f"(fpscr), "b"(inputs), "b"(out)                                               \
                 : "fr1", "fr2", "fr3", "fr4", "cr1", "cr2", "memory");                            \
    Record64(insn, Bits(out[0]), Bits(out[1]), (c_ >> 20) & 0xFFF);                                \
    Mix64(Bits(s_) & FPSCR_MASK);                                                                  \
  } while (0)

static void TestPairedFusedMultiplyAddCases(void)
{
  static const u32 ties[][3] = {
      // a, b, c
      {0x42480000, 0x1b1c72a0, 0xbc88cc38}, {0xc1b4fd47, 0x29ff8325, 0xbf019c77},
      {0xc2118627, 0xb47a1e02, 0xc13b95b2}, {0xbc790257, 0xaabfede6, 0xc3471e58},
      {0xbd881ae9, 0x2a9fca37, 0xc289d2bd}, {0x4280dc53, 0xb3d01405, 0x416a9cb9},
  };
  static const u64 nans[] = {
      0x7FF800000FFFFFFF, 0x7FF4000008000000, 0xFFF000000FFFFFFF, 0x7FF0000000000001,
  };
  const u32 count = sizeof(ties) / sizeof(ties[0]);
  double inputs[6] __attribute__((aligned(8)));
  Begin();
  const double fpscr = Double(0);
  for (u32 i = 0; i < count; i++)
  {
    BeginIteration();
    // a, b and c for half 0 are inputs[0], [2] and [4]; for half 1, [1], [3] and [5].
    for (u32 h = 0; h < 2; h++)
      for (u32 k = 0; k < 3; k++)
        inputs[2 * k + h] = FloatBits(ties[(i + h) % count][k]);
    PSOPD("ps_madd %0,1,3,2");
    PSOPD("ps_msub %0,1,3,2");
    PSOPD("ps_nmadd %0,1,3,2");
    PSOPD("ps_nmsub %0,1,3,2");
    PSOPD("ps_madds0 %0,1,3,2");
    PSOPD("ps_madds1 %0,1,3,2");
  }
  for (u32 i = 0; i < 2 * sizeof(nans) / sizeof(nans[0]); i++)
  {
    BeginIteration();
    inputs[0] = (i & 1) ? 1.0 : 0.0;
    inputs[1] = (i & 1) ? 0.0 : 1.0;
    inputs[2] = inputs[3] = 2.0;
    inputs[4] = Double(nans[i / 2]);
    inputs[5] = Double(nans[(i / 2 + 1) % 4]);
    PSOPD("ps_madd %0,1,3,2");
    PSOPD("ps_nmsub %0,1,3,2");
    PSOPD("ps_madds1 %0,1,3,2");
    PSOPD("ps_mul %0,1,3");
    PSOPD("ps_muls0 %0,1,3");
  }
  End("paired-fma-cases");
}

#define GQR(n) (912 + (n))

static void TestQuantizedLoadsAndStores(void)
{
  Begin();
  static u8 buffer[64] __attribute__((aligned(32)));
  for (int i = 0; i < ITERATIONS; i++)
  {
    for (u32 j = 0; j < sizeof(buffer); j += 4)
      *(u32*)&buffer[j] = Random();
    // Load and store types: float, u8, u16, s8, s16; random scales.
    static const u32 types[] = {0, 4, 5, 6, 7};
    const u32 load_type = types[Random() % 5], store_type = types[Random() % 5];
    const u32 gqr = ((Random() % 64) << 24) | (load_type << 16) | ((Random() % 64) << 8) | store_type;
    double ps0, ps1;
    asm volatile("mtspr %3,%2\n\tpsq_l 1,0(%4),0,1\n\tpsq_st 1,32(%4),0,1\n\t"
                 "psq_l 2,8(%4),1,1\n\tpsq_st 2,40(%4),1,1\n\tfmr %0,1\n\tps_merge11 %1,1,1"
                 : "=&f"(ps0), "=&f"(ps1)
                 : "r"(gqr), "i"(GQR(1)), "b"(buffer)
                 : "fr1", "fr2", "memory");
    Mix64(Bits(ps0));
    Mix64(Bits(ps1));
    for (u32 j = 32; j < 48; j += 4)
      Mix(*(u32*)&buffer[j]);
  }
  End("quantized");
}

// Quantized loads and stores with GQRs that are set outside the function that uses them, so that
// the JITs can compile them for their types and scales, and in all forms.
static void __attribute__((noipa)) QuantizedOps(u8* buf, const double* values, double* out,
                                                u8* stores, u32 index, u8** p, u8** q)
{
  u8* p_ = *p;
  u8* q_ = *q;
  asm volatile(
      "psq_l 1,32(%[buf]),0,2\n\t"
      "psq_l 2,40(%[buf]),1,2\n\t"
      "stfd 1,64(%[out])\n\t"
      "ps_merge11 1,1,1\n\t"
      "stfd 1,72(%[out])\n\t"
      "stfd 2,80(%[out])\n\t"
      "ps_merge11 2,2,2\n\t"
      "stfd 2,88(%[out])\n\t"
      "lfd 3,32(%[values])\n\t"
      "lfd 4,40(%[values])\n\t"
      "ps_merge00 3,3,4\n\t"
      "psq_st 3,32(%[stores]),0,2\n\t"
      "psq_st 3,40(%[stores]),1,2\n\t"
      "psq_l 1,48(%[buf]),0,3\n\t"
      "psq_l 2,56(%[buf]),1,3\n\t"
      "stfd 1,96(%[out])\n\t"
      "ps_merge11 1,1,1\n\t"
      "stfd 1,104(%[out])\n\t"
      "stfd 2,112(%[out])\n\t"
      "ps_merge11 2,2,2\n\t"
      "stfd 2,120(%[out])\n\t"
      "lfd 3,48(%[values])\n\t"
      "lfd 4,56(%[values])\n\t"
      "ps_merge00 3,3,4\n\t"
      "psq_st 3,48(%[stores]),0,3\n\t"
      "psq_st 3,56(%[stores]),1,3\n\t"
      "psq_l 1,64(%[buf]),0,4\n\t"
      "psq_l 2,72(%[buf]),1,4\n\t"
      "stfd 1,128(%[out])\n\t"
      "ps_merge11 1,1,1\n\t"
      "stfd 1,136(%[out])\n\t"
      "stfd 2,144(%[out])\n\t"
      "ps_merge11 2,2,2\n\t"
      "stfd 2,152(%[out])\n\t"
      "lfd 3,64(%[values])\n\t"
      "lfd 4,72(%[values])\n\t"
      "ps_merge00 3,3,4\n\t"
      "psq_st 3,64(%[stores]),0,4\n\t"
      "psq_st 3,72(%[stores]),1,4\n\t"
      "psq_l 1,80(%[buf]),0,5\n\t"
      "psq_l 2,88(%[buf]),1,5\n\t"
      "stfd 1,160(%[out])\n\t"
      "ps_merge11 1,1,1\n\t"
      "stfd 1,168(%[out])\n\t"
      "stfd 2,176(%[out])\n\t"
      "ps_merge11 2,2,2\n\t"
      "stfd 2,184(%[out])\n\t"
      "lfd 3,80(%[values])\n\t"
      "lfd 4,88(%[values])\n\t"
      "ps_merge00 3,3,4\n\t"
      "psq_st 3,80(%[stores]),0,5\n\t"
      "psq_st 3,88(%[stores]),1,5\n\t"
      "psq_l 1,96(%[buf]),0,6\n\t"
      "psq_l 2,104(%[buf]),1,6\n\t"
      "stfd 1,192(%[out])\n\t"
      "ps_merge11 1,1,1\n\t"
      "stfd 1,200(%[out])\n\t"
      "stfd 2,208(%[out])\n\t"
      "ps_merge11 2,2,2\n\t"
      "stfd 2,216(%[out])\n\t"
      "lfd 3,96(%[values])\n\t"
      "lfd 4,104(%[values])\n\t"
      "ps_merge00 3,3,4\n\t"
      "psq_st 3,96(%[stores]),0,6\n\t"
      "psq_st 3,104(%[stores]),1,6\n\t"
      "psq_l 1,112(%[buf]),0,7\n\t"
      "psq_l 2,120(%[buf]),1,7\n\t"
      "stfd 1,224(%[out])\n\t"
      "ps_merge11 1,1,1\n\t"
      "stfd 1,232(%[out])\n\t"
      "stfd 2,240(%[out])\n\t"
      "ps_merge11 2,2,2\n\t"
      "stfd 2,248(%[out])\n\t"
      "lfd 3,112(%[values])\n\t"
      "lfd 4,120(%[values])\n\t"
      "ps_merge00 3,3,4\n\t"
      "psq_st 3,112(%[stores]),0,7\n\t"
      "psq_st 3,120(%[stores]),1,7\n\t"
      "psq_lx 1,%[buf],%[index],0,4\n\t"
      "stfd 1,256(%[out])\n\t"
      "ps_merge11 1,1,1\n\t"
      "stfd 1,264(%[out])\n\t"
      "psq_lux 1,%[p],%[index],1,5\n\t"
      "stfd 1,272(%[out])\n\t"
      "ps_merge11 1,1,1\n\t"
      "stfd 1,280(%[out])\n\t"
      "psq_stx 3,%[stores],%[index],0,6\n\t"
      "psq_stux 3,%[q],%[index],1,7\n\t"
      "psq_lu 2,8(%[p]),0,6\n\t"
      "stfd 2,288(%[out])\n\t"
      "ps_merge11 2,2,2\n\t"
      "stfd 2,296(%[out])\n\t"
      "psq_stu 3,16(%[q]),0,2\n\t"
      : [p] "+b"(p_), [q] "+b"(q_)
      : [buf] "b"(buf), [values] "b"(values), [out] "b"(out), [stores] "b"(stores),
        [index] "r"(index)
      : "fr1", "fr2", "fr3", "fr4", "memory");
  *p = p_;
  *q = q_;
}

static void __attribute__((noipa)) SetGQR3(u32 value)
{
  asm volatile("mtspr %0,%1" : : "i"(GQR(3)), "r"(value));
}
static void (*volatile set_gqr3)(u32) = SetGQR3;

static void TestQuantizedCompiled(void)
{
  // GQR2-GQR7: load type and scale, store type and scale, with scales of both signs.
  static const u32 gqrs[] = {
      (0 << 16) | (0 << 24) | (0 << 0) | (0 << 8),    // float
      (4 << 16) | (3 << 24) | (4 << 0) | (5 << 8),    // u8
      (5 << 16) | (60 << 24) | (5 << 0) | (40 << 8),  // u16, negative scales
      (6 << 16) | (31 << 24) | (6 << 0) | (2 << 8),   // s8
      (7 << 16) | (8 << 24) | (7 << 0) | (63 << 8),   // s16
      (7 << 16) | (33 << 24) | (4 << 0) | (16 << 8),  // s16 loads, u8 stores
  };
  static u8 buf[512] __attribute__((aligned(32)));
  static u8 stores[512] __attribute__((aligned(32)));
  static double values[16] __attribute__((aligned(16)));
  static double out[48] __attribute__((aligned(16)));
  asm volatile("mtspr %0,%1" : : "i"(GQR(2)), "r"(gqrs[0]));
  asm volatile("mtspr %0,%1" : : "i"(GQR(3)), "r"(gqrs[1]));
  asm volatile("mtspr %0,%1" : : "i"(GQR(4)), "r"(gqrs[2]));
  asm volatile("mtspr %0,%1" : : "i"(GQR(5)), "r"(gqrs[3]));
  asm volatile("mtspr %0,%1" : : "i"(GQR(6)), "r"(gqrs[4]));
  asm volatile("mtspr %0,%1" : : "i"(GQR(7)), "r"(gqrs[5]));
  Begin();
  for (int i = 0; i < ITERATIONS; i++)
  {
    // Change a GQR near the end, so that the JITs find their assumption wrong. The call is
    // indirect, so that the change isn't in the same block as the instructions that use it.
    if (i == ITERATIONS - 100)
      set_gqr3(gqrs[3]);
    for (u32 j = 0; j < sizeof(buf); j += 4)
      *(u32*)&buf[j] = Random();
    for (int j = 0; j < 16; j++)
    {
      // Values in the ranges of the integer types, after scaling, and others.
      const double v = FloatValue();
      values[j] = (Random() & 1) ? v : (double)(s32)(Random() % 140000 - 70000) / 64.0;
    }
    u8* p = buf + 64;
    u8* q = stores + 128;
    const u32 index = 8 * (Random() % 8);
    QuantizedOps(buf, values, out, stores, index, &p, &q);
    for (int j = 0; j < 38; j++)
      Mix64(Bits(out[j]));
    for (u32 j = 0; j < 256; j += 4)
      Mix(*(u32*)&stores[j]);
    Mix((u32)(p - buf));
    Mix((u32)(q - stores));
  }
  End("quantized-compiled");
}

static u32 code[8] __attribute__((aligned(32)));

static void FlushCode(void* start, u32 size)
{
  for (u32 i = 0; i < size; i += 32)
    asm volatile("dcbst 0,%0\n\tsync\n\ticbi 0,%0" : : "r"((u8*)start + i) : "memory");
  asm volatile("sync\n\tisync" : : : "memory");
}

static void TestSelfModifyingCode(void)
{
  Begin();
  u32 (*const function)(void) = (u32(*)(void))code;
  for (u32 i = 0; i < 100; i++)
  {
    code[0] = 0x38600000 | (i * 7);  // li r3, i * 7
    code[1] = 0x4E800020;            // blr
    FlushCode(code, sizeof(code));
    Mix(function());
  }
  End("self-modifying-code");
}

// Exception handlers, copied to the exception vectors. They count exceptions in SPRG1-SPRG3.
extern const u32 program_handler[], program_handler_end[];
extern const u32 syscall_handler[], syscall_handler_end[];
extern const u32 fpu_handler[], fpu_handler_end[];
asm(".section .text\n"
    "program_handler:\n"
    "  mtsprg0 3\n  mfsprg1 3\n  addi 3,3,1\n  mtsprg1 3\n"
    "  mfsrr0 3\n  addi 3,3,4\n  mtsrr0 3\n  mfsprg0 3\n  rfi\n"
    "program_handler_end:\n"
    "syscall_handler:\n"
    "  mtsprg0 3\n  mfsprg2 3\n  addi 3,3,1\n  mtsprg2 3\n  mfsprg0 3\n  rfi\n"
    "syscall_handler_end:\n"
    "fpu_handler:\n"
    "  mtsprg0 3\n  mfsprg3 3\n  addi 3,3,1\n  mtsprg3 3\n"
    "  mfsrr1 3\n  ori 3,3,0x2000\n  mtsrr1 3\n  mfsprg0 3\n  rfi\n"
    "fpu_handler_end:\n");

static void InstallHandler(u32 vector, const u32* start, const u32* end)
{
  u32* const destination = (u32*)(0x80000000 + vector);
  for (const u32* p = start; p < end; p++)
    destination[p - start] = *p;
  FlushCode(destination, 0x100);
}

#ifdef TEST_DSI
// Data storage exceptions, which only MMU mode raises: an empty page table makes every address
// that the BATs don't map fault. The handler records DAR, DSISR and a count in SPRG1-SPRG3, and
// skips the faulting instruction, which must not have written any register.
extern const u32 dsi_handler[], dsi_handler_end[];
asm(".section .text\n"
    "dsi_handler:\n"
    "  mtsprg0 3\n  mfsprg1 3\n  addi 3,3,1\n  mtsprg1 3\n"
    "  mfdar 3\n  mtsprg2 3\n  mfdsisr 3\n  mtsprg3 3\n"
    "  mfsrr0 3\n  addi 3,3,4\n  mtsrr0 3\n  mfsprg0 3\n  rfi\n"
    "dsi_handler_end:\n");

// The page table: 64 KiB of invalid entries, at physical 16 MiB.
#define PAGE_TABLE ((u32*)0x81000000)

static void RecordDSI(const char* name, u32 a, u32 b)
{
  u32 count, dar, dsisr, srr0;
  asm volatile("mfsprg1 %0\n\tmfsprg2 %1\n\tmfsprg3 %2\n\tmfsrr0 %3"
               : "=r"(count), "=r"(dar), "=r"(dsisr), "=r"(srr0));
  Record(name, a, b, dar);
  Mix(count);
  Mix(dsisr);
  Mix(srr0);
}

#define DSI_LOAD(insn, offset)                                                                     \
  do                                                                                               \
  {                                                                                                \
    u32 r_ = value;                                                                                \
    asm volatile(insn " %0," #offset "(%1)" : "+r"(r_) : "b"(bad) : "memory");                     \
    RecordDSI(insn, r_, 0);                                                                        \
  } while (0)
#define DSI_LOADX(insn)                                                                            \
  do                                                                                               \
  {                                                                                                \
    u32 r_ = value;                                                                                \
    asm volatile(insn " %0,%1,%2" : "+r"(r_) : "b"(bad), "r"(index) : "memory");                   \
    RecordDSI(insn, r_, 0);                                                                        \
  } while (0)
#define DSI_LOADU(insn, offset)                                                                    \
  do                                                                                               \
  {                                                                                                \
    u32 r_ = value;                                                                                \
    u8* p_ = bad;                                                                                  \
    asm volatile(insn " %0," #offset "(%1)" : "+r"(r_), "+b"(p_) : : "memory");                    \
    RecordDSI(insn, r_, (u32)(p_ - bad));                                                          \
  } while (0)
#define DSI_LOADUX(insn)                                                                           \
  do                                                                                               \
  {                                                                                                \
    u32 r_ = value;                                                                                \
    u8* p_ = bad;                                                                                  \
    asm volatile(insn " %0,%1,%2" : "+r"(r_), "+b"(p_) : "r"(index) : "memory");                   \
    RecordDSI(insn, r_, (u32)(p_ - bad));                                                          \
  } while (0)
#define DSI_STORE(insn, offset)                                                                    \
  do                                                                                               \
  {                                                                                                \
    asm volatile(insn " %0," #offset "(%1)" : : "r"(value), "b"(bad) : "memory");                  \
    RecordDSI(insn, 0, 0);                                                                         \
  } while (0)
#define DSI_STOREX(insn)                                                                           \
  do                                                                                               \
  {                                                                                                \
    asm volatile(insn " %0,%1,%2" : : "r"(value), "b"(bad), "r"(index) : "memory");                \
    RecordDSI(insn, 0, 0);                                                                         \
  } while (0)
#define DSI_STOREU(insn, offset)                                                                   \
  do                                                                                               \
  {                                                                                                \
    u8* p_ = bad;                                                                                  \
    asm volatile(insn " %1," #offset "(%0)" : "+b"(p_) : "r"(value) : "memory");                   \
    RecordDSI(insn, (u32)(p_ - bad), 0);                                                           \
  } while (0)
#define DSI_FLOAD(insn, offset)                                                                    \
  do                                                                                               \
  {                                                                                                \
    double d_ = fvalue;                                                                            \
    u8* p_ = bad;                                                                                  \
    asm volatile(insn " %0," #offset "(%1)" : "+f"(d_), "+b"(p_) : : "memory");                    \
    const u64 bits_ = Bits(d_);                                                                    \
    RecordDSI(insn, (u32)(bits_ >> 32) ^ (u32)bits_, (u32)(p_ - bad));                             \
  } while (0)
#define DSI_FSTORE(insn, offset)                                                                   \
  do                                                                                               \
  {                                                                                                \
    u8* p_ = bad;                                                                                  \
    asm volatile(insn " %1," #offset "(%0)" : "+b"(p_) : "f"(fvalue) : "memory");                  \
    RecordDSI(insn, (u32)(p_ - bad), 0);                                                           \
  } while (0)

static void TestDSI(void)
{
  Begin();
  InstallHandler(0x300, dsi_handler, dsi_handler_end);
  asm volatile("li 3,0\n\tmtsprg1 3\n\tmtsprg2 3\n\tmtsprg3 3" : : : "r3");

  u32 sdr1;
  asm volatile("mfsdr1 %0" : "=r"(sdr1));
  for (u32 i = 0; i < 0x10000 / 4; i++)
    PAGE_TABLE[i] = 0;
  FlushCode(PAGE_TABLE, 0x10000);
  asm volatile("mtsdr1 %0\n\tsync\n\ttlbsync\n\tsync" : : "r"((u32)PAGE_TABLE & 0x3FFFFFFF));
  for (u32 page = 0; page < 0x100000; page += 0x1000)
    asm volatile("tlbie %0" : : "r"(page));
  asm volatile("sync\n\ttlbsync\n\tsync");

  for (int i = 0; i < 50; i++)
  {
    BeginIteration();
    u8* const bad = (u8*)(0x40000000 + (Random() & 0xFFFFF0));
    const u32 index = Random() % 4096;
    const u32 value = Value();
    const double fvalue = FloatValue();

    DSI_LOAD("lbz", 1);
    DSI_LOAD("lhz", 2);
    DSI_LOAD("lha", -2);
    DSI_LOAD("lwz", 4);
    DSI_LOADX("lwzx");
    DSI_LOADX("lhbrx");
    DSI_LOADX("lwbrx");
    DSI_LOADU("lbzu", 1);
    DSI_LOADU("lhau", 2);
    DSI_LOADU("lwzu", 8);
    DSI_LOADUX("lwzux");
    DSI_STORE("stb", 1);
    DSI_STORE("sth", 2);
    DSI_STORE("stw", 4);
    DSI_STOREX("stwx");
    DSI_STOREX("sthbrx");
    DSI_STOREU("stbu", 1);
    DSI_STOREU("stwu", 8);
    DSI_FLOAD("lfs", 4);
    DSI_FLOAD("lfd", 8);
    DSI_FLOAD("lfsu", 4);
    DSI_FLOAD("lfdu", 8);
    DSI_FSTORE("stfs", 4);
    DSI_FSTORE("stfd", 8);
    DSI_FSTORE("stfdu", 8);

    // Quantized, with GQR0, whose types are float.
    {
      double ps0 = fvalue, ps1;
      u8* p_ = bad;
      asm volatile("ps_merge00 %0,%0,%0\n\tpsq_lu %0,8(%2),0,0\n\tps_merge11 %1,%0,%0"
                   : "+f"(ps0), "=f"(ps1), "+b"(p_)
                   :
                   : "memory");
      RecordDSI("psq_lu", (u32)Bits(ps0) ^ (u32)(Bits(ps1) >> 32), (u32)(p_ - bad));
      p_ = bad;
      asm volatile("psq_stu %1,8(%0),0,0" : "+b"(p_) : "f"(fvalue) : "memory");
      RecordDSI("psq_stu", (u32)(p_ - bad), 0);
    }

    // A fault in the middle of a block: what the instructions before and after it wrote must
    // be kept, and what it would have written must not.
    {
      u32 a = value, b = value ^ 0x5A5A5A5A;
      double f = fvalue, g = fvalue;
      asm volatile("addi %0,%0,1\n\tfadd %2,%2,%2\n\tlwz %1,0(%4)\n\taddi %0,%0,2\n\t"
                   "lfd %3,0(%4)\n\tfadd %2,%2,%2"
                   : "+r"(a), "+r"(b), "+f"(f), "+f"(g)
                   : "b"(bad)
                   : "memory");
      RecordDSI("mid-block", a, b);
      Mix64(Bits(f));
      Mix64(Bits(g));
    }
  }

  asm volatile("mtsdr1 %0\n\tsync" : : "r"(sdr1));
  u32 count;
  asm volatile("mfsprg1 %0" : "=r"(count));
  OSReport("TEST dsi count %u\n", count);
  End("dsi");
}
#endif

static void TestExceptions(void)
{
  Begin();
  InstallHandler(0x700, program_handler, program_handler_end);
  InstallHandler(0xC00, syscall_handler, syscall_handler_end);
  InstallHandler(0x800, fpu_handler, fpu_handler_end);
  asm volatile("li 3,0\n\tmtsprg1 3\n\tmtsprg2 3\n\tmtsprg3 3" : : : "r3");

  for (u32 i = 0; i < 20; i++)
  {
    const u32 a = Random() % 4;
    // Traps: unconditional, and conditional on a.
    asm volatile("trap\n\ttwi 4,%0,1\n\ttwi 8,%0,2\n\ttwi 16,%0,3\n\ttw 31,%0,%0" : : "r"(a));
    asm volatile("sc" : : : "memory");
    if (i % 5 == 0)
    {
      // Disable the FPU, so that the next floating-point instruction raises an exception.
      u32 msr;
      double d = (double)a;
      asm volatile("mfmsr %0\n\trlwinm %0,%0,0,19,17\n\tmtmsr %0\n\tisync\n\tfadd %1,%1,%1"
                   : "=&r"(msr), "+f"(d));
      Mix64(Bits(d));
    }
  }

  u32 programs, syscalls, fpus;
  asm volatile("mfsprg1 %0\n\tmfsprg2 %1\n\tmfsprg3 %2" : "=r"(programs), "=r"(syscalls), "=r"(fpus));
  Mix(programs);
  Mix(syscalls);
  Mix(fpus);
  OSReport("exceptions: %u program, %u system call, %u FPU unavailable\n", programs,
                  syscalls, fpus);
  End("exceptions");
}

int main(void)
{
  // Enable paired singles and quantized loads and stores (HID2 PSE and LSQE).
  asm volatile("mfspr 3,920\n\toris 3,3,0xA000\n\tmtspr 920,3" : : : "r3");

  OSReport("START\n");
  TestIntegerArithmetic();
  TestIntegerLogic();
  TestRotates();
  TestCompareAndConditionRegister();
  TestLoadsAndStores();
  TestMMIO();
  TestBranches();
  TestRegisterPressure();
  TestFloatingPoint(0);
  TestFloatingPoint(4);  // FPSCR.NI
  TestFusedMultiplyAddCases();
  TestFloatLoadsAndStores();
  TestPairedSingles();
  TestPairedFusedMultiplyAddCases();
  TestQuantizedLoadsAndStores();
  TestQuantizedCompiled();
  TestSelfModifyingCode();
  TestExceptions();
#ifdef TEST_DSI
  TestDSI();
#endif
  // Dolphin's printf takes the first word of the format string for a pointer to a this argument if
  // it looks like an address, as "DONE" does, so start every report with "TEST".
  OSReport("TEST done\n");
  return 0;
}
