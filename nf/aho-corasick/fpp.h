#pragma once
#ifndef AHO_CORASICK_FPP_H
#define AHO_CORASICK_FPP_H
#define FPP_EXPENSIVE(x)	{}					// Just a hint
#define FPP_ISSET(n, i) (n & (1 << i))
#define FPP_SET(n, i) (n | (1 << i))	// Set the ith bit of n
	
// Prefetch, Save, and Switch
#define FPP_PSS(addr, label) \
do {\
	__builtin_prefetch(addr, 0, 0); \
	batch_rips[I] = &&label; \
	I = (I + 1) & BATCH_SIZE_; \
	goto *batch_rips[I]; \
} while(0)

#define BATCH_SIZE 1
#define BATCH_SIZE_ 3

#define foreach(i, n) for(i = 0; i < n; i++)


#endif
