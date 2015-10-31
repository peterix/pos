#define _XOPEN_SOURCE
#define _XOPEN_SOURCE_EXTENDED 1 /* XPG 4.2 - needed for WCOREDUMP() */
#define _POSIX_C_SOURCE 199506L
#define _REENTRANT

#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <termios.h>
#include <signal.h>
#include <sys/types.h>
#include <errno.h>

#include <vector>
#include <set>
#include <string>

#define BUF_LIMIT 512
char buffer[BUF_LIMIT + 1] = "";
bool buffer_ready = false;
bool buffer_empty = true;

// there is a ``possible data race'' on this variable.
// its purpose is to prevent the shell from writing into the output while a foreground task
// is active - this is only background task completion notifications
bool foreground_active = false;

pthread_mutex_t mutex;
pthread_cond_t cond;
pthread_cond_t cond2;
pthread_condattr_t cattr;
pthread_mutexattr_t mattr;

pthread_mutex_t bg_mutex;
std::set <int> background_processes;
void init_background()
{
	pthread_mutex_init(&bg_mutex, NULL);
}
void finish_background()
{
	pthread_mutex_destroy(&bg_mutex);
}

void add_background(pid_t pid)
{
	pthread_mutex_lock(&bg_mutex);
	background_processes.insert(pid);
	pthread_mutex_unlock(&bg_mutex);
}
void remove_background(pid_t pid)
{
	pthread_mutex_lock(&bg_mutex);
	background_processes.erase(pid);
	pthread_mutex_unlock(&bg_mutex);
}
bool is_background(pid_t pid)
{
	bool result;
	pthread_mutex_lock(&bg_mutex);
	result = background_processes.count(pid);
	pthread_mutex_unlock(&bg_mutex);
	return result;
}

pthread_mutex_t end_mutex;
bool end = false;
void init_is_end()
{
	pthread_mutex_init(&end_mutex, NULL);
}
void finish_is_end()
{
	pthread_mutex_destroy(&end_mutex);
}
void set_end()
{
	pthread_mutex_lock(&end_mutex);
	end = 1;
	pthread_mutex_unlock(&end_mutex);
	kill(getpid(),SIGUSR1);
}
bool is_end()
{
	bool _end;
	pthread_mutex_lock(&end_mutex);
	_end = end;
	pthread_mutex_unlock(&end_mutex);
	return _end;
}

char *skip_whitespace(char * buffer)
{
	while(1)
	{
		char c = *buffer;
		if(c == ' ' || c == '\t')
		{
			buffer++;
			continue;
		}
		return buffer;
	}
}

char * read_str(char * buffer, std::string & out)
{
	out.clear();
	char *buf = skip_whitespace(buffer);
	while(1)
	{
		char c = *buf;
		if(c == ' ' || c == '\t' || c == '>' || c == '<' || c == '&' || c == 0 || c == '\n')
			return buf;
		out.push_back(c);
		buf++;
	}
	return buf;
}

enum parsestate
{
	read_prog,
	read_param,
	read_input,
	read_output,
	decision_point
};

int wait_for_pid(pid_t id, bool synchronous)
{
	int status;
	if (waitpid(id, &status, 0) == -1)
	{
		perror("waitpid");
		return EXIT_FAILURE;
	}
	if(!synchronous && !foreground_active)
	{
		if (WIFSIGNALED(status))
		{
			fprintf(stderr,"child exit (pid = %d, signal = %d)\n", id, WTERMSIG(status));
		}
		else if (WIFEXITED(status))
		{
			fprintf(stderr,"child exit (pid = %d, status = %d)\n", id, WEXITSTATUS(status));
		}
		else
		{
			fprintf(stderr,"child exit (pid = %d)\n", id);
		}
	}
	return EXIT_SUCCESS;
}

