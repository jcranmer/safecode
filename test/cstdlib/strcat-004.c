/*
 * RUN: sh test.sh %s
 * XFAIL: *
 */

/* Concatenate onto destination buffer that is too short. */

#include <string.h>

int main()
{
  char buf[1];
  buf[0] = '\0';
  strcat(buf, "s");
  return 0;
}
