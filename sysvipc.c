/*
 * SystemVIPC: SystemV IPC support for Ruby
 * 
 * $Source: /var/cvs/sysvipc/sysvipc/sysvipc.c,v $
 *
 * $Revision: 1.38 $
 * $Date: 2007/09/02 19:43:53 $
 *
 * Copyright (C) 2001, 2006, 2007  Daiki Ueno
 * Copyright (C) 2006, 2007  James Steven Jenkins
 * 
 * SysVIPC is copyrighted free software by Daiki Ueno, Steven Jenkins,
 * and others.  You can redistribute it and/or modify it under either
 * the terms of the GNU General Public License Version 2 (see file 'GPL'),
 * or the conditions below:
 * 
 *   1. You may make and give away verbatim copies of the source form of the
 *      software without restriction, provided that you duplicate all of the
 *      original copyright notices and associated disclaimers.
 * 
 *   2. You may modify your copy of the software in any way, provided that
 *      you do at least ONE of the following:
 * 
 *        a) place your modifications in the Public Domain or otherwise
 *           make them Freely Available, such as by posting said
 * 	  modifications to Usenet or an equivalent medium, or by allowing
 * 	  the author to include your modifications in the software.
 * 
 *        b) use the modified software only within your corporation or
 *           organization.
 * 
 *        c) rename any non-standard executables so the names do not conflict
 * 	  with standard executables, which must also be provided.
 * 
 *        d) make other distribution arrangements with the author.
 * 
 *   3. You may distribute the software in object code or executable
 *      form, provided that you do at least ONE of the following:
 * 
 *        a) distribute the executables and library files of the software,
 * 	  together with instructions (in the manual page or equivalent)
 * 	  on where to get the original distribution.
 * 
 *        b) accompany the distribution with the machine-readable source of
 * 	  the software.
 * 
 *        c) give non-standard executables non-standard names, with
 *           instructions on where to get the original software distribution.
 * 
 *        d) make other distribution arrangements with the author.
 * 
 *   4. You may modify and include the part of the software into any other
 *      software (possibly commercial).  
 * 
 *   5. The scripts and library files supplied as input to or produced as 
 *      output from the software do not automatically fall under the
 *      copyright of the software, but belong to whomever generated them, 
 *      and may be sold commercially, and may be aggregated with this
 *      software.
 * 
 *   6. THIS SOFTWARE IS PROVIDED "AS IS" AND WITHOUT ANY EXPRESS OR
 *      IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
 *      WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 *      PURPOSE.
 */

#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <errno.h>
#include "ruby.h"
#include "rubysig.h"

#ifndef EWOULDBLOCK
#define EWOULDBLOCK EAGAIN
#endif

struct ipcid_ds {
  int id;
  int flags;
  union {
    struct msqid_ds msgstat;
    struct semid_ds semstat;
    struct shmid_ds shmstat;
  } u;

#define msgstat u.msgstat
#define semstat u.semstat
#define shmstat u.shmstat

  void (*stat) (struct ipcid_ds *);
  void (*rmid) (struct ipcid_ds *);
  struct ipc_perm * (*perm) (struct ipcid_ds *);

  void *data;
};

#if !defined(HAVE_TYPE_STRUCT_MSGBUF)
struct msgbuf {
  long mtype;
  char mtext[1];
};
#endif

#if !defined(HAVE_TYPE_UNION_SEMUN)
union semun {
  int val;                    /* value for SETVAL */
  struct semid_ds *buf;       /* buffer for IPC_STAT, IPC_SET */
  unsigned short int *array;  /* array for GETALL, SETALL */
  struct seminfo *__buf;      /* buffer for IPC_INFO */
};
#endif

static VALUE cError;

/*
 * call-seq:
 *   SystemVIPC.ftok(pathname, proj_id) -> Fixnum
 *
 * Convert a pathname and a project identifier to a System V IPC
 * key. +pathname+ is a string filename and +proj_id+ is an integer.
 * See ftok(3).
 */

static VALUE
rb_ftok (klass, v_path, v_id)
     VALUE klass, v_path, v_id;
{
  const char *path = STR2CSTR (v_path);
  key_t key;

  key = ftok (path, NUM2INT (v_id) & 0x7f);
  if (key == -1)
    rb_sys_fail ("ftok(2)");

  return INT2FIX (key);
}

static struct ipcid_ds *
get_ipcid (obj)
     VALUE obj;
{
  struct ipcid_ds *ipcid;
  Data_Get_Struct (obj, struct ipcid_ds, ipcid);

  if (ipcid->id < 0)
    rb_raise (cError, "closed handle");
  return ipcid;
}

