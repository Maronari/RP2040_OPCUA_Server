#include "services/resource_exhaustion_test.h"
#include <assert.h>

int main(void)
{
    resource_exhaustion_test_result_t result;
    assert(!resource_exhaustion_test_run(&result));
    return 0;
}
