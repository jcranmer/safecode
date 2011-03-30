/*
 * RUN: sh test.sh %s
 * XFAIL: *
 */

/* strncat() when the source array overlaps with the destination string
 * starting after the destination string, and nul is one of the n
 * characters to concatenate. */

#include <string.h>

int main()
{
  char string[20] = "a string";
  strncat(string, &string[2], 10);
  return 0;
}
