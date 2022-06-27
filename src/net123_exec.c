/*
	net123_exec: network (HTTP(S)) streaming for mpg123 via fork+exec

	This avoids linking any network code directly into mpg123, just using external
	tools at runtime.

	This calls wget with fallback to curl by default, one of those two
	specifically if param.network_backend is set accordingly.
*/


#include "config.h"
#include "net123.h"
// for strings
#include "mpg123.h"

// Just for parameter struct that we use for HTTP auth and proxy info.
#include "mpg123app.h"

#include "compat.h"

#include "debug.h"
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

// Those are set via environment variables:
// http_proxy
// https_proxy
//    If set, the http_proxy and https_proxy variables should contain the
//    URLs of the proxies for HTTP and HTTPS connections respectively.
// ftp_proxy
//    same
// wget --user=... --password=... 
// Alternatively: Have them in .netrc.

const char *net123_backends[] =
{
	"wget"
,	"curl"
,	NULL
};

struct net123_handle_struct
{
	int fd;
	pid_t worker;
};

// Combine two given strings into one newly allocated one.
// Use: (--parameter=, value) -> --parameter=value
static char *catstr(const char *par, const char *value)
{
	char *res = malloc(strlen(par)+strlen(value)+1);
	if(res)
	{
		res[0] = 0;
		strcat(res, par);
		strcat(res, value);
	}
	return res;
}

// < 0: not checked, 0: not there, 1: present
static int got_curl = -1;
static int got_wget = -1;

static int check_program(char **argv)
{
	pid_t pid = fork();
	if(pid == 0)
	{
		int outfd = open("/dev/null", O_WRONLY);
		dup2(outfd, STDOUT_FILENO);
		int infd  = open("/dev/null", O_RDONLY);
		dup2(infd,  STDIN_FILENO);
		int errfd = open("/dev/null", O_WRONLY);
		dup2(errfd, STDERR_FILENO);
		execvp(argv[0], argv);
		exit(1);
	}
	else if(pid > 0)
	{
		int stat;
		if( (waitpid(pid, &stat, 0) == pid)
			&& WIFEXITED(stat) && WEXITSTATUS(stat)==0 )
			return 1;
	}
	return 0; // false, not there
}

static char **wget_argv(const char *url, const char * const * client_head)
{
	const char* base_args[] =
	{
		"wget" // begins with program name
	,	"--output-document=-"
#ifndef DEBUG
	,	"--quiet"
#endif
	,	"--save-headers"
	};
	size_t cheads = 0;
	while(client_head && client_head[cheads]){ ++cheads; }
	// Get the count of argument strings right!
	// Fixed args + agent + client headers [+ auth] + URL + NULL
	int argc = sizeof(base_args)/sizeof(char*)+1+cheads+1+1;
	char *httpauth = NULL;
	char *user = NULL;
	char *password = NULL;
	if(param.httpauth && (httpauth = compat_strdup(param.httpauth)))
	{
		char *sep = strchr(httpauth, ':');
		if(sep)
		{
			argc += 2;
			*sep = 0;
			user = httpauth;
			password = sep+1;
		}
	}
	char ** argv = malloc(sizeof(char*)*(argc+1));
	if(!argv)
	{
		error("failed to allocate argv");
		return NULL;
	}
	int an = 0;
	for(;an<sizeof(base_args)/sizeof(char*); ++an)
		argv[an] = compat_strdup(base_args[an]);
	argv[an++] = compat_strdup("--user-agent=" PACKAGE_NAME "/" PACKAGE_VERSION);
	for(size_t ch=0; ch < cheads; ++ch)
		argv[an++] = catstr("--header=", client_head[ch]);
	if(user)
		argv[an++] = catstr("--user=", user);
	if(password)
		argv[an++] = catstr("--password=", password);
	argv[an++] = compat_strdup(url);
	argv[an++] = NULL;
	return argv;
}

