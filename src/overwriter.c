#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>
#include <string.h>
#include <inttypes.h>
#include <sys/wait.h>
#include <errno.h>

static void
print_usage (FILE * fp, int argc, char *argv[])
{
  fprintf (fp, "\n");
  fprintf (fp, "Usage:\n");
  fprintf (fp, "  %s file cmd [arg ...]\n", argv[0]);
  fprintf (fp, "\n");
}

int
main (int argc, char *argv[])
{
  if (argc < 3)
    {
      print_usage (stderr, argc, argv);
      exit (EXIT_FAILURE);
    }
  int fd = open (argv[1], O_RDWR);
  if (fd == -1)
    {
      perror (argv[1]);
      exit (EXIT_FAILURE);
    }
  struct stat st;
  if (fstat (fd, &st) == -1)
    {
      perror ("fstat");
      exit (EXIT_FAILURE);
    }
  int pfds[2][2];
  if (pipe (pfds[0]) == -1)
    {
      perror ("pipe");
      exit (EXIT_FAILURE);
    }
  if (pipe (pfds[1]) == -1)
    {
      perror ("pipe");
      exit (EXIT_FAILURE);
    }
  pid_t pid = fork ();
  if (pid == -1)
    {
      perror ("fork");
      exit (EXIT_FAILURE);
    }
  if (pid == 0)
    {
      close (pfds[0][0]);
      close (pfds[1][1]);
      if (pfds[0][1] != STDOUT_FILENO)
	{
	  dup2 (pfds[0][1], STDOUT_FILENO);
	  close (pfds[0][1]);
	}
      if (pfds[1][0] != STDIN_FILENO)
	{
	  dup2 (pfds[1][0], STDIN_FILENO);
	  close (pfds[1][0]);
	}
      execvp (argv[2], argv + 2);
      perror (argv[2]);
      exit (EXIT_FAILURE);
    }
  close (pfds[0][1]);
  close (pfds[1][0]);
  char buf[2][st.st_blksize];
  size_t bsize[2] = { 0, 0 };
  off_t fpos[2] = { 0, 0 };
  int eof[2] = { 0, 0 };
  int closed[2] = { 0, 0 };
  while (1)
    {
      fd_set rfds, wfds;
      int maxfd = -1;
      FD_ZERO (&rfds);
      FD_ZERO (&wfds);
      // CLOSE
      if (eof[0] && bsize[0] == 0 && !closed[0])
	{
	  close (pfds[1][1]);
	  closed[0] = 1;
	}
      if (!eof[0] && bsize[0] < st.st_blksize)
	{
	  FD_SET (fd, &rfds);
	  if (maxfd < fd)
	    maxfd = fd;
	}
      if (!closed[0] && bsize[0] > 0)
	{
	  FD_SET (pfds[1][1], &wfds);
	  if (maxfd < pfds[1][1])
	    maxfd = pfds[1][1];
	}
      if (!eof[1] && bsize[1] < st.st_blksize)
	{
	  FD_SET (pfds[0][0], &rfds);
	  if (maxfd < pfds[0][0])
	    maxfd = pfds[0][0];
	}
      if (!closed[1] && bsize[1] > 0 && fpos[0] > fpos[1])
	{
	  FD_SET (fd, &wfds);
	  if (maxfd < fd)
	    maxfd = fd;
	}
      if (maxfd == -1)
	{
	  if (eof[0] && bsize[0] == 0 && eof[1] && bsize[1] == 0)
	    break;
	  fprintf (stderr, "buffer exceeded\n");
	  fprintf (stderr, "%s(%ju) -> %s (buffer = %zu/pipe buffer = %u)\n",
		   argv[1], (uintmax_t) fpos[0], argv[2], bsize[0], PIPE_BUF);
	  fprintf (stderr, "%s(%ju) <- %s (buffer = %zu/pipe buffer = %u)\n",
		   argv[1], (uintmax_t) fpos[1], argv[2], bsize[1], PIPE_BUF);
	  exit (EXIT_FAILURE);
	}
      int ret = select (maxfd + 1, &rfds, &wfds, NULL, NULL);
      if (ret == -1)
	{
	  perror ("select");
	  exit (EXIT_FAILURE);
	}
      if (FD_ISSET (pfds[1][1], &wfds))
	{
	  ssize_t sz = write (pfds[1][1], buf[0], bsize[0]);
	  if (sz == -1)
	    {
	      perror ("write");
	      exit (EXIT_FAILURE);
	    }
	  memmove (buf[0], buf[0] + sz, bsize[0] - sz);
	  bsize[0] -= sz;
	  continue;
	}
      if (FD_ISSET (pfds[0][0], &rfds))
	{
	  ssize_t sz =
	    read (pfds[0][0], buf[1] + bsize[1], st.st_blksize - bsize[1]);
	  if (sz == -1)
	    {
	      perror ("read");
	      exit (EXIT_FAILURE);
	    }
	  if (sz == 0)
	      eof[1] = 1;
	  else
	      bsize[1] += sz;
	  continue;
	}
      if (FD_ISSET (fd, &rfds))
	{
	  ssize_t sz =
	    pread (fd, buf + bsize[0], st.st_blksize - bsize[0], fpos[0]);
	  if (sz == -1)
	    {
	      perror ("pread");
	      exit (EXIT_FAILURE);
	    }
	  if (sz == 0)
	      eof[0] = 1;
	  else
	    {
	      fpos[0] += sz;
	      bsize[0] += sz;
	    }
	  continue;
	}
      if (FD_ISSET (fd, &wfds))
	{
	  size_t wsize = bsize[1];
	  if (wsize > fpos[0] - fpos[1])
	    wsize = fpos[0] - fpos[1];
	  ssize_t sz = pwrite (fd, buf[1], bsize[1], fpos[1]);
	  if (sz == -1)
	    {
	      perror ("pwrite");
	      exit (EXIT_FAILURE);
	    }
	  memmove (buf[1], buf[1] + sz, bsize[1] - sz);
          fpos[1] += sz;
	  bsize[1] -= sz;
	  continue;
	}
    }
  close (pfds[0][0]);
  if (ftruncate(fd, fpos[1]) == -1) {
    perror(argv[1]);
    exit(EXIT_FAILURE);
  }
  close (fd);
  while (1)
    {
      int status;
      int ret_status = EXIT_FAILURE;
      pid_t pid_child = wait (&status);
      if (pid_child == -1)
	{
	  if (errno == ECHILD)
	    exit (ret_status);
	  perror ("wait");
	  exit (EXIT_FAILURE);
	}
      if (pid_child == pid)
	ret_status = status;
    }
}