static struct ipcid_ds *
get_ipcid_and_stat (obj)
     VALUE obj;
{
  struct ipcid_ds *ipcid;
  ipcid = get_ipcid (obj);
  ipcid->stat (ipcid);
  return ipcid;
}

/* call-seq:
 *   remove -> IPCObject
 *
 * Remove the IPCObject. Return self.
 */

static VALUE
rb_ipc_remove (obj)
     VALUE obj;
{
  struct ipcid_ds *ipcid;

  ipcid = get_ipcid (obj);
  ipcid->rmid (ipcid);

  return obj;
}

static void
msg_stat (msgid)
     struct ipcid_ds *msgid;
{
  if (msgctl (msgid->id, IPC_STAT, &msgid->msgstat) == -1)
    rb_sys_fail ("msgctl(2)");
}

static struct ipc_perm *
msg_perm (msgid)
     struct ipcid_ds *msgid;
{
  return &msgid->msgstat.msg_perm;
}

static void
msg_rmid (msgid)
     struct ipcid_ds *msgid;
{
  if (msgid->id < 0)
    rb_raise (cError, "already removed");
  if (msgctl (msgid->id, IPC_RMID, 0) == -1)
    rb_sys_fail ("msgctl(2)");
  msgid->id = -1;
}

/*
 * call-seq:
 *   MessageQueue.new(key, msgflg = 0) -> MessageQueue
 *
 * Create a new MessageQueue object associated with the message
 * queue identified by +key+. +msgflg+ is a bitwise OR selected from
 * IPC_CREAT and IPC_EXCL. See msgget(2).
 */

static VALUE
rb_msg_s_new (argc, argv, klass)
     int argc;
     VALUE *argv, klass;
{
  struct ipcid_ds msgid_s, *msgid = &msgid_s;
  VALUE dst, v_key, v_msgflg;

  dst = Data_Make_Struct (klass, struct ipcid_ds, NULL, free, msgid);
  rb_scan_args (argc, argv, "11", &v_key, &v_msgflg);
  if (!NIL_P (v_msgflg))
    msgid->flags = NUM2INT (v_msgflg);
  msgid->id = msgget ((key_t)NUM2INT (v_key), msgid->flags);
  if (msgid->id == -1)
    rb_sys_fail ("msgget(2)");
  msgid->stat = msg_stat;
  msgid->perm = msg_perm;
  msgid->rmid = msg_rmid;

  return dst;
}

/*
 * call-seq:
 *   send(mtype, mtext, msgflg = 0) ->  MessageQueue
 *
 * Send message +mtext+ of type +mtype+ with flags +msgflg+. Return
 * self.  See msgop(2).
 */

static VALUE
rb_msg_send (argc, argv, obj)
     int argc;
     VALUE *argv, obj;
{
  VALUE v_type, v_buf, v_flags;
  int flags = 0, error, nowait;
  struct msgbuf *msgp;
  struct ipcid_ds *msgid;
  char *buf;
  size_t len;

  rb_scan_args (argc, argv, "21", &v_type, &v_buf, &v_flags);
  if (!NIL_P (v_flags))
    flags = NUM2INT (v_flags);
  
  len = RSTRING_LEN(v_buf);
  buf = RSTRING_PTR(v_buf);

  msgp = (struct msgbuf *) ALLOCA_N (char, sizeof (long) + len);
  msgp->mtype = NUM2LONG (v_type);
  memcpy (msgp->mtext, buf, len);

  msgid = get_ipcid (obj);

  nowait = flags & IPC_NOWAIT;
  if (!rb_thread_alone()) flags |= IPC_NOWAIT;

 retry:
  TRAP_BEG;
  error = msgsnd (msgid->id, msgp, len, flags);
  TRAP_END;
  if (error == -1)
    {
      switch (errno)
	{
	case EINTR:
	    goto retry;
	case EWOULDBLOCK:
#if EAGAIN != EWOULDBLOCK
	case EAGAIN:
#endif
	    if (!nowait)
	      {
		rb_thread_polling ();
		goto retry;
	      }
	}
      rb_sys_fail ("msgsnd(2)");
    }

  return obj;
}

/*
 * call-seq:
 *   recv(mtype, msgsz, msgflg = 0) ->  MessageQueue
 *
 * Receive up to +msgsz+ bytes of the next message of type +mtype+
 * with flags +msgflg+. Return self. See msgop(2).
 */

