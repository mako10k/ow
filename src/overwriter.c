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
  int fd = open (argv[1], O_RDWR | O_CREAT, 0666);
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
  int ipfds[2];
  int opfds[2];
  if (pipe (ipfds) == -1)
    {
      perror ("pipe");
      exit (EXIT_FAILURE);
    }
  if (pipe (opfds) == -1)
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
      close (ipfds[1]);
      close (opfds[0]);
      if (ipfds[0] != STDIN_FILENO)
	{
	  dup2 (ipfds[0], STDIN_FILENO);
	  close (ipfds[0]);
	}
      if (opfds[1] != STDOUT_FILENO)
	{
	  dup2 (opfds[1], STDOUT_FILENO);
	  close (opfds[1]);
	}
      execvp (argv[2], argv + 2);
      perror (argv[2]);
      exit (EXIT_FAILURE);
    }
  close (ipfds[0]);
  close (opfds[1]);
  char ibuf[st.st_blksize];
  char obuf[st.st_blksize];
  size_t isize = 0;
  size_t osize = 0;
  off_t ipos = 0;
  off_t opos = 0;
  int ieof = 0;
  int oeof = 0;
  int iclosed = 0;
  while (1)
    {
      fd_set rfds, wfds;
      int maxfd = -1;
      FD_ZERO (&rfds);
      FD_ZERO (&wfds);
      // CLOSE
      if (ieof && isize == 0 && !iclosed)
	{
	  close (ipfds[1]);
	  iclosed = 1;
	}
      if (!ieof && isize < st.st_blksize)
	{
	  FD_SET (fd, &rfds);
	  if (maxfd < fd)
	    maxfd = fd;
	}
      if (isize > 0)
	{
	  FD_SET (ipfds[1], &wfds);
	  if (maxfd < ipfds[1])
	    maxfd = ipfds[1];
	}
      if (!oeof && osize < st.st_blksize)
	{
	  FD_SET (opfds[0], &rfds);
	  if (maxfd < opfds[0])
	    maxfd = opfds[0];
	}
      if (osize > 0 && (ieof || ipos > opos))
	{
	  FD_SET (fd, &wfds);
	  if (maxfd < fd)
	    maxfd = fd;
	}
      if (maxfd == -1)
	{
	  if (ieof && isize == 0 && oeof && osize == 0)
	    break;
	  fprintf (stderr, "buffer exceeded\n");
	  fprintf (stderr,
		   "%s(%ju/%ju) -> %s (buffer = %zu/pipe buffer = %u)\n",
		   argv[1], (uintmax_t) ipos, (uintmax_t) st.st_size, argv[2],
		   isize, PIPE_BUF);
	  fprintf (stderr,
		   "%s(%ju/%ju) <- %s (buffer = %zu/pipe buffer = %u)\n",
		   argv[1], (uintmax_t) opos, (uintmax_t) st.st_size, argv[2],
		   osize, PIPE_BUF);
	  exit (EXIT_FAILURE);
	}
      int ret = select (maxfd + 1, &rfds, &wfds, NULL, NULL);
      if (ret == -1)
	{
	  perror ("select");
	  exit (EXIT_FAILURE);
	}
      if (FD_ISSET (ipfds[1], &wfds))
	{
	  ssize_t sz = write (ipfds[1], ibuf, isize);
	  if (sz == -1)
	    {
	      perror ("write");
	      exit (EXIT_FAILURE);
	    }
	  memmove (ibuf, ibuf + sz, isize - sz);
	  isize -= sz;
	  continue;
	}
      if (FD_ISSET (opfds[0], &rfds))
	{
	  ssize_t sz = read (opfds[0], obuf + osize, st.st_blksize - osize);
	  if (sz == -1)
	    {
	      perror ("read");
	      exit (EXIT_FAILURE);
	    }
	  if (sz == 0)
	    oeof = 1;
	  else
	    osize += sz;
	  continue;
	}
      if (FD_ISSET (fd, &rfds))
	{
	  ssize_t sz = pread (fd, ibuf + isize, st.st_blksize - isize, ipos);
	  if (sz == -1)
	    {
	      perror ("pread");
	      exit (EXIT_FAILURE);
	    }
	  if (sz == 0)
	    ieof = 1;
	  else
	    {
	      ipos += sz;
	      isize += sz;
	    }
	  continue;
	}
      if (FD_ISSET (fd, &wfds))
	{
	  size_t wsize = osize;
	  if (!ieof && wsize > ipos - opos)
	    wsize = ipos - opos;
	  ssize_t sz = pwrite (fd, obuf, wsize, opos);
	  if (sz == -1)
	    {
	      perror ("pwrite");
	      exit (EXIT_FAILURE);
	    }
	  memmove (obuf, obuf + sz, st.st_blksize - sz);
	  opos += sz;
	  osize -= sz;
	  continue;
	}
    }
  close (opfds[0]);
  if (ftruncate (fd, opos) == -1)
    {
      perror (argv[1]);
      exit (EXIT_FAILURE);
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
