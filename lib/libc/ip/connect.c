#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <netinet/in.h>

#include <net/gen/in.h>
#include <net/gen/tcp.h>
#include <net/gen/tcp_io.h>
#include <net/gen/udp.h>
#include <net/gen/udp_io.h>

#include <minix/const.h>

#define DEBUG 0

static int _tcp_connect(int socket, const struct sockaddr *address,
	socklen_t address_len, nwio_tcpconf_t *tcpconfp);
static int _udp_connect(int socket, const struct sockaddr *address,
	socklen_t address_len, nwio_udpopt_t *udpoptp);
static int _uds_connect(int socket, const struct sockaddr *address,
	socklen_t address_len);

int connect(int socket, const struct sockaddr *address,
	socklen_t address_len)
{
	int r;
	nwio_tcpconf_t tcpconf;
	nwio_udpopt_t udpopt;

	r= ioctl(socket, NWIOGTCPCONF, &tcpconf);
	if (r != -1 || (errno != ENOTTY && errno != EBADIOCTL))
	{
		if (r == -1)
		{
			/* Bad file descriptor */
			return -1;
		}
		return _tcp_connect(socket, address, address_len, &tcpconf);
	}

	r= ioctl(socket, NWIOGUDPOPT, &udpopt);
	if (r != -1 || (errno != ENOTTY && errno != EBADIOCTL))
	{
		if (r == -1)
		{
			/* Bad file descriptor */
			return -1;
		}
		return _udp_connect(socket, address, address_len, &udpopt);
	}

	r= _uds_connect(socket, address, address_len);
	if (r != -1 ||
		(errno != ENOTTY && errno != EBADIOCTL &&
		 errno != EAFNOSUPPORT))
	{
		if (r == -1)
		{
			/* Bad file descriptor */
			return -1;
		}

		return r;
	}

#if DEBUG
	fprintf(stderr, "connect: not implemented for fd %d\n", socket);
#endif
	errno= ENOSYS;
	return -1;
}

static int _tcp_connect(int socket, const struct sockaddr *address,
	socklen_t address_len, nwio_tcpconf_t *tcpconfp)
{
	int r;
	struct sockaddr_in *sinp;
	nwio_tcpconf_t tcpconf;
	nwio_tcpcl_t tcpcl;

	if (address_len != sizeof(*sinp))
	{
		errno= EINVAL;
		return -1;
	}
	sinp= (struct sockaddr_in *)address;
	if (sinp->sin_family != AF_INET)
	{
		errno= EINVAL;
		return -1;
	}
	tcpconf.nwtc_flags= NWTC_SET_RA | NWTC_SET_RP;
	if ((tcpconfp->nwtc_flags & NWTC_LOCPORT_MASK) == NWTC_LP_UNSET)
		tcpconf.nwtc_flags |= NWTC_LP_SEL;
	tcpconf.nwtc_remaddr= sinp->sin_addr.s_addr;
	tcpconf.nwtc_remport= sinp->sin_port;

	if (ioctl(socket, NWIOSTCPCONF, &tcpconf) == -1)
        {
		/* Ignore EISCONN error. The NWIOTCPCONN ioctl will get the
		 * right error.
		 */
		if (errno != EISCONN)
			return -1;
	}

	tcpcl.nwtcl_flags= TCF_DEFAULT;

	r= fcntl(socket, F_GETFL);
	if (r == 1)
		return -1;
	if (r & O_NONBLOCK)
		tcpcl.nwtcl_flags |= TCF_ASYNCH;

	r= ioctl(socket, NWIOTCPCONN, &tcpcl);
	return r;
}

