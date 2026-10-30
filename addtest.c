#include "types.h"
#include "stat.h"
#include "user.h"

int
main(void)
{
  printf(1, "add(3, 4) = %d\n", add(3, 4));
  printf(1, "add(-3, 4) = %d\n", add(-3, 4));
  exit();
}