int run(std::vector<std::string> &params, std::string &input_filename, std::string &output_filename, bool background)
{
	// construct the array of C string pointers execvp() expects.
	std::vector<const char *> paramptrs;
	for(std::size_t i = 0; i < params.size(); i++)
	{
		paramptrs.push_back(params[i].c_str());
	}
	paramptrs.push_back(NULL);
	
	pid_t pid = fork();
	if(pid < 0)
	{
		fprintf(stderr, "failed to fork the process\n");
		return EXIT_FAILURE;
	}
	if(pid)
	{
		if(background)
		{
			add_background(pid);
			return EXIT_SUCCESS;
		}
		else
		{
			foreground_active = true;
			int result = wait_for_pid(pid, true);
			foreground_active = false;
			return result;
		}
	}
	else
	{
		int input_filedes = -1;
		if(!input_filename.empty())
		{
			input_filedes = open(input_filename.c_str(),O_RDONLY);
			if(input_filedes < 0)
			{
				fprintf(stderr,"%s doesn't exist or is otherwise inaccessible.\n", input_filename.c_str());
				exit(EXIT_FAILURE);
			}
			dup2(input_filedes, STDIN_FILENO);
			close(input_filedes);
		}
		
		int output_filedes = -1;
		if(!output_filename.empty())
		{
			mode_t mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;
			output_filedes = open(output_filename.c_str(),O_RDWR|O_CREAT|O_TRUNC, mode);
			if(output_filedes < 0)
			{
				fprintf(stderr,"%s doesn't exist or is otherwise inaccessible.\n", output_filename.c_str());
				exit(EXIT_FAILURE);
			}
			dup2(output_filedes, STDOUT_FILENO);
			close(output_filedes);
		}
		
		// re-enable SIGCHLD and SIGUSR1
		sigset_t mask;
		sigemptyset( & mask );
		sigaddset( & mask, SIGCHLD );
		sigaddset( & mask, SIGUSR1 );
		pthread_sigmask( SIG_UNBLOCK, & mask, NULL );
		
		// foreground
		if(!background)
		{
			// enable SIGINT
			sigset_t mask;
			sigemptyset( & mask );
			sigaddset( & mask, SIGINT );
			pthread_sigmask( SIG_UNBLOCK, & mask, NULL );
		}
		// background
		else if(input_filedes == -1)
		{
			// disconnect
			if (setpgid(0, 0) < 0)
			{
				/* create new process group           */
				perror("setpgid");
				exit(EXIT_FAILURE);
			}
		}
		
		/*
		 * possibly could parse PATH and deal with it here.
		 * The system can do the PATH resolution just fine though.
		 */
		int success = execvp(paramptrs[0], (char * const *) &(paramptrs[0]));
		if(success == -1)
		{
			fprintf(stderr,"%s doesn't exist or is otherwise inaccessible.\n", paramptrs[0]);
			exit(EXIT_FAILURE);
		}
	}
	return EXIT_SUCCESS;
}

int parse_and_run(char * buffer)
{
	bool background = false;
	std::string input_filename, output_filename;
	std::vector<std::string> params;
	parsestate ps = read_prog;
	char * buf = buffer;
	bool finished = false;
	while(!finished)
	{
		if(ps == read_prog)
		{
			std::string current;
			buf = read_str(buf, current);
			if(current.empty())
			{
				fprintf(stderr, "nothing to run...\n");
				return false;
			}
			params.push_back(current);
			ps = read_param;
		}
		else if(ps == read_param)
		{
			std::string current;
			buf = read_str(buf, current);
			if(current.empty())
			{
				ps = decision_point;
			}
			else
			{
				params.push_back(current);
			}
			// exit: nexp param or decision point
		}
		else if(ps == decision_point)
		{
			buf = skip_whitespace(buf);
			char c = *buf;
			if(c == '<')
			{
				ps = read_input;
				buf++;
			}
			else if(c == '>')
			{
				ps = read_output;
				buf++;
			}
			else if(c == '&')
			{
				background = true;
				buf++;
				break;
			}
			else if(c == 0 || c == '\n')
			{
				break;
			}
		}
		else if(ps == read_input)
		{
			std::string current;
			buf = read_str(buf, current);
			if(current.empty())
			{
				fprintf(stderr, "can't redirect input from nowhere\n");
				return false;
			}
			input_filename = current;
			ps = decision_point;
		}
		else if(ps == read_output)
		{
			std::string current;
			buf = read_str(buf, current);
			if(current.empty())
			{
				fprintf(stderr, "can't redirect output to nowhere\n");
				return false;
			}
			output_filename = current;
			ps = decision_point;
		}
		else
		{
			fprintf(stderr, "Just what is going on here???\n");
			return false;
		}
	}
	return run(params,input_filename,output_filename,background);
}


int monitor_new()
{
	int all_ok = 0;
	all_ok |= pthread_mutexattr_init(&mattr);
	all_ok |= pthread_condattr_init(&cattr);
	all_ok |= pthread_mutex_init(&mutex, &mattr);
	all_ok |= pthread_cond_init(&cond, &cattr);
	all_ok |= pthread_cond_init(&cond2, &cattr);
	return all_ok == 0;
}

int monitor_delete()
{
	int all_ok = 0;
	all_ok |= pthread_mutex_destroy(&mutex);
	all_ok |= pthread_cond_destroy(&cond);
	all_ok |= pthread_cond_destroy(&cond2);
	all_ok |= pthread_mutexattr_destroy(&mattr);
	all_ok |= pthread_condattr_destroy(&cattr);
	return all_ok == 0;
}

