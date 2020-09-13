#include <sys/time.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <math.h>
#include <string.h>
#include <pthread.h>
#include <stdbool.h>
#include <libxml/xmlreader.h>
#include <sys/time.h>

double rad_normAlpha(double alpha){
   if(alpha < 0){
      while(alpha < 0)
         alpha += 2.0 * M_PI;
   }
   else
      while(alpha >= 2.0 * M_PI)
         alpha -= 2.0 * M_PI;
   return alpha;
}

