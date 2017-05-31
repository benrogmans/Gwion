#ifndef DL_H
#define DL_H

#include <stdlib.h>
#include <complex.h>
#include "defs.h"
#include "vm.h"

typedef struct DL_Return {
  union {
    m_uint    v_uint;
    m_float   v_float;
    m_complex v_complex;
    VEC3_T 	  v_vec3;
    VEC4_T 	  v_vec4;
    M_Object  v_object;
  } d;
  m_uint offset;
} DL_Return;

void dl_return_push(const DL_Return retval, VM_Shred shred, int kind);

typedef void (*f_xtor)(M_Object o, VM_Shred sh);
typedef void (*f_mfun)(M_Object o, DL_Return * RETURN, VM_Shred sh);
typedef void (*f_sfun)(DL_Return * RETURN, VM_Shred sh);
typedef m_bool(*f_init)(Env env);

typedef struct {
  m_str name;
  m_str type;
  m_bool is_const;
  void * static_addr;
  m_str doc;
} DL_Value;

typedef struct {
  m_str name;
  m_str type;
  union {
    f_xtor ctor;
    f_xtor dtor;
    f_mfun mfun;
    f_sfun sfun;
    m_uint addr;
  } d;
  Vector args;
} DL_Func;


DL_Func* new_DL_Func(const m_str t, const m_str n, m_uint addr);
void free_DL_Func(DL_Func* a);
DL_Value* dl_func_add_arg(DL_Func* a, const m_str t, const m_str  n);
#endif
