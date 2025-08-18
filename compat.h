// compat.h  — tiny cross-platform shim (C, not C++)
#pragma once

#if defined(_WIN32)
  #include <windows.h>
  #include <process.h>
  // sleep/usleep
  #define sleep(s)   Sleep((DWORD)((s) * 1000))
  static inline void usleep_win(unsigned int us){ Sleep((DWORD)(us/1000)); }
  #define usleep(us) usleep_win((us))

  // mutex
  typedef CRITICAL_SECTION mutex_t;
  #define MUTEX_INIT(m)     InitializeCriticalSection(&(m))
  #define MUTEX_LOCK(m)     EnterCriticalSection(&(m))
  #define MUTEX_UNLOCK(m)   LeaveCriticalSection(&(m))
  #define MUTEX_DESTROY(m)  DeleteCriticalSection(&(m))

  // threads
  typedef unsigned(__stdcall *thread_fn)(void*);
  typedef HANDLE thread_t;
  static inline void thread_start_detached(thread_fn fn, void* arg) {
      uintptr_t h = _beginthreadex(NULL, 0, fn, arg, 0, NULL);
      if (h) CloseHandle((HANDLE)h);
  }

#else
  // POSIX
  #include <pthread.h>
  #include <unistd.h>
  typedef pthread_mutex_t mutex_t;
  #define MUTEX_INIT(m)     pthread_mutex_init(&(m), NULL)
  #define MUTEX_LOCK(m)     pthread_mutex_lock(&(m))
  #define MUTEX_UNLOCK(m)   pthread_mutex_unlock(&(m))
  #define MUTEX_DESTROY(m)  pthread_mutex_destroy(&(m))

  typedef void* (*thread_fn)(void*);
  typedef pthread_t thread_t;
  static inline void thread_start_detached(thread_fn fn, void* arg) {
      pthread_t t; pthread_create(&t, NULL, fn, arg); pthread_detach(t);
  }
#endif