static VALUE
rb_msg_recv (argc, argv, obj)
     int argc;
     VALUE *argv, obj;
{
  VALUE v_type, v_len, v_flags;
  int flags = 0, nowait;
  struct msgbuf *msgp;
  struct ipcid_ds *msgid;
  long type;
  size_t rlen, len;
  VALUE ret;

  rb_scan_args (argc, argv, "21", &v_type, &v_len, &v_flags);
  type = NUM2LONG (v_type);
  len = NUM2INT (v_len);
  if (!NIL_P (v_flags))
    flags = NUM2INT (v_flags);

  msgp = (struct msgbuf *) ALLOCA_N (char, sizeof (long) + len);
  msgid = get_ipcid (obj);

  nowait = flags & IPC_NOWAIT;
  if (!rb_thread_alone()) flags |= IPC_NOWAIT;

 retry:
  TRAP_BEG;
  rlen = msgrcv (msgid->id, msgp, len, type, flags);
  TRAP_END;
  if (rlen == (size_t)-1)
    {
      switch (errno)
	{
	case EINTR:
	    goto retry;
	case ENOMSG:
	case EWOULDBLOCK:
#if EAGAIN != EWOULDBLOCK
	case EAGAIN:
#endif
	    if (!nowait)
	      {
		rb_thread_polling ();
		goto retry;
	      }
	}
      rb_sys_fail ("msgrcv(2)");
    }

  ret = rb_str_new (msgp->mtext, rlen);
  return ret;
}

static void
sem_stat (semid)
     struct ipcid_ds *semid;
{
  union semun arg;

  arg.buf = &semid->semstat;
  if (semctl (semid->id, 0, IPC_STAT, arg) == -1)
    rb_sys_fail ("semctl(2)");
}

static struct ipc_perm *
sem_perm (semid)
     struct ipcid_ds *semid;
{
  return &semid->semstat.sem_perm;
}

static void
sem_rmid (semid)
     struct ipcid_ds *semid;
{
  if (semid->id < 0)
    rb_raise (cError, "already removed");
  if (semctl (semid->id, 0, IPC_RMID, 0) == -1)
    rb_sys_fail ("semctl(2)");
  semid->id = -1;
}

/*
 * call-seq:
 *   Semaphore.new(key, nsems, semflg = 0) -> Semaphore
 *
 * Create a new Semaphore object encapsulating the semaphore
 * set identified by +key+. +nsems+ is the number of semaphores
 * in the set, and +semflg+ is a bitwise OR selected from
 * IPC_CREAT and IPC_EXCL. See semget(2).
 */

static VALUE
rb_sem_s_new (argc, argv, klass)
     int argc;
     VALUE *argv, klass;
{
  struct ipcid_ds *semid;
  VALUE dst, v_key, v_nsems, v_semflg;
  int nsems = 0;

  dst = Data_Make_Struct (klass, struct ipcid_ds, NULL, free, semid);
  rb_scan_args (argc, argv, "12", &v_key, &v_nsems, &v_semflg);
  if (!NIL_P (v_nsems))
    nsems = NUM2INT (v_nsems);
  if (!NIL_P (v_semflg))
    semid->flags = NUM2INT (v_semflg);
  semid->id = semget ((key_t)NUM2INT (v_key), nsems, semid->flags);
  if (semid->id == -1)
    rb_sys_fail ("semget(2)");
  semid->stat = sem_stat;
  semid->perm = sem_perm;
  semid->rmid = sem_rmid;

  return dst;
}

#define Check_Valid_Semnum(n, semid)		\
  if (n > semid->semstat.sem_nsems)		\
    rb_raise (cError, "invalid semnum")

/*
 * call-seq:
 *   to_a -> Array
 *
 * Return values for the semaphore set as an array. See semctl(2).
 */

static VALUE
rb_sem_to_a (obj)
     VALUE obj;
{
  struct ipcid_ds *semid;
  int i, nsems;
  VALUE dst;
  union semun arg;

  semid = get_ipcid_and_stat (obj);
  nsems = semid->semstat.sem_nsems;
  arg.array = (unsigned short int *) ALLOCA_N (unsigned short int, nsems);

  semctl (semid->id, 0, GETALL, arg);

  dst = rb_ary_new ();
  for (i = 0; i < nsems; i++)
    rb_ary_push (dst, INT2FIX (arg.array[i]));

  return dst;
}

/*
 * call-seq:
 *   set_all(array) -> Semaphore
 *
 * Set all values of a semaphore to corresponding values from
 * +array+. Return self. See semctl(2).
 */

