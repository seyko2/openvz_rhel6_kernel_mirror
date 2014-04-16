/*
 * fs/lockd/ve.h
 *
 * VE context for LockD
 */

#ifndef __VE_LOCKD_H__
#define __VE_LOCKD_H__

struct ve_nlm_data {
	unsigned int		_nlmsvc_users;
	struct task_struct*	_nlmsvc_task;
	unsigned long		_nlmsvc_timeout;
	struct svc_rqst*	_nlmsvc_rqst;

	struct hlist_head	_nlm_reserved_pids;
	spinlock_t		_nlm_reserved_lock;

	struct delayed_work	_grace_period_end;
	struct list_head	_grace_list;
	struct lock_manager	_lockd_manager;
};

#ifdef CONFIG_VE

#include <linux/ve.h>

#define NLM_CTX_FIELD(arg)	(get_exec_env()->nlm_data->_##arg)

#else

#define NLM_CTX_FIELD(arg)	_##arg

#endif

#define nlmsvc_grace_period	NLM_CTX_FIELD(nlmsvc_grace_period)
#define nlmsvc_timeout		NLM_CTX_FIELD(nlmsvc_timeout)
#define nlmsvc_users		NLM_CTX_FIELD(nlmsvc_users)
#define nlmsvc_task		NLM_CTX_FIELD(nlmsvc_task)
#define nlmsvc_rqst		NLM_CTX_FIELD(nlmsvc_rqst)

#define nlm_reserved_pids	NLM_CTX_FIELD(nlm_reserved_pids)
#define nlm_reserved_lock	NLM_CTX_FIELD(nlm_reserved_lock)

#define grace_period_end	NLM_CTX_FIELD(grace_period_end)
#define grace_list		NLM_CTX_FIELD(grace_list)
#define lockd_manager		NLM_CTX_FIELD(lockd_manager)

#endif /* __VE_LOCKD_H__ */

