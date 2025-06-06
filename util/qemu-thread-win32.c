/*
 * Win32 implementation for mutex/cond/thread functions
 *
 * Copyright Red Hat, Inc. 2010
 *
 * Author:
 *  Paolo Bonzini <pbonzini@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#include "qemu/osdep.h"
#include "qemu/thread.h"
#include "qemu/notify.h"
#include "qemu-thread-common.h"
#include <process.h>

static bool name_threads;

typedef HRESULT (WINAPI *pSetThreadDescription) (HANDLE hThread,
                                                 PCWSTR lpThreadDescription);
static pSetThreadDescription SetThreadDescriptionFunc;
static HMODULE kernel32_module;

static bool load_set_thread_description(void)
{
    static gsize _init_once = 0;

    if (g_once_init_enter(&_init_once)) {
        kernel32_module = LoadLibrary("kernel32.dll");
        if (kernel32_module) {
            SetThreadDescriptionFunc =
                (pSetThreadDescription)GetProcAddress(kernel32_module,
                                                      "SetThreadDescription");
            if (!SetThreadDescriptionFunc) {
                FreeLibrary(kernel32_module);
            }
        }
        g_once_init_leave(&_init_once, 1);
    }

    return !!SetThreadDescriptionFunc;
}

void qemu_thread_naming(bool enable)
{
    name_threads = enable;

    if (enable && !load_set_thread_description()) {
        fprintf(stderr, "qemu: thread naming not supported on this host\n");
        name_threads = false;
    }
}

static void error_exit(int err, const char *msg)
{
    char *pstr;

    FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER,
                  NULL, err, 0, (LPTSTR)&pstr, 2, NULL);
    fprintf(stderr, "qemu: %s: %s\n", msg, pstr);
    LocalFree(pstr);
    abort();
}

void qemu_mutex_init(QemuMutex *mutex)
{
    InitializeSRWLock(&mutex->lock);
    qemu_mutex_post_init(mutex);
}

void qemu_mutex_destroy(QemuMutex *mutex)
{
    assert(mutex->initialized);
    mutex->initialized = false;
    InitializeSRWLock(&mutex->lock);
}

void qemu_mutex_lock_impl(QemuMutex *mutex, const char *file, const int line)
{
    assert(mutex->initialized);
    qemu_mutex_pre_lock(mutex, file, line);
    AcquireSRWLockExclusive(&mutex->lock);
    qemu_mutex_post_lock(mutex, file, line);
}

int qemu_mutex_trylock_impl(QemuMutex *mutex, const char *file, const int line)
{
    int owned;

    assert(mutex->initialized);
    owned = TryAcquireSRWLockExclusive(&mutex->lock);
    if (owned) {
        qemu_mutex_post_lock(mutex, file, line);
        return 0;
    }
    return -EBUSY;
}

void qemu_mutex_unlock_impl(QemuMutex *mutex, const char *file, const int line)
{
    assert(mutex->initialized);
    qemu_mutex_pre_unlock(mutex, file, line);
    ReleaseSRWLockExclusive(&mutex->lock);
}

void qemu_rec_mutex_init(QemuRecMutex *mutex)
{
    InitializeCriticalSection(&mutex->lock);
    mutex->initialized = true;
}

void qemu_rec_mutex_destroy(QemuRecMutex *mutex)
{
    assert(mutex->initialized);
    mutex->initialized = false;
    DeleteCriticalSection(&mutex->lock);
}

void qemu_rec_mutex_lock_impl(QemuRecMutex *mutex, const char *file, int line)
{
    assert(mutex->initialized);
    EnterCriticalSection(&mutex->lock);
}

int qemu_rec_mutex_trylock_impl(QemuRecMutex *mutex, const char *file, int line)
{
    assert(mutex->initialized);
    return !TryEnterCriticalSection(&mutex->lock);
}

void qemu_rec_mutex_unlock_impl(QemuRecMutex *mutex, const char *file, int line)
{
    assert(mutex->initialized);
    LeaveCriticalSection(&mutex->lock);
}

void qemu_cond_init(QemuCond *cond)
{
    memset(cond, 0, sizeof(*cond));
    InitializeConditionVariable(&cond->var);
    cond->initialized = true;
}

void qemu_cond_destroy(QemuCond *cond)
{
    assert(cond->initialized);
    cond->initialized = false;
    InitializeConditionVariable(&cond->var);
}

void qemu_cond_signal(QemuCond *cond)
{
    assert(cond->initialized);
    WakeConditionVariable(&cond->var);
}

void qemu_cond_broadcast(QemuCond *cond)
{
    assert(cond->initialized);
    WakeAllConditionVariable(&cond->var);
}

void qemu_cond_wait_impl(QemuCond *cond, QemuMutex *mutex, const char *file, const int line)
{
    assert(cond->initialized);
    qemu_mutex_pre_unlock(mutex, file, line);
    SleepConditionVariableSRW(&cond->var, &mutex->lock, INFINITE, 0);
    qemu_mutex_post_lock(mutex, file, line);
}

bool qemu_cond_timedwait_impl(QemuCond *cond, QemuMutex *mutex, int ms,
                              const char *file, const int line)
{
    int rc = 0;

    assert(cond->initialized);
    trace_qemu_mutex_unlock(mutex, file, line);
    if (!SleepConditionVariableSRW(&cond->var, &mutex->lock, ms, 0)) {
        rc = GetLastError();
    }
    trace_qemu_mutex_locked(mutex, file, line);
    if (rc && rc != ERROR_TIMEOUT) {
        error_exit(rc, __func__);
    }
    return rc != ERROR_TIMEOUT;
}

void qemu_sem_init(QemuSemaphore *sem, int init)
{
    /* Manual reset.  */
    sem->sema = CreateSemaphore(NULL, init, LONG_MAX, NULL);
    sem->initialized = true;
}

