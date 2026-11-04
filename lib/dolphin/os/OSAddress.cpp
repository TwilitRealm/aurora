#include <dolphin/os.h>

void* OSCachedToUncached(void* caddr) {
  return caddr;
}

void* OSUncachedToCached(void* ucaddr) {
  return ucaddr;
}
