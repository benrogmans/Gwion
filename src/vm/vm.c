#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "gwion_util.h"
#include "gwion_ast.h"
#include "oo.h"
#include "vm.h"
#include "env.h"
#include "nspc.h"//dot func
#include "func.h"//dot func
#include "type.h"
#include "instr.h"
#include "object.h"
#include "import.h"
#include "ugen.h"
#include "shreduler_private.h"
#include "emit.h"
#include "gwion.h"
#include "map_private.h"

#include "value.h"
#include "gack.h"


static inline uint64_t splitmix64_stateless(uint64_t index) {
  uint64_t z = (index + UINT64_C(0x9E3779B97F4A7C15));
  z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
  z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
  return z ^ (z >> 31);
}

static inline uint32_t rotl(const uint32_t x, int k) {
  return (x << k) | (x >> (32 -k));
}

void gw_seed(uint32_t rnd[2], const uint64_t s) {
  uint64_t seed = splitmix64_stateless(s);
  memcpy(rnd, &seed, sizeof(uint64_t));
}

/*xoroshiro32** */
uint32_t gw_rand(uint32_t s[2]) {
  const uint32_t s0 = s[0];
  const uint32_t s1 = s[1] ^ s0;
  const uint32_t ret = rotl(s0 * 0x9E3779BB, 5) * 5;
  s[0] = rotl(s0, 26) ^ s1 ^ (s1 << 9);
  s[1] = rotl(s1, 13);
  return ret;
}

VM* new_vm(void) {
  VM* vm = (VM*)mp_alloc(VM);
  vector_init(&vm->ugen);
  vm->bbq = new_driver();
  vm->shreduler  = (Shreduler)mp_alloc(Shreduler);
  vector_init(&vm->shreduler->shreds);
  vm->shreduler->bbq = vm->bbq;
  gw_seed(vm->rand, (uint64_t)time(NULL));
  return vm;
}

void vm_remove(const VM* vm, const m_uint index) {
  const Vector v = (Vector)&vm->shreduler->shreds;
  LOOP_OPTIM
  for(m_uint i = vector_size(v) + 1; i--;) {
    const VM_Shred sh = (VM_Shred)vector_at(v, i - 1);
    if(sh->tick->xid == index)
       Except(sh, "MsgRemove");
  }
}

ANN void free_vm(VM* vm) {
  vector_release(&vm->shreduler->shreds);
  vector_release(&vm->ugen);
  if(vm->bbq)
    free_driver(vm->bbq, vm);
  mp_free(Shreduler, vm->shreduler);
  mp_free(VM, vm);
}

ANN m_uint vm_add_shred(const VM* vm, const VM_Shred shred) {
  shred->info->vm = (VM*)vm;
  shred->info->me = new_shred(shred);
  shreduler_add(vm->shreduler, shred);
  shredule(vm->shreduler, shred, .5);
  return shred->tick->xid;
}

__attribute__((hot))
ANN static inline void vm_ugen_init(const VM* vm) {
  const Vector v = (Vector)&vm->ugen;
  LOOP_OPTIM
  for(m_uint i = vector_size(v) + 1; --i;) {
    const UGen u = (UGen)vector_at(v, i - 1);
    u->done = 0;
    if(u->multi) {
      struct ugen_multi_* m = u->connect.multi;
      LOOP_OPTIM
      for(m_uint j = m->n_chan + 1; --j;)
        UGEN(m->channel[j - 1])->done = 0;
    }
  }
  const UGen hole = (UGen)vector_at(v, 0);
  hole->compute(hole);
}

#ifdef DEBUG_STACK
#define VM_INFO                                                              \
  if(s->curr)                                                                \
    gw_err("shred[%" UINT_F "] mem[%" INT_F"] reg[%" INT_F"]\n", \
    shred->tick->xid, \
    mem - ((m_bit*)shred + sizeof(struct VM_Shred_) + SIZEOF_REG), reg - ((m_bit*)shred + sizeof(struct VM_Shred_)));
#else
#define VM_INFO
#endif

#ifdef VMBENCH
#include <time.h>
static struct timespec exec_time;
#include <bsd/sys/time.h>
#endif


ANN static inline m_bool overflow_(const m_bit* mem, const VM_Shred c) {
  return mem >  (((m_bit*)c + sizeof(struct VM_Shred_) + SIZEOF_REG) + (SIZEOF_MEM) - (MEM_STEP));
}