void qemu_sem_destroy(QemuSemaphore *sem)
{
    assert(sem->initialized);
    sem->initialized = false;
    CloseHandle(sem->sema);
}

void qemu_sem_post(QemuSemaphore *sem)
{
    assert(sem->initialized);
    ReleaseSemaphore(sem->sema, 1, NULL);
}

int qemu_sem_timedwait(QemuSemaphore *sem, int ms)
{
    int rc;

    assert(sem->initialized);
    rc = WaitForSingleObject(sem->sema, ms);
    if (rc == WAIT_OBJECT_0) {
        return 0;
    }
    if (rc != WAIT_TIMEOUT) {
        error_exit(GetLastError(), __func__);
    }
    return -1;
}

void qemu_sem_wait(QemuSemaphore *sem)
{
    assert(sem->initialized);
    if (WaitForSingleObject(sem->sema, INFINITE) != WAIT_OBJECT_0) {
        error_exit(GetLastError(), __func__);
    }
}

struct QemuThreadData {
    /* Passed to win32_start_routine.  */
    void             *(*start_routine)(void *);
    void             *arg;
    short             mode;
    NotifierList      exit;

    /* Only used for joinable threads. */
    bool              exited;
    void             *ret;
    CRITICAL_SECTION  cs;
};

static bool atexit_registered;
static NotifierList main_thread_exit;

static __thread QemuThreadData *qemu_thread_data;

static void run_main_thread_exit(void)
{
    notifier_list_notify(&main_thread_exit, NULL);
}

void qemu_thread_atexit_add(Notifier *notifier)
{
    if (!qemu_thread_data) {
        if (!atexit_registered) {
            atexit_registered = true;
            atexit(run_main_thread_exit);
        }
        notifier_list_add(&main_thread_exit, notifier);
    } else {
        notifier_list_add(&qemu_thread_data->exit, notifier);
    }
}

void qemu_thread_atexit_remove(Notifier *notifier)
{
    notifier_remove(notifier);
}

