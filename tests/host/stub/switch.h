// Minimal libnx stand-ins so the overlay's non-UI code can run on a PC.
#pragma once
#include <cstdint>
#include <cstddef>
#include <ctime>
#include <pthread.h>
#include <random>

typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef int8_t s8; typedef int16_t s16; typedef int32_t s32; typedef int64_t s64;
typedef u32 Result;
#define R_SUCCEEDED(r) ((r) == 0)
#define R_FAILED(r) ((r) != 0)

typedef void (*ThreadFunc)(void*);
struct Thread { pthread_t t; ThreadFunc fn; void* arg; };

inline void* _stubThreadTrampoline(void* p) { auto* t = static_cast<Thread*>(p); t->fn(t->arg); return nullptr; }
inline Result threadCreate(Thread* t, ThreadFunc fn, void* arg, void*, size_t, int, int) { t->fn = fn; t->arg = arg; return 0; }
inline Result threadStart(Thread* t) { return pthread_create(&t->t, nullptr, _stubThreadTrampoline, t) == 0 ? 0 : 1; }
inline Result threadWaitForExit(Thread* t) { pthread_join(t->t, nullptr); return 0; }
inline Result threadClose(Thread*) { return 0; }

inline u64 armGetSystemTick() { timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return u64(ts.tv_sec) * 1000000000ULL + ts.tv_nsec; }
inline u64 armTicksToNs(u64 t) { return t; }

inline Result nifmGetCurrentIpAddress(u32* ip) { *ip = 0x0100007F; return 0; }  // 127.0.0.1
inline u64 randomGet64() { static std::mt19937_64 rng{std::random_device{}()}; return rng(); }