ANN static inline VM_Shred init_spork_shred(const VM_Shred shred, const VM_Code code) {
  const VM_Shred sh = new_vm_shred(code);
  ADD_REF(code)
  sh->tick->parent = shred->tick;
  if(!shred->tick->child.ptr)
    vector_init(&shred->tick->child);
  vector_add(&shred->tick->child, (vtype)sh);
  sh->base = shred->base;
  vm_add_shred(shred->info->vm, sh);
  return sh;
}

#define TEST0(t, pos) if(!*(t*)(reg-pos)){ exception(shred, "ZeroDivideException"); break; }
#define DISPATCH()\
  instr =(Instr)(ip[pc++]);\
  VM_INFO;\
  goto *dispatch[instr->opcode];

#define OP(t, sz, op, ...) \
  reg -= sz;\
  __VA_ARGS__\
  *(t*)(reg - sz) op##= *(t*)reg;\
  DISPATCH();

#define INT_OP(op, ...) OP(m_int, SZ_INT, op, __VA_ARGS__)
#define FLOAT_OP(op) OP(m_float, SZ_FLOAT, op)

#define LOGICAL(t, sz0, sz, op)\
reg -= sz0;\
*(m_int*)(reg-SZ_INT) = (*(t*)(reg - SZ_INT) op *(t*)(reg+sz));\
DISPATCH()

#define INT_LOGICAL(op) LOGICAL(m_int, SZ_INT, 0, op)

#define FLOAT_LOGICAL(op) LOGICAL(m_float, SZ_FLOAT * 2 - SZ_INT, \
  SZ_FLOAT - SZ_INT, op)

#define SELF(t, sz,op) \
  *(t*)(reg - sz) = op*(t*)(reg - sz);\
  DISPATCH();