static VALUE
rb_sem_set_all (obj, ary)
     VALUE obj, ary;
{
  struct ipcid_ds *semid;
  union semun arg;
  int i, nsems;

  semid = get_ipcid_and_stat (obj);
  nsems = semid->semstat.sem_nsems;

  if (RARRAY(ary)->len != nsems)
    rb_raise (cError, "doesn't match with semnum");

  arg.array = (unsigned short int *) ALLOCA_N (unsigned short int, nsems);
  for (i = 0; i < nsems; i++)
    arg.array[i] = NUM2INT (RARRAY(ary)->ptr[i]);
  semctl (semid->id, 0, SETALL, arg);

  return obj;
}

/*
 * call-seq: value(semnum) -> Fixnum
 *
 * Return the value of semaphore +semnum+. See semctl(2).
 */

static VALUE
rb_sem_value (obj, v_pos)
     VALUE obj, v_pos;
{
  struct ipcid_ds *semid;
  int pos;
  int value;

  semid = get_ipcid_and_stat (obj);
  pos = NUM2INT (v_pos);
  Check_Valid_Semnum (pos, semid);
  value = semctl (semid->id, pos, GETVAL, 0);
  if (value == -1)
    rb_sys_fail ("semctl(2)");
  return INT2FIX (value);
}

/*
 * call-seq: set_value(semnum, value) -> Semaphore
 *
 * Set the value of semaphore +semnum+ to +value+. Return self. See
 * semctl(2).
 */

static VALUE
rb_sem_set_value (obj, v_pos, v_value)
     VALUE obj, v_pos, v_value;
{
  struct ipcid_ds *semid;
  int pos;
  union semun arg;

  semid = get_ipcid_and_stat (obj);
  pos = NUM2INT (v_pos);
  Check_Valid_Semnum (pos, semid);
  arg.val = NUM2INT(v_value);
  if (semctl (semid->id, pos, SETVAL, arg) == -1)
    rb_sys_fail ("semctl(2)");
  return obj;
}

/*
 * call-seq:
 *   n_count(semnum) -> Fixnum
 *
 * Return the number of processes waiting for the value semaphore
 * +semnum+ to increase. See semctl(2).
 *
 * *Note*: Ruby threads waiting for a semaphore do not increment
 * this counter. In a multi-threaded program, the SystemVIPC
 * module emulates waiting by repeatedly calling the underlying
 * semop(2) with the IPC_NOWAIT flag set and sleeping between calls.
 */

static VALUE
rb_sem_ncnt (obj, v_pos)
     VALUE obj, v_pos;
{
  struct ipcid_ds *semid;
  int ncnt, pos;

  semid = get_ipcid_and_stat (obj);
  pos = NUM2INT (v_pos);
  Check_Valid_Semnum (pos, semid);
  ncnt = semctl (semid->id, pos, GETNCNT, 0);
  if (ncnt == -1)
    rb_sys_fail ("semctl(2)");
  return INT2FIX (ncnt);
}

/*
 * call-seq:
 *   z_count(semnum) -> Fixnum
 *
 * Return the number of processes waiting for the value semaphore
 * +semnum+ to become zero. See semctl(2).
 *
 * *Note*: Ruby threads waiting for a semaphore do not increment
 * this counter. In a multi-threaded program, the SystemVIPC
 * module emulates waiting by repeatedly calling the underlying
 * semop(2) with the IPC_NOWAIT flag set and sleeping between calls.
 */

static VALUE
rb_sem_zcnt (obj, v_pos)
     VALUE obj, v_pos;
{
  struct ipcid_ds *semid;
  int zcnt, pos;

  semid = get_ipcid_and_stat (obj);
  pos = NUM2INT (v_pos);
  Check_Valid_Semnum (pos, semid);
  zcnt = semctl (semid->id, pos, GETZCNT, 0);
  if (zcnt == -1)
    rb_sys_fail ("semctl(2)");
  return INT2FIX (zcnt);
}

/*
 * call-seq:
 *   pid(semnum) -> Fixnum
 *
 * Return the PID of the process that executed the last semop()
 * call for semaphore +semnum+. See semctl(2).
 */

static VALUE
rb_sem_pid (obj, v_pos)
     VALUE obj, v_pos;
{
  struct ipcid_ds *semid;
  int pid, pos;

  semid = get_ipcid_and_stat (obj);
  pos = NUM2INT (v_pos);
  Check_Valid_Semnum (pos, semid);
  pid = semctl (semid->id, pos, GETPID, 0);
  if (pid == -1)
    rb_sys_fail ("semctl(2)");
  return INT2FIX (pid);
}

/*
 * call-seq:
 *   size -> Fixnum
 *
 * Return the number of semaphores in the set.  See semctl(2).
 */

static VALUE
rb_sem_size (obj)
     VALUE obj;
{
  struct ipcid_ds *semid;
  semid = get_ipcid_and_stat (obj);
  return INT2FIX (semid->semstat.sem_nsems);
}

