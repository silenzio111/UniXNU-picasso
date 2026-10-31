// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include "../DarwinBootPkg/Driver/WindowSize.h"
int main(void) {
 const unsigned long long yes[]={0x10000000ULL,0xe0000000ULL,0x100000000ULL,0x130000000ULL,0x200000000ULL};
 const unsigned long long no[]={0,0xfffffffULL,0x10000001ULL,0x130001000ULL,0x200200000ULL,~0ULL};
 for(unsigned i=0;i<sizeof(yes)/sizeof(yes[0]);++i)assert(DarwinWindowSizeValid(yes[i]));
 for(unsigned i=0;i<sizeof(no)/sizeof(no[0]);++i)assert(!DarwinWindowSizeValid(no[i]));
}