void *reader_func(void *threadid)
{
	ssize_t num_read;
	int ch;
	char tempbuf[BUF_LIMIT + 1];
	while(!is_end())
	{
		pthread_mutex_lock(&mutex);
		while(!buffer_empty)
		{
			pthread_cond_wait(&cond2, &mutex);
		}
		buffer_empty = false;
		pthread_mutex_unlock(&mutex);
		if(is_end())
			break;
		while(1)
		{
			printf(">"); fflush(stdout);
			num_read = read(STDIN_FILENO,tempbuf,BUF_LIMIT + 1);
			if(num_read < 0)
			{
				perror("read:");
			}
			if(num_read == BUF_LIMIT + 1)
			{
				if(tempbuf[BUF_LIMIT] == '\n')
				{
					tempbuf[BUF_LIMIT] = 0;
					break;
				}
				fprintf(stderr, "Input overflow!\n");
				while ((ch = getchar()) != '\n' && ch != EOF);
				continue;
			}
			/* trim \n */
			tempbuf[num_read - 1] = 0;
			break;
		}
		pthread_mutex_lock(&mutex);
		strcpy(buffer, tempbuf);
		buffer_ready = true;
		pthread_cond_signal(&cond);
		pthread_mutex_unlock(&mutex);
	}
	
	return NULL;
}

void *executor_func(void *threadid)
{
	while(!is_end())
	{
		pthread_mutex_lock(&mutex);
		while(!buffer_ready)
		{
			pthread_cond_wait(&cond, &mutex);
		}
		buffer_ready = false;
		pthread_mutex_unlock(&mutex);
		if(strcmp(buffer, "exit") == 0)
		{
			set_end();
		}
		else
		{
			parse_and_run(buffer);
		}
		pthread_mutex_lock(&mutex);
		buffer_empty = true;
		pthread_cond_signal(&cond2);
		pthread_mutex_unlock(&mutex);
	}
	return NULL;
}

int main(int argc, char **argv)
{
	pthread_t reader, executor;
	pthread_attr_t attr;
	int rc;
	void *retval;
	
	if(!isatty(STDIN_FILENO))
	{
		fprintf(stderr, "Input is not a terminal, exiting.\n");
		return EXIT_FAILURE;
	}
	
	// ignore SIGINT in all our threads by default
	// along with a few others we don't want delivered to random threads
	sigset_t mask;
	sigemptyset( & mask );
	sigaddset( & mask, SIGINT );
	sigaddset( & mask, SIGCHLD );
	sigaddset( & mask, SIGUSR1 );
	pthread_sigmask( SIG_BLOCK, & mask, NULL );
	
	monitor_new();
	init_background();
	init_is_end();
	
	pthread_attr_init(&attr);
	rc = pthread_create(&reader, &attr, reader_func, NULL);
	if (rc != 0)
	{
		fprintf(stderr, "ERROR; return code from pthread_create() is %d\n", rc);
		exit(EXIT_FAILURE);
	}
	rc = pthread_create(&executor, &attr, executor_func, NULL);
	if (rc != 0)
	{
		fprintf(stderr, "ERROR; return code from pthread_create() is %d\n", rc);
		exit(EXIT_FAILURE);
	}
	// wait for any stray processes, expect the end.
	{
		while(!is_end())
		{
			siginfo_t info;
			int sig;
			// wait for a signal from our mask
			while((sig = sigwaitinfo(&mask,&info)) == -1 && errno == EINTR);
			
			// SIGCHLD && background -> waitpid
			if(sig == SIGCHLD && is_background(info.si_pid))
			{
				wait_for_pid(info.si_pid, false);
				remove_background(info.si_pid);
				// re-print the prompt, if no foreground
				if(!foreground_active)
				{
					printf("\n>");
					fflush(stdout);
				}
			}
			// SIGUSR1 && end -> break
			else if(sig == SIGUSR1 && is_end())
			{
				fprintf(stderr, "GOODBYE\n");
				break;
			}
		}
	}
	rc = pthread_join(reader, &retval);
	if (rc != 0)
	{
		printf("ERROR; return code from pthread_create() is %d\n", rc);
		exit(EXIT_FAILURE);
	}
	rc = pthread_join(executor, &retval);
	if (rc != 0)
	{
		printf("ERROR; return code from pthread_create() is %d\n", rc);
		exit(EXIT_FAILURE);
	}
	pthread_attr_destroy(&attr);
	
	monitor_delete();
	finish_background();
	finish_is_end();
	
	return EXIT_SUCCESS;
}