static char **curl_argv(const char *url, const char * const * client_head)
{
	const char* base_args[] =
	{
		"curl" // begins with program name
#ifdef DEBUG
	,	"--verbose"
#else
	,	"--silent"
	,	"--show-error"
#endif
	,	"--dump-header"
	,	"-"
	};
	size_t cheads = 0;
	while(client_head && client_head[cheads]){ ++cheads; }
	// Get the count of argument strings right!
	// Fixed args + agent + client headers [+ auth] + URL + NULL
	int argc = sizeof(base_args)/sizeof(char*)+2+2*cheads+1+1;
	char *httpauth = NULL;
	if(param.httpauth && (httpauth = compat_strdup(param.httpauth)))
		argc += 2;
	char ** argv = malloc(sizeof(char*)*(argc+1));
	if(!argv)
	{
		error("failed to allocate argv");
		return NULL;
	}
	int an = 0;
	for(;an<sizeof(base_args)/sizeof(char*); ++an)
		argv[an] = compat_strdup(base_args[an]);
	argv[an++] = compat_strdup("--user-agent");
	argv[an++] = compat_strdup(PACKAGE_NAME "/" PACKAGE_VERSION);
	for(size_t ch=0; ch < cheads; ++ch)
	{
		argv[an++] = compat_strdup("--header");
		argv[an++] = compat_strdup(client_head[ch]);
	}
	if(httpauth)
	{
		argv[an++] = compat_strdup("--user");
		argv[an++] = httpauth;
	}
	argv[an++] = compat_strdup(url);
	argv[an++] = NULL;
	return argv;
}

net123_handle *net123_open(const char *url, const char * const * client_head)
{
	int use_curl = 0;
	// Semi-threadsafe: The check might take place multiple times, but writing the integer
	// should be safe enough.
	if(!strcmp("auto",param.network_backend))
	{
		char *curl_argv[] = { "curl", "--version", NULL };
		char *wget_argv[] = { "wget", "--version", NULL };
		if(got_curl < 0)
			got_curl = check_program(curl_argv);
		if(got_wget < 0)
			got_wget = check_program(wget_argv);
		if(got_wget < 1 && got_curl == 1)
			use_curl = 1;
	} else if(!strcmp("curl", param.network_backend))
	{
		use_curl = 1;
	} else if(!strcmp("wget", param.network_backend))
	{
		use_curl = 0;
	} else
	{
		merror("invalid network backend specified: %s", param.network_backend);
		return NULL;
	}

	int fd[2];
	int hi = -1; // index of header value that might get a continuation line
	net123_handle *nh = malloc(sizeof(net123_handle));
	if(!nh)
		return NULL;
	nh->fd = -1;
	nh->worker = 0;
	errno = 0;
	if(pipe(fd))
	{	
		merror("failed creating a pipe: %s", strerror(errno));
		free(nh);
		return NULL;
	}

	compat_binmode(fd[0], TRUE);
	compat_binmode(fd[1], TRUE);

	nh->worker = fork();
	if(nh->worker == -1)
	{
		merror("fork failed: %s", strerror(errno));
		free(nh);
		return NULL;
	}

	if(nh->worker == 0)
	{
		close(fd[0]);
		dup2(fd[1], STDOUT_FILENO);
		int infd  = open("/dev/null", O_RDONLY);
		dup2(infd,  STDIN_FILENO);
		// child
		// Proxy environment variables can just be set in the user and inherited here, right?
		int argc;
		
		char **argv = use_curl ? curl_argv(url, client_head) : wget_argv(url, client_head);
		if(!argv)
			exit(1);
		errno = 0;
		if(param.verbose > 2)
		{
			char **a = argv;
			fprintf(stderr, "HTTP helper command:\n");
			while(*a)
			{
				fprintf(stderr, " %s\n", *a);
				++a;
			}
		} else
		{
			int errfd = open("/dev/null", O_WRONLY);
			dup2(errfd, STDERR_FILENO);
		}
		execvp(argv[0], argv);
		merror("cannot execute %s: %s", argv[0], strerror(errno));
		exit(1);
	}
	// parent
	if(param.verbose > 1)
		fprintf(stderr, "Note: started network helper with PID %"PRIiMAX"\n", (intmax_t)nh->worker);
	errno = 0;
	close(fd[1]);
	nh->fd = fd[0];
	return nh;
}

size_t net123_read(net123_handle *nh, void *buf, size_t bufsize)
{
	if(!nh || (bufsize && !buf))
		return 0;
	return unintr_read(nh->fd, buf, bufsize);
}

void net123_close(net123_handle *nh)
{
	if(!nh)
		return;
	if(nh->worker)
	{
		kill(nh->worker, SIGKILL);
		errno = 0;
		if(waitpid(nh->worker, NULL, 0) < 0)
			merror("failed to wait for worker process: %s", strerror(errno));
		else if(param.verbose > 1)
			fprintf(stderr, "Note: network helper %"PRIiMAX" finished\n", (intmax_t)nh->worker);
	}
	if(nh->fd > -1)
		close(nh->fd);
	free(nh);
}