/*
 * call-seq:
 *   apply(array) -> Semaphore
 *
 * Apply an +array+ of SemaphoreOperation elements.  See semop(2).
 */

static VALUE
rb_sem_apply (obj, ary)
     VALUE obj, ary;
{
  struct ipcid_ds *semid;
  struct sembuf *array;
  int nsops, i, nsems, error, nowait = 0;

  semid = get_ipcid_and_stat (obj);
  nsems = semid->semstat.sem_nsems;
  nsops = RARRAY(ary)->len;
  array = (struct sembuf *) ALLOCA_N (struct sembuf, nsems);
  for (i = 0; i < nsops; i++)
    {
      struct sembuf *op;
      Data_Get_Struct (RARRAY(ary)->ptr[i], struct sembuf, op);
      nowait = nowait || (op->sem_flg & IPC_NOWAIT);
      if (!rb_thread_alone()) op->sem_flg |= IPC_NOWAIT;
      memcpy (&array[i], op, sizeof (struct sembuf));
      Check_Valid_Semnum (array[i].sem_num, semid);
    }
      
 retry:
  TRAP_BEG;
  error = semop (semid->id, array, nsops);
  TRAP_END;
  if (error == -1)
    {
      switch (errno)
	{
	case EINTR:
	    goto retry;
	case EWOULDBLOCK:
#if EAGAIN != EWOULDBLOCK
	case EAGAIN:
#endif
	  if (!nowait)
	    {
	      rb_thread_polling ();
	      goto retry;
	    }
	}
	rb_sys_fail ("semop(2)");
    }

  return obj;
}

static void
shm_stat (shmid)
     struct ipcid_ds *shmid;
{
  if (shmctl (shmid->id, IPC_STAT, &shmid->shmstat) == -1)
    rb_sys_fail ("shmctl(2)");
}

static struct ipc_perm *
shm_perm (shmid)
     struct ipcid_ds *shmid;
{
  return &shmid->shmstat.shm_perm;
}

static void
shm_rmid (shmid)
     struct ipcid_ds *shmid;
{
  if (shmid->id < 0)
    rb_raise (cError, "already removed");
  if (shmctl (shmid->id, IPC_RMID, 0) == -1)
    rb_sys_fail ("shmctl(2)");
  shmid->id = -1;
}

/*
 * call-seq:
 *   SharedMemory.new(key, size = 0, shmflg = 0) -> SharedMemory
 *
 * Return a SharedMemory object encapsulating the
 * shared memory segment associated with +key+. See shmget(2).
 */

static VALUE
rb_shm_s_new (argc, argv, klass)
     int argc;
     VALUE *argv, klass;
{
  struct ipcid_ds *shmid;
  VALUE dst, v_key, v_size, v_shmflg;
  int size = 0;

  dst = Data_Make_Struct (klass, struct ipcid_ds, NULL, free, shmid);
  rb_scan_args (argc, argv, "12", &v_key, &v_size, &v_shmflg);
  if (!NIL_P (v_size))
    size = NUM2INT (v_size);
  if (!NIL_P (v_shmflg))
    shmid->flags = NUM2INT (v_shmflg);
  shmid->id = shmget ((key_t)NUM2INT (v_key), size, shmid->flags);
  if (shmid->id == -1)
    rb_sys_fail ("shmget(2)");
  shmid->stat = shm_stat;
  shmid->perm = shm_perm;
  shmid->rmid = shm_rmid;

  return dst;
}

/*
 * call-seq:
 *   attach(shmflg = 0) -> SharedMemory
 *
 * Attach the shared memory segment. See shmat(2).
 */

static VALUE
rb_shm_attach (argc, argv, obj)
     int argc;
     VALUE *argv, obj;
{
  VALUE v_flags;
  struct ipcid_ds *shmid;
  int flags = 0;
  void *data;

  shmid = get_ipcid (obj);
  if (shmid->data)
    rb_raise (cError, "already attached");

  rb_scan_args (argc, argv, "01", &v_flags);
  if (!NIL_P (v_flags))
    flags = NUM2INT (v_flags);

  data = shmat (shmid->id, 0, flags);
  if (data == (void*)-1)
    rb_sys_fail ("shmat(2)");
  shmid->data = data;

  return obj;
}

/*
 * call-seq:
 *   detach -> SharedMemory
 *
 * Detach the shared memory segment. See shmdt(2).
 */

