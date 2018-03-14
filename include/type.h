#ifndef __TYPE
#define __TYPE

#include "nspc.h"
#include "env.h"

struct Type_ {
  m_str     name;
  m_uint    size;
  Type      parent;
  m_uint    xid;
  Nspc      info;
  Nspc      owner;
  m_uint    array_depth;
  Class_Def def;
  union type_data {
    Func      func;
    Type      base_type;
  } d;
  m_uint flag;
  struct VM_Object_ obj;
};

m_bool type_engine_check_prog(Env, Ast, const m_str) ANN;
Type new_type(const m_uint xid, const m_str name, const Type);
Type type_copy(const Type type) ANN;
Env type_engine_init(VM*, const Vector) ANN;
void start_type_xid(void);
Value find_value(const Type, const Symbol);
Func find_func(const Type, const Symbol) ANN;
Type find_type(const Env, ID_List) ANN;
const m_bool isa(const Type, const Type) ANN;
const m_bool isres(const Symbol, const m_uint) ANN;
const Type array_type(const Type, const m_uint) ANN;
const Type get_array(const Type, const Array_Sub, const m_str);
const Type find_common_anc(const Type, const Type) ANN;
const m_uint id_list_len(ID_List) ANN;
void type_path(m_str, const ID_List) ANN;
const m_bool env_add_value(Env env, const m_str, const Type, const m_bool, void* value);
const m_bool env_add_type(Env, const Type) ANN;
const m_int str2char(const m_str, const m_int) ANN;
const m_uint num_digit(const m_uint);
const Type array_base(Type) ANN;
const m_bool type_ref(Type) ANN;
const m_bool prim_ref(const Type_Decl*, const Type) ANN;

extern struct Type_ t_int, t_float, t_dur, t_time, t_now;
extern struct Type_ t_complex, t_polar, t_vec3, t_vec4;
extern struct Type_ t_function, t_func_ptr;
extern struct Type_ t_void, t_class, t_null, t_union;
extern struct Type_ t_object, t_shred, t_event, t_ugen, t_array;
extern struct Type_ t_vararg, t_string, t_ptr, t_gack;
#endif