static unsigned __stdcall win32_start_routine(void *arg)
{
    QemuThreadData *data = (QemuThreadData *) arg;
    void *(*start_routine)(void *) = data->start_routine;
    void *thread_arg = data->arg;

    qemu_thread_data = data;
    qemu_thread_exit(start_routine(thread_arg));
    abort();
}

void qemu_thread_exit(void *arg)
{
    QemuThreadData *data = qemu_thread_data;

    notifier_list_notify(&data->exit, NULL);
    if (data->mode == QEMU_THREAD_JOINABLE) {
        data->ret = arg;
        EnterCriticalSection(&data->cs);
        data->exited = true;
        LeaveCriticalSection(&data->cs);
    } else {
        g_free(data);
    }
    _endthreadex(0);
}

void *qemu_thread_join(QemuThread *thread)
{
    QemuThreadData *data;
    void *ret;
    HANDLE handle;

    data = thread->data;
    if (data->mode == QEMU_THREAD_DETACHED) {
        return NULL;
    }

    /*
     * Because multiple copies of the QemuThread can exist via
     * qemu_thread_get_self, we need to store a value that cannot
     * leak there.  The simplest, non racy way is to store the TID,
     * discard the handle that _beginthreadex gives back, and
     * get another copy of the handle here.
     */
    handle = qemu_thread_get_handle(thread);
    if (handle) {
        WaitForSingleObject(handle, INFINITE);
        CloseHandle(handle);
    }
    ret = data->ret;
    DeleteCriticalSection(&data->cs);
    g_free(data);
    return ret;
}

static bool set_thread_description(HANDLE h, const char *name)
{
    HRESULT hr;
    g_autofree wchar_t *namew = NULL;

    if (!load_set_thread_description()) {
        return false;
    }

    namew = g_utf8_to_utf16(name, -1, NULL, NULL, NULL);
    if (!namew) {
        return false;
    }

    hr = SetThreadDescriptionFunc(h, namew);

    return SUCCEEDED(hr);
}

void qemu_thread_create(QemuThread *thread, const char *name,
                       void *(*start_routine)(void *),
                       void *arg, int mode)
{
    HANDLE hThread;
    struct QemuThreadData *data;

    data = g_malloc(sizeof *data);
    data->start_routine = start_routine;
    data->arg = arg;
    data->mode = mode;
    data->exited = false;
    notifier_list_init(&data->exit);

    if (data->mode != QEMU_THREAD_DETACHED) {
        InitializeCriticalSection(&data->cs);
    }

    hThread = (HANDLE) _beginthreadex(NULL, 0, win32_start_routine,
                                      data, 0, &thread->tid);
    if (!hThread) {
        error_exit(GetLastError(), __func__);
    }
    if (name_threads && name && !set_thread_description(hThread, name)) {
        fprintf(stderr, "qemu: failed to set thread description: %s\n", name);
    }
    CloseHandle(hThread);

    thread->data = data;
}

int qemu_thread_set_affinity(QemuThread *thread, unsigned long *host_cpus,
                             unsigned long nbits)
{
    return -ENOSYS;
}

int qemu_thread_get_affinity(QemuThread *thread, unsigned long **host_cpus,
                             unsigned long *nbits)
{
    return -ENOSYS;
}

void qemu_thread_get_self(QemuThread *thread)
{
    thread->data = qemu_thread_data;
    thread->tid = GetCurrentThreadId();
}

HANDLE qemu_thread_get_handle(QemuThread *thread)
{
    QemuThreadData *data;
    HANDLE handle;

    data = thread->data;
    if (data->mode == QEMU_THREAD_DETACHED) {
        return NULL;
    }

    EnterCriticalSection(&data->cs);
    if (!data->exited) {
        handle = OpenThread(SYNCHRONIZE | THREAD_SUSPEND_RESUME |
                            THREAD_SET_CONTEXT, FALSE, thread->tid);
    } else {
        handle = NULL;
    }
    LeaveCriticalSection(&data->cs);
    return handle;
}

bool qemu_thread_is_self(QemuThread *thread)
{
    return GetCurrentThreadId() == thread->tid;
}