static VALUE
rb_shm_detach (obj)
     VALUE obj;
{
  struct ipcid_ds *shmid;

  shmid = get_ipcid (obj);
  if (!shmid->data)
    rb_raise (cError, "already detached");

  if (shmdt (shmid->data) == -1)
    rb_sys_fail ("shmdt(2)");
  shmid->data = NULL;

  return obj;
}

#define Check_Valid_Shm_Segsz(n, shmid)		\
  if (n > shmid->shmstat.shm_segsz)		\
    rb_raise (cError, "invalid shm_segsz")

/*
 * call-seq:
 *   read(len = 0, offset = 0) -> String
 *
 * Read +len+ bytes from the shared memory segment starting at
 * optional +offset+.
 */

static VALUE
rb_shm_read (argc, argv, obj)
     int argc;
     VALUE *argv, obj;
{
  struct ipcid_ds *shmid;
  VALUE v_len, v_offset;
  int len, offset = 0;

  shmid = get_ipcid (obj);
  if (!shmid->data)
    rb_raise (cError, "detached memory");
  shmid->stat (shmid);

  len = shmid->shmstat.shm_segsz;

  rb_scan_args (argc, argv, "11", &v_len, &v_offset);
  if (!NIL_P (v_len))
    len = NUM2INT (v_len);
  if (!NIL_P (v_offset))
    offset = NUM2INT (v_offset);
  Check_Valid_Shm_Segsz (len + offset, shmid);

  return rb_str_new (shmid->data + offset, len);
}

/*
 * call-seq:
 *   write(buf, offset = 0) -> SharedMemory
 *
 * Write data from +buf+ to the shared memory segment at
 * optional +offset+ offset.
 */

static VALUE
rb_shm_write (argc, argv, obj)
     int argc;
     VALUE *argv, obj;
{
  struct ipcid_ds *shmid;
  int i, len, offset = 0;
  char *buf;
  VALUE v_buf;

  shmid = get_ipcid (obj);
  if (!shmid->data)
    rb_raise (cError, "detached memory");
  shmid->stat (shmid);

  v_buf = argv[0];

  if (argc == 2)
    offset = NUM2INT (argv[1]);

  len = RSTRING_LEN(v_buf);
  Check_Valid_Shm_Segsz (len + offset, shmid);

  buf = shmid->data + offset;

  for (i = 0; i < len; i++)
    *buf++ = RSTRING_PTR(v_buf)[i];

  return obj;
}

/*
 * call-seq:
 *   size -> Fixnum
 *
 * Return the size of the shared memory segment.
 */

static VALUE
rb_shm_size (obj)
     VALUE obj;
{
  struct ipcid_ds *shmid;
  shmid = get_ipcid_and_stat (obj);
  return INT2FIX (shmid->shmstat.shm_segsz);
}

/*
 * call-seq:
 *   SemaphoreOperation.new(pos, value, flags = 0) -> SemaphoreOperation
 *
 * Create a new SemaphoreOperation. +pos+ is an index identifying
 * a particular semaphore within a semaphore set. +value+ is the
 * value to added or subtracted from the semaphore. +flags+ is a
 * bitwise OR selected from IPC_NOWAIT and SEM_UNDO. See semop(2).
 */

static VALUE
rb_semop_s_new (argc, argv, klass)
     int argc;
     VALUE *argv, klass;
{
  struct sembuf *op;
  VALUE dst, v_pos, v_value, v_flags;

  dst = Data_Make_Struct (klass, struct sembuf, NULL, free, op);
  rb_scan_args (argc, argv, "21", &v_pos, &v_value, &v_flags);
  op->sem_num = NUM2INT (v_pos);
  op->sem_op = NUM2INT (v_value);
  if (!NIL_P (v_flags))
    op->sem_flg = NUM2INT (v_flags);

  return dst;
}

/*
 * call-seq:
 *   pos -> Fixnum
 *
 * Return the operation semaphore position. See semop(2).
 */

static VALUE
rb_semop_pos (obj)
     VALUE obj;
{
  struct sembuf *op;

  Data_Get_Struct (obj, struct sembuf, op);
  return INT2FIX (op->sem_num);
}

/*
 * call-seq:
 *   value -> Fixnum
 *
 * Return the operation value. See semop(2).
 */

static VALUE
rb_semop_value (obj)
     VALUE obj;
{
  struct sembuf *op;

  Data_Get_Struct (obj, struct sembuf, op);
  return INT2FIX (op->sem_op);
}

/*
 * call-seq:
 *   flags -> Fixnum
 *
 * Return the operation flags. See semop(2).
 */

static VALUE
rb_semop_flags (obj)
     VALUE obj;
{
  struct sembuf *op;

  Data_Get_Struct (obj, struct sembuf, op);
  return INT2FIX (op->sem_flg);
}

