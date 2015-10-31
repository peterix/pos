#define _XOPEN_SOURCE
#define _XOPEN_SOURCE_EXTENDED 1 /* XPG 4.2 - needed for WCOREDUMP() */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/wait.h>

#define SON 0
#define PARENT 1
#define GRANDPARENT 2
const char* id_names[] =
{
	"son",
	"parent",
	"grandparent"
};

/* print who you are */
void print_ident(int identity)
{
	printf("%s identification: \n", id_names[identity]);
	printf("pid = %d,ppid = %d,pgrp = %d\n", getpid(), getppid(), getpgrp());
	printf("uid = %d,gid = %d\n", getuid(), getgid());
	printf("euid = %d,egid = %d\n", geteuid(), getegid());
}

/* wait for a process and report how it ended */
int wait_for_pid(pid_t id, int identity)
{
	char * coredumped = "";
	int status;
	if (waitpid(id, &status, 0) == -1)
	{
		perror("waitpid");
		return EXIT_FAILURE;
	}
	if (WIFSIGNALED(status))
	{
#ifdef WCOREDUMP
		if(WCOREDUMP(status))
			coredumped = "with core dump ";
#endif
		printf("%s exit (pid = %d):", id_names[identity], id);
		printf("signal termination %s(signal = %d)\n", coredumped, WTERMSIG(status));
	}
	else if (WIFEXITED(status))
	{
		printf("%s exit (pid = %d):", id_names[identity], id);
		printf("child exit status %d\n", WEXITSTATUS(status));
	}
	else
	{
		printf("%s exit (pid = %d):", id_names[identity], id);
		printf("unknown type of termination\n");
	}
	return EXIT_SUCCESS;
}

/* this is the son process */
int son(int argc, char **argv)
{
	int success;
	print_ident(SON);
	success = execvp(argv[1], &argv[1]);
	if(success == -1)
	{
		perror("Execv error");
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}

/* this is the parent process (starts son) */
int parent(int argc, char **argv)
{
	pid_t pid;
	/* print ident before spawning the next process */
	print_ident(PARENT);
	
	pid = fork();
	if(pid < 0)
	{
		fprintf(stderr, "parent: call to fork() failed\n");
		return EXIT_FAILURE;
	}
	if(pid)
	{
		return wait_for_pid(pid,SON);
	}
	else
	{
		return son(argc, argv);
	}
}

/* this is the grandparent process (starts parent) */
int main(int argc, char **argv)
{
	pid_t ppid;
	if(argc < 2)
	{
		fprintf(stderr, "program started with an incorrect number of parameters\n");
		return EXIT_FAILURE;
	}
	/* print ident before spawning the next process */
	print_ident(GRANDPARENT);
	ppid = fork();
	if(ppid < 0)
	{
		fprintf(stderr, "grandparent: call to fork() failed\n");
		return EXIT_FAILURE;
	}
	if(ppid)
	{
		return wait_for_pid(ppid, PARENT);
	}
	else
	{
		return parent(argc, argv);
	}

	return EXIT_SUCCESS;
}