#define R(t, sz, op, ...) \
reg -= SZ_INT;\
__VA_ARGS__\
*(t*)(reg-sz) = (**(t**)reg op##= (*(t*)(reg-sz)));\
DISPATCH()
#define INT_R(op, ...) R(m_int, SZ_INT, op, __VA_ARGS__)
#define FLOAT_R(op, ...) R(m_float, SZ_FLOAT, op)

#define INT_PRE(op) \
/*assert(*(m_int**)(reg-SZ_INT));*/\
*(m_int*)(reg- SZ_INT) = op(**(m_int**)(reg-SZ_INT));\
DISPATCH()

#define INT_POST(op) \
/*assert(*(m_int**)(reg-SZ_INT));*/\
*(m_int*)(reg- SZ_INT) = (**(m_int**)(reg-SZ_INT))op;\
DISPATCH()

#define IF_OP(op) \
  reg -=SZ_INT;\
    *(m_float*)(reg-SZ_FLOAT) = (m_float)*(m_int*)(reg-SZ_FLOAT) op \
    *(m_float*)(reg + SZ_INT - SZ_FLOAT); \
  DISPATCH()

#define IF_LOGICAL(op)\
  reg -= SZ_FLOAT; \
  *(m_int*)(reg-SZ_INT) = (*(m_int*)(reg-SZ_INT) op (m_int)*(m_float*)reg); \
  DISPATCH()
__attribute__((hot))

#define IF_R(op) \
  reg -= SZ_INT * 2 - SZ_FLOAT; \
  *(m_float*)(reg-SZ_FLOAT) = (**(m_float**)(reg +SZ_INT - SZ_FLOAT) op##= \
    (m_float)*(m_int*)(reg-SZ_FLOAT)); \
  DISPATCH()

#define FI_OP(op)\
  reg -= SZ_INT; \
  *(m_float*)(reg-SZ_FLOAT) op##= (m_float)*(m_int*)reg; \
  DISPATCH()
  
#define FI_LOGICAL(op) \
  reg -= SZ_FLOAT; \
  *(m_int*)(reg-SZ_INT) = ((m_int)*(m_float*)(reg-SZ_INT) op\
    *(m_int*)(reg + SZ_FLOAT-SZ_INT)); \
  DISPATCH()

#define FI_R(op) \
  reg -= SZ_FLOAT; \
  *(m_int*)(reg-SZ_INT) = (**(m_int**)(reg+SZ_FLOAT -SZ_INT) op##= \
    (m_int)(*(m_float*)(reg-SZ_INT))); \
  DISPATCH()

ANN void vm_run(const VM* vm) { // lgtm [cpp/use-of-goto]
  static const void* dispatch[] = {
    &&regsetimm,
    &&regpushimm, &&regpushfloat, &&regpushother, &&regpushaddr,
    &&regpushmem, &&regpushmemfloat, &&regpushmemother, &&regpushmemaddr,
    &&pushnow,
    &&baseint, &&basefloat, &&baseother, &&baseaddr,
    &&regdup, &&regdup2,
    &&memsetimm,
    &&regpushme, &&regpushmaybe,
    &&funcreturn,
    &&_goto,
    &&allocint, &&allocfloat, &&allocother, &&allocaddr,
    &&intplus, &&intminus, && intmul, &&intdiv, &&intmod,
    // int relationnal
    &&inteq, &&intne, &&intand, &&intor,
    &&intgt, &&intge, &&intlt, &&intle,
    &&intsl, &&intsr, &&intsand, &&intsor, &&intxor,
    &&intnegate, &&intnot, &&intcmp,
    &&intrassign,
    &&intradd, &&intrsub, &&intrmul, &&intrdiv, &&intrmod,
    &&intrsl, &&intrsr, &&intrsand, &&intrsor, &&intrxor,
    &&preinc, &&predec,
    &&postinc, &&postdec,
    &&floatadd, &&floatsub, &&floatmul, &&floatdiv,
// logical
    &&floatand, &&floator, &&floateq, &&floatne,
    &&floatgt, &&floatge, &&floatlt, &&floatle,
    &&floatneg, &&floatnot,
    &&floatrassign, &&floatradd, &&floatrsub, &&floatrmul, &&floatrdiv,
    &&ifadd, &&ifsub, &&ifmul, &&ifdiv,
    &&ifand, &&ifor, &&ifeq, &&ifne, &&ifgt, &&ifge, &&iflt, &&ifle,
    &&ifrassign, &&ifradd, &&ifrsub, &&ifrmul, &&ifrdiv,
    &&fiadd, &&fisub, &&fimul, &&fidiv,
    &&fiand, &&fior, &&fieq, &&fine, &&figt, &&fige, &&filt, &&file,
    &&firassign, &&firadd, &&firsub, &&firmul, &&firdiv,
    &&itof, &&ftoi,
    &&timeadv,
    &&setcode, &&funcptr, &&funcmember,
    &&funcusr, &&regpop, &&regtomem, &&overflow, &&next, &&funcusrend, &&funcmemberend,
    &&sporkini, &&sporkfunc, &&sporkthis, &&sporkexp, &&sporkend,
    &&brancheqint, &&branchneint, &&brancheqfloat, &&branchnefloat,
    &&decintaddr, &&initloop,
    &&arraytop, &&newobj,
    &&addref, &&assign, &&remref,
    &&except, &&allocmemberaddr, &&dotmember, &&dotfloat, &&dotother, &&dotaddr,
    &&staticint, &&staticfloat, &&staticother,
    &&dotfunc, &&dotstaticfunc, &&staticcode, &&pushstr,
    &&gcini, &&gcadd, &&gcend,
    &&gack, &&other
  };
  const Shreduler s = vm->shreduler;
  register VM_Shred shred;
register m_bit next;
  while((shred = shreduler_get(s))) {
register VM_Code code = shred->code;
register m_uint* ip = code->instr->ptr + OFFSET;
register
size_t pc = shred->pc;
register
m_bit* reg = shred->reg;
register
m_bit* mem = shred->mem;
register union {
M_Object obj;
VM_Code code;
VM_Shred child;
} a;

#ifdef VMBENCH
struct timespec exec_ini, exec_end, exec_ret;
clock_gettime(CLOCK_THREAD_CPUTIME_ID, &exec_ini);
#endif
  do {
    register Instr instr; DISPATCH();
regsetimm:
  *(m_uint*)reg = instr->m_val;
  DISPATCH();
regpushimm:
  *(m_uint*)reg = instr->m_val;
  reg += SZ_INT;
  DISPATCH();
regpushfloat:
  *(m_float*)reg = instr->f;
  reg += SZ_FLOAT;
  DISPATCH();
regpushother:
//  LOOP_OPTIM
  for(m_uint i = 0; i <= instr->m_val2; i+= SZ_INT)
    *(m_bit**)(reg+i) = (m_bit*)(instr->m_val + i);
  reg += instr->m_val2;
  DISPATCH();
regpushaddr:
  *(m_uint**)reg =  &instr->m_val;
  reg += SZ_INT;
  DISPATCH()
regpushmem:
  *(m_uint*)reg = *(m_uint*)(mem + instr->m_val);
  reg += SZ_INT;
  DISPATCH();
regpushmemfloat:
  *(m_float*)reg = *(m_float*)(mem + instr->m_val);
  reg += SZ_FLOAT;
  DISPATCH();
regpushmemother:
//  LOOP_OPTIM
  for(m_uint i = 0; i <= instr->m_val2; i+= SZ_INT)
    *(m_uint*)(reg+i) = *(m_uint*)((mem + instr->m_val) + i);
  reg += instr->m_val2;
  DISPATCH();
regpushmemaddr:
  *(m_bit**)reg = (mem + instr->m_val);
  reg += SZ_INT;
  DISPATCH()
pushnow:
  *(m_float*)reg = vm->bbq->pos;
  reg += SZ_FLOAT;
  DISPATCH();
baseint:
  *(m_uint*)reg = *(m_uint*)(shred->base + instr->m_val);
  reg += SZ_INT;
  DISPATCH();
basefloat:
  *(m_float*)reg = *(m_float*)(shred->base + instr->m_val);
  reg += SZ_FLOAT;
  DISPATCH();
baseother:
//  LOOP_OPTIM
  for(m_uint i = 0; i <= instr->m_val2; i+= SZ_INT)
    *(m_uint*)(reg+i) = *(m_uint*)((shred->base + instr->m_val) + i);
  reg += instr->m_val2;
  DISPATCH();
baseaddr:
  *(m_bit**)reg = (shred->base + instr->m_val);
  reg += SZ_INT;
  DISPATCH();
regdup:
  *(m_uint*)reg = *(m_uint*)(reg-SZ_INT);
  reg += SZ_INT;
  DISPATCH()
regdup2:
  *(m_uint*)(reg+SZ_INT) = *(m_uint*)(reg);
  reg += SZ_INT;
  DISPATCH()
memsetimm:
  *(m_uint*)(mem+instr->m_val) = instr->m_val2;
  DISPATCH();
regpushme:
  *(M_Object*)reg = shred->info->me;
  reg += SZ_INT;
  DISPATCH()
regpushmaybe:
  *(m_uint*)reg = gw_rand((uint32_t*)vm->rand) > (UINT32_MAX / 2);
  reg += SZ_INT;
  DISPATCH();
funcreturn:
  pc = *(m_uint*)(mem-SZ_INT*2);
  code = *(VM_Code*)(mem-SZ_INT*3);
  mem -= (*(m_uint*)(mem-SZ_INT*4) + SZ_INT*4);
  ip = code->instr->ptr + OFFSET;
  DISPATCH();
_goto:
  pc = instr->m_val;
  DISPATCH();
allocint:
  *(m_uint*)reg = *(m_uint*)(mem+instr->m_val) = 0;
  reg += SZ_INT;
  DISPATCH()
allocfloat:
  *(m_float*)reg = *(m_float*)(mem+instr->m_val) = 0;
  reg += SZ_FLOAT;
  DISPATCH()
allocother:
//  LOOP_OPTIM
  for(m_uint i = 0; i <= instr->m_val; i += SZ_INT)
    *(m_uint*)(reg+i) = (*(m_uint*)(mem+instr->m_val+i) = 0);
  reg += instr->m_val2;
  DISPATCH()
allocaddr:
  *(m_uint*)(mem+instr->m_val) = 0; // just set object to null in
  *(m_bit**)reg = mem + instr->m_val;
  reg += SZ_INT;
  DISPATCH()
intplus:  INT_OP(+)
intminus: INT_OP(-)
intmul:   INT_OP(*)
intdiv:   INT_OP(/, TEST0(m_int, 0))
intmod:   INT_OP(%, TEST0(m_int, 0))

inteq:   INT_LOGICAL(==)
intne:   INT_LOGICAL(!=)
intand:  INT_LOGICAL(&&)
intor:   INT_LOGICAL(||)
intgt:   INT_LOGICAL(>)
intge:   INT_LOGICAL(>=)
intlt:   INT_LOGICAL(<)
intle:   INT_LOGICAL(<=)
intsl:   INT_LOGICAL(<<)
intsr:   INT_LOGICAL(>>)
intsand: INT_LOGICAL(&)
intsor:  INT_LOGICAL(|)
intxor:  INT_LOGICAL(^)

intnegate:
  *(m_int*)(reg - SZ_INT) *= -1;
  DISPATCH()
intnot: SELF(m_int, SZ_INT, !)
intcmp: SELF(m_int, SZ_INT, ~)

intrassign:
  reg -= SZ_INT;
  **(m_int**)reg = *(m_int*)(reg-SZ_INT);
  DISPATCH()

intradd: INT_R(+)
intrsub: INT_R(-)
intrmul: INT_R(*)
intrdiv: INT_R(/, TEST0(m_int, -SZ_INT))
intrmod: INT_R(%, TEST0(m_int, -SZ_INT))
intrsl: INT_R(<<)
intrsr: INT_R(>>)
intrsand: INT_R(&)
intrsor: INT_R(|)
intrxor: INT_R(^)

preinc: INT_PRE(++)
predec: INT_PRE(--)

postinc: INT_POST(++)
postdec: INT_POST(--)

floatadd: FLOAT_OP(+)
floatsub: FLOAT_OP(-)
floatmul: FLOAT_OP(*)
floatdiv: FLOAT_OP(/)

floatand: FLOAT_LOGICAL(&&)
floator: FLOAT_LOGICAL(||)
floateq: FLOAT_LOGICAL(==)
floatne: FLOAT_LOGICAL(!=)
floatgt: FLOAT_LOGICAL(>)
floatge: FLOAT_LOGICAL(>=)
floatlt: FLOAT_LOGICAL(<)
floatle: FLOAT_LOGICAL(<=)

floatneg: SELF(m_float, SZ_FLOAT, -)

floatnot:
  reg -= SZ_FLOAT - SZ_INT;
  *(m_int*)(reg - SZ_INT) = !*(m_float*)(reg - SZ_INT);
  DISPATCH()

floatrassign:
  reg -= SZ_INT;
  **(m_float**)reg = *(m_float*)(reg-SZ_FLOAT);
  DISPATCH()

floatradd: FLOAT_R(+)
floatrsub: FLOAT_R(-)
floatrmul: FLOAT_R(*)
floatrdiv: FLOAT_R(/)

ifadd: IF_OP(+)
ifsub: IF_OP(-)
ifmul: IF_OP(*)
ifdiv: IF_OP(/)

ifand: IF_LOGICAL(&&)
ifor: IF_LOGICAL(||)
ifeq: IF_LOGICAL(==)
ifne: IF_LOGICAL(!=)
ifgt: IF_LOGICAL(>)
ifge: IF_LOGICAL(>=)
iflt: IF_LOGICAL(<)
ifle: IF_LOGICAL(<=)

ifrassign: IF_R()
ifradd: IF_R(+)
ifrsub: IF_R(-)
ifrmul: IF_R(*)
ifrdiv: IF_R(/)

fiadd: FI_OP(+)
fisub: FI_OP(-)
fimul: FI_OP(*)
fidiv: FI_OP(/)

fiand: FI_LOGICAL(&&)
fior: FI_LOGICAL(||)
fieq: FI_LOGICAL(==)
fine: FI_LOGICAL(!=)
figt: FI_LOGICAL( >)
fige: FI_LOGICAL(>=)
filt: FI_LOGICAL( <)
file: FI_LOGICAL(<=)

firassign:
  reg -=SZ_FLOAT;
  *(m_int*)(reg-SZ_INT) = **(m_int**)(reg + SZ_FLOAT-SZ_INT) =
    (m_int)*(m_float*)(reg-SZ_INT);
  DISPATCH()

firadd: FI_R(+)
firsub: FI_R(-)
firmul: FI_R(*)
firdiv: FI_R(/)

itof:
  reg -= SZ_INT - SZ_FLOAT;
  *(m_float*)(reg-SZ_FLOAT) = (m_float)*(m_int*)(reg-SZ_FLOAT);
  DISPATCH()
ftoi:
  reg -= SZ_FLOAT - SZ_INT;
  *(m_int*)(reg-SZ_INT) = (m_int)*(m_float*)(reg-SZ_INT);
  DISPATCH()

timeadv:
  reg -= SZ_FLOAT;
{
  register const m_float f = *(m_float*)(reg-SZ_FLOAT);
  *(m_float*)(reg-SZ_FLOAT) = (shred->tick->wake_time += f);
  shredule(s, shred, f);
}
shred->code = code;
shred->reg = reg;
shred->mem = mem;
shred->pc = pc;
  break;
setcode:
  a.code = *(VM_Code*)(reg-SZ_INT);
funcptr:
  if(!GET_FLAG((VM_Code)a.code, builtin))
    goto funcusr;
funcmember:
  reg -= SZ_INT;
  a.code = *(VM_Code*)reg;
  mem += *(m_uint*)(reg + SZ_INT);
  next = eFuncMemberEnd;
  goto *dispatch[eRegPop];
funcusr:
{
  reg -= SZ_INT;
  a.code = *(VM_Code*)reg;
  register const m_uint push = *(m_uint*)(reg + SZ_INT) + *(m_uint*)(mem-SZ_INT);
  mem += push;
  *(m_uint*)  mem = push;mem += SZ_INT;
  *(VM_Code*) mem = code; mem += SZ_INT;
  *(m_uint*)  mem = pc + instr->m_val2; mem += SZ_INT;
  *(m_uint*) mem = a.code->stack_depth; mem += SZ_INT;
  next = eFuncUsrEnd;
}
regpop:
  reg -= instr->m_val;
  DISPATCH();
regtomem:
  *(m_uint*)(mem+instr->m_val) = *(m_uint*)(reg+instr->m_val2);
  DISPATCH()
overflow:
  if(overflow_(mem, shred))
    Except(shred, "StackOverflow");
next:
  goto *dispatch[next];
funcusrend:
  ip = (code = a.code)->instr->ptr + OFFSET;
  pc = 0;
  DISPATCH();
funcmemberend:
  shred->mem = mem;
  shred->reg = reg;
  shred->pc = pc;
  shred->code = code;
  ((f_mfun)a.code->native_func)((*(M_Object*)mem), reg, shred);
  reg += instr->m_val;
  shred->mem = (mem -= instr->m_val2);
  if(!s->curr)break;
  pc = shred->pc;
  DISPATCH()
sporkini:
  a.child = init_spork_shred(shred, (VM_Code)instr->m_val);
  DISPATCH()
sporkfunc:
//  LOOP_OPTIM
  for(m_uint i = 0; i < instr->m_val; i+= SZ_INT)
    *(m_uint*)(a.child->reg + i) = *(m_uint*)(reg + i - SZ_INT);
  a.child->reg += instr->m_val;
  DISPATCH()
sporkthis:
  *(M_Object*)a.child->reg = *(M_Object*)(reg -SZ_INT + instr->m_val);
  a.child->reg += SZ_INT;
  DISPATCH()
sporkexp:
//  LOOP_OPTIM
  for(m_uint i = 0; i < instr->m_val; i+= SZ_INT)
    *(m_uint*)(a.child->mem + i) = *(m_uint*)(mem+i);
  DISPATCH()
sporkend:
  *(M_Object*)(reg-SZ_INT) = a.child->info->me;
  DISPATCH()
brancheqint:
  reg -= SZ_INT;
  if(!*(m_uint*)reg)
    pc = instr->m_val;
  DISPATCH();
branchneint:
  reg -= SZ_INT;
  if(*(m_uint*)reg)
    pc = instr->m_val;
  DISPATCH();
brancheqfloat:
  reg -= SZ_FLOAT;
  if(!*(m_float*)reg)
    pc = instr->m_val;
  DISPATCH();
branchnefloat:
  reg -= SZ_FLOAT;
  if(*(m_float*)reg)
    pc = instr->m_val;
  DISPATCH();
decintaddr:
  --(*((m_uint*)(instr->m_val)));
  DISPATCH()
initloop:
  reg -= SZ_INT;
  (*(m_uint*)instr->m_val) = labs(*(m_int*)reg);
  DISPATCH()
arraytop:
  if(*(m_uint*)(reg - SZ_INT * 2) < *(m_uint*)(reg-SZ_INT))
    goto newobj;
  else
    goto _goto;
newobj:
  *(M_Object*)reg = new_object(shred, (Type)instr->m_val2);
  reg += SZ_INT;
  DISPATCH()
addref:
  if((a.obj = instr->m_val ? **(M_Object**)(reg-SZ_INT) :
    *(M_Object*)(reg-SZ_INT)))
    ++a.obj->ref;
  DISPATCH()
assign:
  reg -= SZ_INT;
  a.obj = *(M_Object*)(reg-SZ_INT);
  const M_Object tgt = **(M_Object**)reg;
  if(tgt) {
    --tgt->ref;
    _release(tgt, shred);
  }
  **(M_Object**)reg = a.obj;
  DISPATCH()
remref:
  release(*(M_Object*)(mem + instr->m_val), shred);
  DISPATCH()
except:
  if(!(a.obj  = *(M_Object*)(reg-SZ_INT)))
    Except(shred, "NullPtrException");
  DISPATCH();
allocmemberaddr:
  a.obj = *(M_Object*)mem;
  *(m_bit**)reg = a.obj->data + instr->m_val;
  reg += SZ_INT;
  DISPATCH()
dotmember:
  *(m_uint*)(reg-SZ_INT) = *(m_uint*)(a.obj->data + instr->m_val);
  DISPATCH()
dotfloat:
  *(m_float*)(reg-SZ_INT) = *(m_float*)(a.obj->data + instr->m_val);
  reg += SZ_FLOAT - SZ_INT;
  DISPATCH()
dotother:
//  LOOP_OPTIM
  for(m_uint i = 0; i <= instr->m_val2; i += SZ_INT)
    *(m_uint*)(reg+i-SZ_INT) = *(m_uint*)((a.obj->data + instr->m_val) + i);
  reg += instr->m_val2 - SZ_INT;
  DISPATCH()
dotaddr:
  *(m_bit**)(reg-SZ_INT) = (a.obj->data + instr->m_val);
  DISPATCH()
staticint:
  *(m_uint*)reg = *(m_uint*)instr->m_val;
  reg += SZ_INT;
  DISPATCH()
staticfloat:
  *(m_float*)reg = *(m_float*)instr->m_val;
  reg += SZ_FLOAT;
  DISPATCH()
staticother:
//  LOOP_OPTIM
  for(m_uint i = 0; i <= instr->m_val2; i += SZ_INT)
    *(m_uint*)(reg+i) = *(m_uint*)((m_bit*)instr->m_val + i);
  reg += instr->m_val2;
  DISPATCH()
dotfunc:
  assert(a.obj);
  reg += SZ_INT;
dotstaticfunc:
  *(VM_Code*)(reg-SZ_INT) = ((Func)vector_at(a.obj->vtable, instr->m_val))->code;
  DISPATCH()
staticcode:
  instr->m_val = (m_uint)(a.code = (*(VM_Code*)reg = ((Func)instr->m_val)->code));
  instr->opcode = eRegPushImm;
  reg += SZ_INT;
  DISPATCH()
pushstr:
  *(M_Object*)reg = new_string2(shred, (m_str)instr->m_val);
  reg += SZ_INT;
  DISPATCH();
gcini:
  vector_add(&shred->gc, 0);
  DISPATCH();
gcadd:
  vector_add(&shred->gc, *(vtype*)(reg-SZ_INT));
  DISPATCH();
gcend:
  while((a.obj = (M_Object)vector_pop(&shred->gc)))
    _release(a.obj, shred);
  DISPATCH()
gack:
  gack(reg, instr);
  DISPATCH()
other:
shred->code = code;
shred->reg = reg;
shred->mem = mem;
shred->pc = pc;
      instr->execute(shred, instr);
if(!s->curr)break;
code = shred->code;
ip = shred->code->instr->ptr + OFFSET;
reg = shred->reg;
mem = shred->mem;
pc = shred->pc;
DISPATCH()
    } while(s->curr);
#ifdef VMBENCH
clock_gettime(CLOCK_THREAD_CPUTIME_ID, &exec_end);
timespecsub(&exec_end, &exec_ini, &exec_ret);
timespecadd(&exec_time, &exec_ret, &exec_time);
#endif
  }
  if(!vm->bbq->is_running) {
#ifdef VMBENCH
    printf("[VM] exec time %lu.%09lu\n", exec_time.tv_sec, exec_time.tv_nsec);
    printf("[VM] exec time %09lu\n", exec_time.tv_nsec/1000);
#endif
    return;
}
  vm_ugen_init(vm);
}
