#include <stdio.h>

#include "../../mato_config.h"

int main()
{
    void *cfg = mato_config_read("08_mato_config/config_test.cfg");
    printf("abc='%s'\n", mato_config_get_strval(cfg, "abc", "xx"));
    printf("def='%s'\n", mato_config_get_strval(cfg, "def", "xx"));
    printf("ghi='%d'\n", mato_config_get_intval(cfg, "ghi", 777));
    printf("jkl='%d'\n", mato_config_get_intval(cfg, "jkl", 999));
    printf("mno='%G'\n", mato_config_get_doubleval(cfg, "mno", 777.999));
    printf("pqr='%G'\n", mato_config_get_doubleval(cfg, "pqr", 999.777));
    printf("this_is_one='%d'\n", mato_config_get_intval(cfg, "this_is_one", 999));
    printf("this_is_zero='%d'\n", mato_config_get_intval(cfg, "this_is_zero", 999));
  
    mato_config_dispose(cfg);
    return 0;
}


