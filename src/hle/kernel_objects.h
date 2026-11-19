/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Registry of HLE kernel objects handed to the guest as opaque handles.
 *
 * Handles are host pointers stored in guest memory. Every live object is
 * registered, so a garbage handle is rejected instead of dereferenced. Each
 * object struct starts with its obj_kind. */
#ifndef PS5C_KERNEL_OBJECTS_H
#define PS5C_KERNEL_OBJECTS_H

#include "base/base.h"

typedef enum {
    OBJ_MUTEX = 1,
    OBJ_MUTEXATTR,
    OBJ_COND,
    OBJ_CONDATTR,
    OBJ_SEMA,
    OBJ_THREADATTR,
    OBJ_RWLOCK,
    OBJ_EQUEUE,
    OBJ_EVENTFLAG,
} obj_kind;

bool kobj_init(void);
/* Zeroed object of `size` bytes whose first field is `kind`. */
void *kobj_new(obj_kind kind, size_t size);
void kobj_delete(void *obj);
/* Unregisters without freeing (the handle becomes invalid; memory stays valid
 * for threads still blocked on the object). */
void kobj_forget(void *obj);
/* The object for `handle` if it is live and of `kind`, else NULL. */
void *kobj_check(uint64_t handle, obj_kind kind);

/* Global lock, also used to serialize lazy creation of static initializers. */
void kobj_lock(void);
void kobj_unlock(void);

/* Thread exit: the next pthread key of `t`, from *cursor on, holding a value
 * and a destructor. Clears the value (as POSIX does before the call) and
 * advances *cursor. False when none is left. */
struct guest_thread;
bool kernel_thread_take_specific(struct guest_thread *t, uint32_t *cursor, uint64_t *destructor, uint64_t *value);

#endif
