#include "ctforge.h"

static const char *ct_err_str[CT_ERRNO_MAX] = {
	"success", // 0 (not used for errors, but index alignment)
	"argument list too long", // CT_E2BIG (-1)
	"permission denied", // CT_EACCES (-2)
	"address already in use", // CT_EADDRINUSE (-3)
	"cannot assign requested address", // CT_EADDRNOTAVAIL (-4)
	"address family not supported", // CT_EAFNOSUPPORT (-5)
	"resource temporarily unavailable", // CT_EAGAIN (-6)
	"connection already in progress", // CT_EALREADY (-7)
	"bad file descriptor", // CT_EBADF (-8)
	"resource busy or locked", // CT_EBUSY (-9)
	"operation canceled", // CT_ECANCELED (-10)
	"invalid Unicode character", // CT_ECHARSET (-11)
	"software caused connection abort", // CT_ECONNABORTED (-12)
	"connection refused", // CT_ECONNREFUSED (-13)
	"connection reset by peer", // CT_ECONNRESET (-14)
	"destination address required", // CT_EDESTADDRREQ (-15)
	"file already exists", // CT_EEXIST (-16)
	"bad address in system call argument", // CT_EFAULT (-17)
	"file too large", // CT_EFBIG (-18)
	"host is unreachable", // CT_EHOSTUNREACH (-19)
	"interrupted system call", // CT_EINTR (-20)
	"invalid argument", // CT_EINVAL (-21)
	"i/o error", // CT_EIO (-22)
	"socket is already connected", // CT_EISCONN (-23)
	"illegal operation on a directory", // CT_EISDIR (-24)
	"too many symbolic links encountered", // CT_ELOOP (-25)
	"too many open files", // CT_EMFILE (-26)
	"message too long", // CT_EMSGSIZE (-27)
	"name too long", // CT_ENAMETOOLONG (-28)
	"network is down", // CT_ENETDOWN (-29)
	"network is unreachable", // CT_ENETUNREACH (-30)
	"file table overflow", // CT_ENFILE (-31)
	"no buffer space available", // CT_ENOBUFS (-32)
	"no such device", // CT_ENODEV (-33)
	"no such file or directory", // CT_ENOENT (-34)
	"not enough memory", // CT_ENOMEM (-35)
	"machine is not on the network", // CT_ENONET (-36)
	"protocol not available", // CT_ENOPROTOOPT (-37)
	"no space left on device", // CT_ENOSPC (-38)
	"function not implemented", // CT_ENOSYS (-39)
	"socket is not connected", // CT_ENOTCONN (-40)
	"not a directory", // CT_ENOTDIR (-41)
	"directory not empty", // CT_ENOTEMPTY (-42)
	"socket operation on non-socket", // CT_ENOTSOCK (-43)
	"operation not supported", // CT_ENOTSUP (-44)
	"operation not permitted", // CT_EPERM (-45)
	"broken pipe", // CT_EPIPE (-46)
	"protocol error", // CT_EPROTO (-47)
	"protocol not supported", // CT_EPROTONOSUPPORT (-48)
	"protocol wrong type for socket", // CT_EPROTOTYPE (-49)
	"result too large", // CT_ERANGE (-50)
	"read-only file system", // CT_EROFS (-51)
	"cannot send after transport endpoint shutdown", // CT_ESHUTDOWN (-52)
	"invalid seek", // CT_ESPIPE (-53)
	"no such process", // CT_ESRCH (-54)
	"connection timed out", // CT_ETIMEDOUT (-55)
	"text file is busy", // CT_ETXTBSY (-56)
	"cross-device link not permitted", // CT_EXDEV (-57)
	"unknown error" // CT_UNKNOWN (-58)
};

const char *ct_strerror(int err)
{
	int idx = (err < 0) ? -err : err;

	if (idx >= 0 && idx < CT_ERRNO_MAX)
		return ct_err_str[idx];

	return "unknown error";
}

const char *ct_strerror_r(int err, char *buf, size_t buflen)
{
	const char *msg = ct_strerror(err);

	if (buf && buflen > 0)
		snprintf(buf, buflen, "%s", msg);

	return msg;
}