static int _udp_connect(int socket, const struct sockaddr *address,
	socklen_t address_len, nwio_udpopt_t *udpoptp)
{
	int r;
	struct sockaddr_in *sinp;
	nwio_udpopt_t udpopt;

	if (address == NULL)
	{
		/* Unset remote address */
		udpopt.nwuo_flags= NWUO_RP_ANY | NWUO_RA_ANY | NWUO_RWDATALL;

		r= ioctl(socket, NWIOSUDPOPT, &udpopt);
		return r;
	}

	if (address_len != sizeof(*sinp))
	{
		errno= EINVAL;
		return -1;
	}
	sinp= (struct sockaddr_in *)address;
	if (sinp->sin_family != AF_INET)
	{
		errno= EINVAL;
		return -1;
	}
	udpopt.nwuo_flags= NWUO_RP_SET | NWUO_RA_SET | NWUO_RWDATONLY;
	if ((udpoptp->nwuo_flags & NWUO_LOCPORT_MASK) == NWUO_LP_ANY)
		udpopt.nwuo_flags |= NWUO_LP_SEL;
	udpopt.nwuo_remaddr= sinp->sin_addr.s_addr;
	udpopt.nwuo_remport= sinp->sin_port;

	r= ioctl(socket, NWIOSUDPOPT, &udpopt);
	return r;
}

static int in_group(uid_t uid, gid_t gid)
{
	int r, i;
	int size;
	gid_t *list;

	size = sysconf(_SC_NGROUPS_MAX);
	list = malloc(size * sizeof(gid_t));

	if (list == NULL) {
		return 0;
	}

	r= getgroups(size, list);
	if (r == -1) {
		free(list);
		return 0;
	}

	for (i = 0; i < r; i++) {
		if (gid == list[i]) {
			free(list);
			return 1;
		}
	}

	free(list);
	return 0;
}

static int _uds_connect(int socket, const struct sockaddr *address,
	socklen_t address_len)
{
	mode_t bits, perm_bits, access_desired;
	struct stat buf;
	uid_t euid;
	gid_t egid;
	char real_sun_path[PATH_MAX+1];
	char *realpath_result;
	int i, r, shift;
	int null_found;

	if (address == NULL) {
		errno = EFAULT;
		return -1;
	}

	/* sun_family is always supposed to be AF_UNIX */
	if (((struct sockaddr_un *) address)->sun_family != AF_UNIX) {
		errno = EAFNOSUPPORT;
		return -1;
	}

	/* an empty path is not supported */
	if (((struct sockaddr_un *) address)->sun_path[0] == '\0') {
		errno = EINVAL;
		return -1;
	}

	/* the path must be a null terminated string for realpath to work */
	for (null_found = i = 0;
		i < sizeof(((struct sockaddr_un *) address)->sun_path); i++) {

		if (((struct sockaddr_un *) address)->sun_path[i] == '\0') {
			null_found = 1;
			break;
		}
	}

	if (!null_found) {
		errno = EINVAL;
		return -1;
	}

	/*
	 * Get the realpath(3) of the socket file.
	 */

	realpath_result = realpath(((struct sockaddr_un *) address)->sun_path,
							real_sun_path);
	if (realpath_result == NULL) {
		return -1;
	}

	if (strlen(real_sun_path) >= UNIX_PATH_MAX) {
		errno = ENAMETOOLONG;
		return -1;
	}

	strcpy(((struct sockaddr_un *) address)->sun_path, real_sun_path);

	/*
	 * input parameters look good -- check the permissions of the 
	 * socket file. emulate eaccess() (i.e. the access(2) function 
	 * with effective UID/GID).
	 */

	access_desired = R_BIT | W_BIT; /* read + write access */

	euid = geteuid();
	egid = getegid();

	if (euid == -1 || egid == -1) {
		errno = EACCES;
		return -1;
	}

	r= stat(((struct sockaddr_un *) address)->sun_path, &buf);
	if (r == -1) {
		return -1;
	}

	if (!S_ISSOCK(buf.st_mode)) {
		errno = EINVAL;
		return -1;
	}

	bits = buf.st_mode;

	if (euid == ((uid_t) 0)) {
		perm_bits = R_BIT | W_BIT;
	} else {
		if (euid == buf.st_uid) {
			shift = 6; /* owner */
		} else if (egid == buf.st_gid) {
			shift = 3; /* group */
		} else if (in_group(euid, buf.st_gid)) {
			shift = 3; /* suppl. groups */
		} else {
			shift = 0; /* other */
		}

		perm_bits = (bits >> shift) & (R_BIT | W_BIT | X_BIT);
	}

	if ((perm_bits | access_desired) != perm_bits) {
		errno = EACCES;
		return -1;
	}

	/* perform the connect */
	r= ioctl(socket, NWIOSUDSCONN, (void *) address);
	return r;
}
