#include "gwion_util.h"
#include "gwion_ast.h"
#include "oo.h"
#include "vm.h"
#include "env.h"
#include "type.h"
#include "object.h"
#include "instr.h"
#include "gwion.h"
#include "operator.h"
#include "import.h"

GWION_IMPORT(typedef_test) {
  GWI_OB(gwi_typedef_ini(gwi, "int", "Typedef"))
  GWI_OB(gwi_typedef_end(gwi, ae_flag_none))
  return GW_OK;
}
