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
  fprintf (fp, "  %s [options] [--] cmd [arg ...]\n", argv[0]);
  fprintf (fp, "\n");
  fprintf (fp, "Options:\n");
  fprintf (fp, "  -r infile     : input        file [<stdin>]\n");
  fprintf (fp, "  -w outfile    : output       file [<stdout>]\n");
  fprintf (fp, "  -o inoutfile  : input/output file\n");
  fprintf (fp, "  -n renamefile : rename output file\n");
  fprintf (fp, "  -a            : append output file\n");
  fprintf (fp, "  -t            : test run\n");
  fprintf (fp, "  -p            : punchhole after read\n");
  fprintf (fp, "  -h            : show usage\n");
  fprintf (fp, "\n");
}

static const char *
getfilename (int fd)
{
  size_t sz = snprintf (NULL, 0, "/proc/self/fd/%d", fd);
  char path[sz + 1];
  snprintf (path, sz + 1, "/proc/self/fd/%d", fd);
  struct stat st;
  if (lstat (path, &st) == -1)
    {
      perror (path);
      exit (EXIT_FAILURE);
    }
  char *buf = malloc (st.st_size + 1);
  if (buf == NULL)
    {
      perror ("realloc");
      exit (EXIT_FAILURE);
    }
  ssize_t ret = readlink (path, buf, st.st_size + 1);
  buf[ret] = '\0';
  return buf;
}

static const char *
getrelative (const char *path)
{
  char cwd[PATH_MAX];
  if (getcwd (cwd, PATH_MAX) == NULL)
    {
      perror ("getcwd");
      exit (EXIT_FAILURE);
    }
  char *c = cwd;
  const char *p = path;
  while (1)
    {
      if (*c == '\0')
	return *p == '\0' ? "." : p + 1;
      if (*c != *p)
	break;
      c++;
      p++;
    }
  return path;
}