/* call-seq:
 *   Permission.new(ipcobject) -> Permission
 *
 * Create a Permission object for +ipcobject+.
 */

static VALUE
rb_perm_s_new (klass, v_ipcid)
     VALUE klass, v_ipcid;
{
  struct ipcid_ds *ipcid;

  Data_Get_Struct (v_ipcid, struct ipcid_ds, ipcid);
  ipcid->stat (ipcid);

  return Data_Wrap_Struct (klass, NULL, NULL, ipcid->perm (ipcid));
}

/*
 * call-seq:
 *   cuid -> Fixnum
 *
 * Return effective UID of creator.
 */

static VALUE
rb_perm_cuid (obj)
     VALUE obj;
{
  struct ipc_perm *perm;

  Data_Get_Struct (obj, struct ipc_perm, perm);
  return INT2FIX (perm->cuid);
}

/*
 * call-seq:
 *   cgid -> Fixnum
 *
 * Return effective GID of creator.
 */

static VALUE
rb_perm_cgid (obj)
     VALUE obj;
{
  struct ipc_perm *perm;

  Data_Get_Struct (obj, struct ipc_perm, perm);
  return INT2FIX (perm->cgid);
}

/*
 * call-seq:
 *   uid -> Fixnum
 *
 * Return effective UID of owner.
 */

static VALUE
rb_perm_uid (obj)
     VALUE obj;
{
  struct ipc_perm *perm;

  Data_Get_Struct (obj, struct ipc_perm, perm);
  return INT2FIX (perm->uid);
}

/*
 * call-seq:
 *   gid -> Fixnum
 *
 * Return effective GID of owner.
 */

static VALUE
rb_perm_gid (obj)
     VALUE obj;
{
  struct ipc_perm *perm;

  Data_Get_Struct (obj, struct ipc_perm, perm);
  return INT2FIX (perm->gid);
}

/*
 * call-seq:
 *   mode -> Fixnum
 *
 * Return mode bits.
 */

static VALUE
rb_perm_mode (obj)
     VALUE obj;
{
  struct ipc_perm *perm;

  Data_Get_Struct (obj, struct ipc_perm, perm);
  return INT2FIX (perm->mode);
}

/*
 * Document-class: SystemVIPC
 *
 * = SystemVIPC
 *
 * Ruby module for System V Inter-Process Communication:
 * message queues, semaphores, and shared memory.
 *
 * Hosted as project sysvipc[http://rubyforge.org/projects/sysvipc/]
 * on RubyForge[http://rubyforge.org/].
 *
 * Copyright (C) 2001, 2006, 2007  Daiki Ueno
 * Copyright (C) 2006, 2007  James Steven Jenkins
 *
 * == Usage Synopsis
 * === Common Code
 *
 * All programs using this module must include
 *
 *     require 'sysvipc'
 *
 * It may be convenient to add
 *
 *     include SystemVIPC
 *
 * All IPC objects are identified by a key. SystemVIPC includes a
 * convenience function for mapping file names and integer IDs into a
 * key:
 *
 *     key = ftok('/a/file/that/must/exist', 0)
 *
 * Any IPC object +ipc+ can be removed after use by
 *
 *     ipc.remove
 *
 * === Message Queues
 *
 * Get (create if necessary) a message queue:
 *
 *     mq = MessageQueue.new(key, IPC_CREAT | 0600)
 *
 * Send a message of type 0:
 *
 *     mq.send(0, 'message')
 *
 * Receive up to 100 bytes from the first message of type 0:
 *
 *     msg = mq.recv(0, 100)
 *
 * === Semaphores
 *
 * Get (create if necessary) a set of 5 semaphores:
 *
 *     sm = Semaphore.new(key, 5, IPC_CREAT | 0600)
 *
 * Initialize semaphores if newly created:
 *
 *     sm.set_all(Array.new(5, 1)) if sm.pid(0) == 0
 *
 * Acquire semaphore 2 (waiting if necessary):
 *
 *     sm.apply([SemaphoreOperation.new(2, -1)])
 *
 * Release semaphore 2:
 *
 *     sm.apply([SemaphoreOperation.new(2, 1)])
 *
 * === Shared Memory
 *
 * Get (create if necessary) an 8192-byte shared memory region:
 *
 *     sh = SharedMemory(key, 8192, IPC_CREAT | 0660)
 *
 * Attach shared memory:
 *
 *     sh.attach
 *
 * Write data:
 *
 *     sh.write('testing')
 *
 * Read 100 bytes of data:
 *
 *     data = sh.read(100);
 *
 * Detach shared memory:
 *
 *     sh.detach
 *
 * == Installation
 *
 * 1. <tt>ruby extconf.rb</tt>
 * 2. <tt>make</tt>
 * 3. <tt>make install</tt> (requires appropriate privilege)
 *
 * == Testing
 *
 * 1. <tt>./test_sysvipc</tt>
 */

