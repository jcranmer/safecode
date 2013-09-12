// RUN: test.sh -e -t %t %s silly
//
// TEST: env-001
//
// Description:
//  Test that array bounds checking works on environment strings
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int
main (int argc, char ** argv, char ** env) {
  int q = 0;
  for (q = 0; env[q]; q++) {
    printf("env[%d]: %p %p\n", q, env[q], env[q] + strlen(env[q]));
  }
  int index = 0;
  while (env[1])
    env++;
  for (index = 0; index < strlen (env[0]) + 5; ++index) {
    printf ("%c %c", env[0][index], argv[0]);
  }

  return 0;
}

