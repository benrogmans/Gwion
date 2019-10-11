#include "gwion_util.h"
#include "gwion_ast.h"
#include "oo.h"
#include "vm.h"
#include "env.h"
#include "type.h"
#include "instr.h"
#include "object.h"
#include "gwion.h"
#include "value.h"
#include "operator.h"
#include "import.h"
#include "instr.h"

GWION_IMPORT(too_many_args) {
  GWI_BB(gwi_func_ini(gwi, "int", "test", (f_xfun)1))
  GWI_BB(gwi_func_arg(gwi, "int", "i[][]"))
  GWI_BB(gwi_func_arg(gwi, "Int", "i"))
  GWI_BB(gwi_func_end(gwi, 0))
  return GW_OK;
}