void Init_sysvipc ()
{
  VALUE mSystemVIPC, cPermission, cIPCObject, cSemaphoreOparation;
  VALUE cMessageQueue, cSemaphore, cSharedMemory;

  mSystemVIPC = rb_define_module ("SystemVIPC");
  rb_define_module_function (mSystemVIPC, "ftok", rb_ftok, 2);

  cPermission =
    rb_define_class_under (mSystemVIPC, "Permission", rb_cObject);
  rb_define_singleton_method (cPermission, "new", rb_perm_s_new, 1);
  rb_define_method (cPermission, "cuid", rb_perm_cuid, 0);
  rb_define_method (cPermission, "cgid", rb_perm_cgid, 0);
  rb_define_method (cPermission, "uid", rb_perm_uid, 0);
  rb_define_method (cPermission, "gid", rb_perm_gid, 0);
  rb_define_method (cPermission, "mode", rb_perm_mode, 0);

  cIPCObject =
    rb_define_class_under (mSystemVIPC, "IPCObject", rb_cObject);
  rb_define_method (cIPCObject, "remove", rb_ipc_remove, 0);
  rb_undef_method (CLASS_OF (cIPCObject), "new");

  cSemaphoreOparation =
    rb_define_class_under (mSystemVIPC, "SemaphoreOperation", rb_cObject);
  rb_define_singleton_method (cSemaphoreOparation, "new", rb_semop_s_new, -1);
  rb_define_method (cSemaphoreOparation, "pos", rb_semop_pos, 0);
  rb_define_method (cSemaphoreOparation, "value", rb_semop_value, 0);
  rb_define_method (cSemaphoreOparation, "flags", rb_semop_flags, 0);

  cError =
    rb_define_class_under (mSystemVIPC, "Error", rb_eStandardError);

  cMessageQueue =
    rb_define_class_under (mSystemVIPC, "MessageQueue", cIPCObject);
  rb_define_singleton_method (cMessageQueue, "new", rb_msg_s_new, -1);
  rb_define_method (cMessageQueue, "send", rb_msg_send, -1);
  rb_define_method (cMessageQueue, "recv", rb_msg_recv, -1);

  cSemaphore =
    rb_define_class_under (mSystemVIPC, "Semaphore", cIPCObject);
  rb_define_singleton_method (cSemaphore, "new", rb_sem_s_new, -1);
  rb_define_method (cSemaphore, "to_a", rb_sem_to_a, 0);
  rb_define_method (cSemaphore, "set_all", rb_sem_set_all, 1);
  rb_define_method (cSemaphore, "value", rb_sem_value, 1);
  rb_define_method (cSemaphore, "set_value", rb_sem_set_value, 2);
  rb_define_method (cSemaphore, "n_count", rb_sem_ncnt, 1);
  rb_define_method (cSemaphore, "z_count", rb_sem_zcnt, 1);
  rb_define_method (cSemaphore, "pid", rb_sem_pid, 1);
  rb_define_method (cSemaphore, "apply", rb_sem_apply, 1);
  rb_define_method (cSemaphore, "size", rb_sem_size, 0);

  cSharedMemory =
    rb_define_class_under (mSystemVIPC, "SharedMemory", cIPCObject);
  rb_define_singleton_method (cSharedMemory, "new", rb_shm_s_new, -1);
  rb_define_method (cSharedMemory, "attach", rb_shm_attach, -1);
  rb_define_method (cSharedMemory, "detach", rb_shm_detach, 0);
  rb_define_method (cSharedMemory, "read", rb_shm_read, -1);
  rb_define_method (cSharedMemory, "write", rb_shm_write, -1);
  rb_define_method (cSharedMemory, "size", rb_shm_size, 0);

  rb_define_const (mSystemVIPC, "IPC_PRIVATE", INT2FIX (IPC_PRIVATE));
  rb_define_const (mSystemVIPC, "IPC_CREAT", INT2FIX (IPC_CREAT));
  rb_define_const (mSystemVIPC, "IPC_EXCL", INT2FIX (IPC_EXCL));
  rb_define_const (mSystemVIPC, "IPC_NOWAIT", INT2FIX (IPC_NOWAIT));
  rb_define_const (mSystemVIPC, "SEM_UNDO", INT2FIX (SEM_UNDO));
}