int
main (int argc, char *argv[])
{
  const char *ifile = NULL;
  const char *ofile = NULL;
  const char *rfile = NULL;
  int append = 0;
  int test = 0;
  int punchhole = 0;
  int overwrite = 0;

  while (1)
    {
      int c = getopt (argc, argv, "r:w:o:n:atph");
      if (c == -1)
	break;
      switch (c)
	{
	case 'r':
	  ifile = optarg;
	  break;
	case 'w':
	  ofile = optarg;
	  break;
	case 'o':
	  ifile = optarg;
	  ofile = optarg;
	  break;
	case 'n':
	  rfile = optarg;
	  break;
	case 'a':
	  append = 1;
	  break;
	case 't':
	  test = 1;
	  break;
	case 'p':
	  punchhole = 1;
	  break;
	case 'h':
	  print_usage (stdout, argc, argv);
	  exit (EXIT_SUCCESS);
	default:
	  exit (EXIT_FAILURE);
	}
    }

  int ifd = STDIN_FILENO;
  int ofd = STDOUT_FILENO;

  if (ifile != NULL)
    {
      int flags;
      if (punchhole && !test)
	flags = O_RDWR | O_CLOEXEC;
      else
	flags = O_RDONLY;
      ifd = open (ifile, flags, 0666);
      if (ifd == -1)
	{
	  perror (ifile);
	  exit (EXIT_FAILURE);
	}
    }
  if (ofile != NULL)
    {
      int flags;
      if (append)
	flags = O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC;
      else
	flags = O_WRONLY | O_CREAT | O_CLOEXEC;
      ofd = open (ofile, flags, 0666);
      if (ofd == -1)
	{
	  perror (ofile);
	  exit (EXIT_FAILURE);
	}
    }

  struct stat ist;
  struct stat ost;
  off_t opos = 0;
  if (fstat (ifd, &ist) == -1)
    {
      perror ("fstat");
      exit (EXIT_FAILURE);
    }
  if (fstat (ofd, &ost) == -1)
    {
      perror ("fstat");
      exit (EXIT_FAILURE);
    }
  if (ist.st_dev == ost.st_dev && ist.st_ino == ost.st_ino
      && S_ISREG (ost.st_mode))
    {
      overwrite = 1;
      if (append)
	opos = ost.st_size;
    }
  if (ifile == NULL && S_ISREG (ist.st_mode))
    ifile = getfilename (ifd);
  if (ofile == NULL && S_ISREG (ost.st_mode))
    ofile = getfilename (ofd);
  if (rfile != NULL && !S_ISREG (ost.st_mode))
    {
      fprintf (stderr, "cannot rename from non regular file\n");
      print_usage (stderr, argc, argv);
      exit (EXIT_FAILURE);
    }
  if (append && !S_ISREG (ost.st_mode))
    {
      fprintf (stderr, "cannot append to non regular file\n");
      print_usage (stderr, argc, argv);
      exit (EXIT_FAILURE);
    }
  if (test)
    {
      close (ofd);
      ofd = open ("/dev/null", O_WRONLY);
      if (ofd == -1)
	{
	  perror ("/dev/null");
	  exit (EXIT_FAILURE);
	}
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
      if (argc <= optind)
	{
	  char buf[PIPE_BUF];
	  while (1)
	    {
	      ssize_t rsize = read (STDIN_FILENO, buf, PIPE_BUF);
	      if (rsize == -1)
		{
		  perror ("read");
		  exit (EXIT_FAILURE);
		}
	      if (rsize == 0)
		exit (EXIT_SUCCESS);
	      ssize_t wsize = write (STDOUT_FILENO, buf, rsize);
	      if (wsize == -1)
		{
		  perror ("write");
		  exit (EXIT_FAILURE);
		}
	      if (wsize < rsize)
		{
		  fprintf (stderr, "short read from pipe\n");
		  exit (EXIT_FAILURE);
		}
	    }
	}
      execvp (argv[optind], argv + optind);
      perror (argv[optind]);
      exit (EXIT_FAILURE);
    }
  close (ipfds[0]);
  close (opfds[1]);
  char ibuf[ist.st_blksize];
  char obuf[ost.st_blksize];
  size_t isize = 0;
  size_t osize = 0;
  off_t ipos = 0;
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
      if (!ieof && isize < ist.st_blksize)
	{
	  FD_SET (ifd, &rfds);
	  if (maxfd < ifd)
	    maxfd = ifd;
	}
      if (isize > 0)
	{
	  FD_SET (ipfds[1], &wfds);
	  if (maxfd < ipfds[1])
	    maxfd = ipfds[1];
	}
      if (!oeof && osize < ost.st_blksize)
	{
	  FD_SET (opfds[0], &rfds);
	  if (maxfd < opfds[0])
	    maxfd = opfds[0];
	}
      if (osize > 0 && (!overwrite || ieof || ipos > opos))
	{
	  FD_SET (ofd, &wfds);
	  if (maxfd < ofd)
	    maxfd = ofd;
	}
      if (maxfd == -1)
	{
	  if (ieof && isize == 0 && oeof && osize == 0)
	    break;
	  fprintf (stderr, "buffer exceeded\n");
	  fprintf (stderr,
		   "%s(%ju/%ju) -> %s (buffer = %zu/pipe buffer = %u)\n",
		   ifile == NULL ? "<stdin>" : getrelative (ifile),
		   (uintmax_t) ipos, (uintmax_t) ist.st_size,
		   argv[optind] == NULL ? argv[0] : argv[optind], isize,
		   PIPE_BUF);
	  fprintf (stderr,
		   "%s(%ju/%ju) <- %s (buffer = %zu/pipe buffer = %u)\n",
		   ofile == NULL ? "<stdout>" : getrelative (ofile),
		   (uintmax_t) opos, (uintmax_t) ist.st_size,
		   argv[optind] == NULL ? argv[0] : argv[optind], osize,
		   PIPE_BUF);
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
	  ssize_t sz = read (opfds[0], obuf + osize, ost.st_blksize - osize);
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
      if (FD_ISSET (ifd, &rfds))
	{
	  size_t rsize = ist.st_blksize - isize;
	  if (overwrite && append && ist.st_size - ipos < rsize)
	    rsize = ist.st_size - ipos;
	  ssize_t sz =
	    rsize == 0 ? 0 : read (ifd, ibuf + isize, ist.st_blksize - isize);
	  if (sz == -1)
	    {
	      perror ("pread");
	      exit (EXIT_FAILURE);
	    }
	  if (sz == 0)
	    ieof = 1;
	  else
	    {
	      if (punchhole && !test)
		{
		  if (fallocate
		      (ifd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, ipos,
		       sz) == -1)
		    {
		      perror ("fallocate");
		      exit (EXIT_FAILURE);
		    }
		}
	      ipos += sz;
	      isize += sz;
	    }
	  continue;
	}
      if (FD_ISSET (ofd, &wfds))
	{
	  size_t wsize = osize;
	  if (!ieof && overwrite && wsize > ipos - opos)
	    wsize = ipos - opos;
	  ssize_t sz = write (ofd, obuf, wsize);
	  if (sz == -1)
	    {
	      perror ("pwrite");
	      exit (EXIT_FAILURE);
	    }
	  memmove (obuf, obuf + sz, ost.st_blksize - sz);
	  opos += sz;
	  osize -= sz;
	  continue;
	}
    }
  close (ifd);
  close (opfds[0]);
  int ret_status = EXIT_FAILURE;
  while (1)
    {
      int status;
      pid_t pid_child = wait (&status);
      if (pid_child == -1)
	{
	  if (errno == ECHILD)
	    exit (ret_status);
	  perror ("wait");
	  exit (EXIT_FAILURE);
	}
      if (pid_child == pid)
	{
	  if (WIFEXITED (status))
	    ret_status = WEXITSTATUS (status);
	  if (opos > 0 || ret_status == EXIT_SUCCESS)
	    {
	      if (test)
		{
		  fprintf (stderr, "%s(%ju bytes) -> %s -> %s(%ju bytes)\n",
			   ifile == NULL ? "<stdin>" : getrelative (ifile),
			   (uintmax_t) ipos,
			   argv[optind] == NULL ? argv[0] : argv[optind],
			   ofile == NULL ? "<stdout>" : getrelative (ofile),
			   (uintmax_t) opos);
		}
	      else if (overwrite && ftruncate (ofd, opos) == -1)
		{
		  perror (ofile);
		  exit (EXIT_FAILURE);
		}
	      close (ofd);
	      if (rfile != NULL)
		{
		  if (test)
		    fprintf (stderr, "rename %s -> %s\n", getrelative (ofile),
			     rfile);
		  else if (ofile != NULL && rename (ofile, rfile) == -1)
		    {
		      perror (rfile);
		      exit (EXIT_FAILURE);
		    }
		}
	    }
	}
    }
}