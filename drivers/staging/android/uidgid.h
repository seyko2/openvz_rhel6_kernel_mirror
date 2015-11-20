#ifndef _LINUX_UIDGID_H
#define _LINUX_UIDGID_H

#define INVALID_UID -1

typedef uid_t kuid_t;
extern struct user_namespace init_user_ns;

static inline uid_t from_kuid(struct user_namespace *to, kuid_t kuid)
{
	return kuid;
}

static inline uid_t __kuid_val(kuid_t uid)
{
	return uid;
}

static inline bool uid_eq(kuid_t left, kuid_t right)
{
	return __kuid_val(left) == __kuid_val(right);
}

static inline bool uid_valid(kuid_t uid)
{
	return !uid_eq(uid, INVALID_UID);
}

#endif
