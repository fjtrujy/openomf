#include <CUnit/CUnit.h>
#include <CUnit/Basic.h>
#include <shadowdive/shadowdive.h>

void af_test_suite(CU_pSuite suite);
void bk_test_suite(CU_pSuite suite);

int main(int argc, char **argv) {
    CU_pSuite suite = NULL;

    if(CU_initialize_registry() != CUE_SUCCESS) {
        return CU_get_error();
    }

    suite = CU_add_suite("AF Reading and writing", NULL, NULL);
    if(suite == NULL) goto end;
    af_test_suite(suite);

    suite = CU_add_suite("BK Reading and writing", NULL, NULL);
    if(suite == NULL) goto end;
    bk_test_suite(suite);

    // Run tests
    CU_basic_set_mode(CU_BRM_VERBOSE);
    CU_basic_run_tests();
   
end:
    CU_cleanup_registry();
    return CU_get_error();
}
