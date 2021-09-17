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
#include <ctype.h>
#include <sys/sendfile.h>
#include <libgen.h>
#include <locale.h>

#include "config.h"

#define OFF_MAX (~((off_t)1<<(sizeof(off_t)*8-1)))

struct opt
{
  const char *file_input;
  const char *file_output;
  const char *file_rename;
  int append:1;
  int punchhole:1;
  int file_stdin:1;
  int file_stdout:1;
};

#define OPT_INITIALIZER {\
  .file_input = NULL,\
  .file_output = NULL,\
  .file_rename = NULL,\
  .append = 0,\
  .punchhole = 0,\
  .file_stdin = 0,\
  .file_stdout = 0,\
}

static void
print_version (FILE * fp)
{
  fprintf (fp, "%s\n", PACKAGE_STRING);
}

static void
print_usage (FILE * fp, int argc, char *const argv[])
{
  fprintf (fp, "Usage:\n");
  fprintf (fp, "  %s [options] [--] cmd [arg ...] [redirects]\n", argv[0]);
  fprintf (fp, "\n");
  fprintf (fp, "Options:\n");
  fprintf (fp, "  -i infile     : input file\n");
  fprintf (fp, "  -o outfile    : output file\n");
  fprintf (fp, "  -f inoutfile  : input/output file\n");
  fprintf (fp, "  -r renamefile : rename output file\n");
  fprintf (fp, "  -a            : append mode\n");
  fprintf (fp,
	   "  -p            : punchhole mode (punchhole read data on input file)\n");
  fprintf (fp, "  -V            : show version\n");
  fprintf (fp, "  -h            : show usage\n");
  fprintf (fp, "\n");
  fprintf (fp, "Redirects:\n");
  fprintf (fp, "  < infile      : input file\n");
  fprintf (fp, "  > outfile     : output file\n");
  fprintf (fp, "  >> outfile    : output file (append mode)\n");
  fprintf (fp, "  <> inoutfile  : input/output file\n");
  fprintf (fp, "  <>> inoutfile : input/output file (append mode)\n");
  fprintf (fp, "\n");
  fprintf (fp, "  NOTE: You can use same file for input and output.\n");
  fprintf (fp,
	   "        It writes to output file only read position to safe read.\n");
  fprintf (fp,
	   "        But you shouldn't output widely incresed size data against input\n");
  fprintf (fp, "        when you use same file for input and output.\n");
  fprintf (fp,
	   "        It would be stopped program because the all buffer consumed\n");
  fprintf (fp,
	   "        to wait forever writing for read position on the file.\n");
  fprintf (fp, "\n");
  fprintf (fp, "  NOTE: < and > must escape or quote on shell.\n");
  fprintf (fp, "    example:\n");
  fprintf (fp,
	   "      %s -p -r hugefile.txt.gz gzip -c '<hugefile.txt' \\> hugefile.txt\n",
	   argv[0]);
  fprintf (fp, "\n");
  fprintf (fp,
	   "  NOTE: Using same file for input and output or punchhole option\n");
  fprintf (fp, "        may destructive.\n");
  fprintf (fp, "\n");
}

static const char *getfilename (int) __attribute__((malloc));
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
  ssize_t ret = readlink (path, buf, st.st_size);
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

static void pump_read_write (int[2], off_t, size_t) __attribute__((noreturn));

static void
pump_read_write (int fds[2], off_t size, size_t size_buf)
{
  char buf[size_buf];
  off_t size_transfered = 0;
  while (size_transfered < size)
    {
      size_t size_to_read =
	size - size_transfered > size_buf ? size_buf : size - size_transfered;
      if (size_to_read == 0)
	exit (EXIT_SUCCESS);
      ssize_t size_read = read (fds[0], buf, size_to_read);
      if (size_read == -1)
	{
	  perror ("read");
	  exit (EXIT_FAILURE);
	}
      if (size_read == 0)
	exit (EXIT_SUCCESS);
      ssize_t size_written = write (fds[1], buf, size_read);
      if (size_written == -1)
	{
	  perror ("write");
	  exit (EXIT_FAILURE);
	}
      size_transfered += size_written;
    }
  exit (EXIT_SUCCESS);
}

static void pump_splice (int[2], off_t) __attribute__((noreturn));

static void
pump_splice (int fds[2], off_t size)
{
  off_t size_transfered = 0;
  while (size_transfered < size)
    {
      size_t size_to_splice =
	size - size_transfered > SIZE_MAX ? SIZE_MAX : size - size_transfered;
      if (size_to_splice == 0)
	exit (EXIT_SUCCESS);
      ssize_t size_spliced =
	splice (fds[0], NULL, fds[1], NULL, size_to_splice, 0);
      if (size_spliced == -1)
	{
	  perror ("splice");
	  exit (EXIT_FAILURE);
	}
      if (size_spliced == 0)
	exit (EXIT_SUCCESS);
      size_transfered += size_spliced;
    }
  exit (EXIT_SUCCESS);
}

static void pump_sendfile (int[2], off_t) __attribute__((noreturn));

static void
pump_sendfile (int fds[2], off_t size)
{
  off_t size_transfered = 0;
  while (size_transfered < size)
    {
      size_t size_to_send =
	size - size_transfered > SIZE_MAX ? SIZE_MAX : size - size_transfered;
      if (size_to_send == 0)
	exit (EXIT_SUCCESS);
      ssize_t size_sent = sendfile (fds[1], fds[0], NULL, size_to_send);
      if (size_sent == -1)
	{
	  perror ("sendfile");
	  exit (EXIT_FAILURE);
	}
      if (size_sent == 0)
	exit (EXIT_SUCCESS);
      size_transfered += size_sent;
    }
  exit (EXIT_SUCCESS);
}

static void pump (int[2]) __attribute__((noreturn));

static void
pump (int fds[2])
{
  struct stat st[2];
  if (fstat (fds[0], st + 0) == -1)
    {
      perror ("fstat");
      exit (EXIT_FAILURE);
    }
  if (fstat (fds[1], st + 1) == -1)
    {
      perror ("fstat");
      exit (EXIT_FAILURE);
    }
  int flags = fcntl (fds[1], F_GETFL);
  if (flags == -1)
    {
      perror ("fcntl(..., F_GETFL)");
      exit (EXIT_FAILURE);
    }
  off_t size_to_transfer = OFF_MAX;
  int append = (flags & O_APPEND) != 0;
  if (S_ISREG (st[0].st_mode) && st[0].st_dev == st[1].st_dev
      && st[0].st_ino == st[1].st_ino && append)
    size_to_transfer = st[0].st_size;
  if (append)
    pump_read_write (fds, size_to_transfer, PIPE_BUF);
  if (S_ISREG (st[0].st_mode))
    pump_sendfile (fds, size_to_transfer);
  if (S_ISFIFO (st[0].st_mode) || S_ISFIFO (st[1].st_mode))
    pump_splice (fds, size_to_transfer);
  pump_read_write (fds, size_to_transfer, PIPE_BUF);
}

static void
parse_redirect (int argc, char **argv, struct opt *opt)
{
  for (int i = 1; i < argc; i++)
    {
      // "\\<...", "\\>...", "\\\\<..." or "\\\\>..." is escaped argument
      if (argv[i][0] == '\\'
	  && (argv[i][1] == '<' || argv[i][1] == '>'
	      || (argv[i][1] == '\\'
		  && (argv[i][2] == '<' || argv[i][2] == '>'))))
	{
	  argv[i]++;
	  continue;
	}
      // <>>...
      // <>...
      if (argv[i][0] == '<' || argv[i][0] == '>')
	{
	  int in = 0;
	  int out = 0;
	  const char *file = argv[i];
	  int rargc = 0;
	  char *rargv[2];
	  char op[4];
	  char *o = op;

	  rargv[rargc++] = argv[i];
	  if (*file == '<')
	    {
	      in = 1;
	      *o++ = *file;
	      file++;
	    }
	  if (*file == '>')
	    {
	      out = 1;
	      *o++ = *file;
	      file++;
	    }
	  if (*file == '>')
	    {
	      opt->append = 1;
	      *o++ = *file;
	      file++;
	    }
	  *o = '\0';
	  while (isspace (*file))
	    file++;
	  if (*file == '\0')
	    {
	      i++;
	      if (i >= argc)
		{
		  fprintf (stderr, "no file specified for %s\n", op);
		  print_usage (stderr, argc, argv);
		  exit (EXIT_FAILURE);
		}
	      rargv[rargc++] = argv[i];
	      file = argv[i];
	    }
	  if (in && opt->file_input != NULL)
	    {
	      fprintf (stderr, "cannot set input file twice or more\n");
	      print_usage (stderr, argc, argv);
	      exit (EXIT_FAILURE);
	    }
	  if (out && opt->file_output != NULL)
	    {
	      fprintf (stderr, "cannot set output file twice or more\n");
	      print_usage (stderr, argc, argv);
	      exit (EXIT_FAILURE);
	    }
	  if (in)
	    opt->file_input = file;
	  if (out)
	    opt->file_output = file;
	  memmove (argv + optind + rargc, argv + optind,
		   sizeof (char *) * (i - optind));
	  memcpy (argv + optind, rargv, sizeof (char *) * rargc);
	  optind += rargc;
	  continue;
	}
    }
}

static void
parse_options (int argc, char *argv[], struct opt *opt)
{
  while (1)
    {
      int c = getopt (argc, argv, "+i:o:f:r:apVh");
      if (c == -1)
	break;
      switch (c)
	{
	case 'i':
	  if (opt->file_input != NULL)
	    {
	      fprintf (stderr, "cannot set input file twice or more\n");
	      print_usage (stderr, argc, argv);
	      exit (EXIT_FAILURE);
	    }
	  opt->file_input = optarg;
	  break;
	case 'o':
	  if (opt->file_output != NULL)
	    {
	      fprintf (stderr, "cannot set output file twice or more\n");
	      print_usage (stderr, argc, argv);
	      exit (EXIT_FAILURE);
	    }
	  opt->file_output = optarg;
	  break;
	case 'f':
	  if (opt->file_input != NULL)
	    {
	      fprintf (stderr, "cannot set input file twice or more\n");
	      print_usage (stderr, argc, argv);
	      exit (EXIT_FAILURE);
	    }
	  if (opt->file_output != NULL)
	    {
	      fprintf (stderr, "cannot set output file twice or more\n");
	      print_usage (stderr, argc, argv);
	      exit (EXIT_FAILURE);
	    }
	  opt->file_input = optarg;
	  opt->file_output = optarg;
	  break;
	case 'r':
	  if (opt->file_rename != NULL)
	    {
	      fprintf (stderr, "cannot set rename file twice or more\n");
	      print_usage (stderr, argc, argv);
	      exit (EXIT_FAILURE);
	    }
	  opt->file_rename = optarg;
	  break;
	case 'a':
	  if (opt->append)
	    {
	      fprintf (stderr, "cannot set append mode twice or more\n");
	      print_usage (stderr, argc, argv);
	      exit (EXIT_FAILURE);
	    }
	  opt->append = 1;
	  break;
	case 'p':
	  if (opt->punchhole)
	    {
	      fprintf (stderr, "cannot set punchhole mode twice or more\n");
	      print_usage (stderr, argc, argv);
	      exit (EXIT_FAILURE);
	    }
	  opt->punchhole = 1;
	  break;
	case 'V':
	  print_version (stdout);
	  exit (EXIT_SUCCESS);
	case 'h':
	  print_version (stdout);
	  print_usage (stdout, argc, argv);
	  exit (EXIT_SUCCESS);
	default:
	  print_usage (stderr, argc, argv);
	  exit (EXIT_FAILURE);
	}
    }
}

static void
check_stdio (struct opt *opt)
{
  // IDENTIFY STDIN / STDOUT
  struct stat st_stdin;
  struct stat st_stdout;

  if (fstat (STDIN_FILENO, &st_stdin) == -1)
    {
      perror ("fstat");
      exit (EXIT_FAILURE);
    }
  if (fstat (STDOUT_FILENO, &st_stdout) == -1)
    {
      perror ("fstat");
      exit (EXIT_FAILURE);
    }
  if (S_ISREG (st_stdin.st_mode))
    {
      opt->file_input = getfilename (STDIN_FILENO);
      opt->file_stdin = 1;
    }
  if (S_ISREG (st_stdout.st_mode))
    {
      opt->file_output = getfilename (STDOUT_FILENO);
      opt->file_stdin = 1;
      int flags = fcntl (STDOUT_FILENO, F_GETFL);
      if (flags == -1)
	{
	  perror ("fcntl(STDOUT_FILENO, F_GETFL)");
	  exit (EXIT_FAILURE);
	}
      opt->append = (flags & O_APPEND) != 0;
    }
}

static void
setopenflags (int fd, int flags)
{
  int curflags = fcntl (fd, F_GETFL);
  if (curflags == -1)
    {
      perror ("fcntl(..., F_GETFL)\n");
      exit (EXIT_FAILURE);
    }
  curflags |= flags;
  if (fcntl (fd, F_SETFL, flags) == -1)
    {
      perror ("cannot set append mode on <stdout>\n");
      exit (EXIT_FAILURE);
    }
}

static void
open_iofile (struct opt *opt, int fds[2])
{
  if (opt->file_input && !opt->file_stdin)
    {
      int flags = O_RDONLY | O_CLOEXEC;
      if (opt->punchhole)
	{
	  flags &= ~O_ACCMODE;
	  flags |= O_RDWR;
	}
      fds[0] = open (opt->file_input, flags);
      if (fds[0] == -1)
	{
	  perror (opt->file_input);
	  exit (EXIT_FAILURE);
	}
    }
  else
    {
      fds[0] = STDIN_FILENO;
      if (opt->punchhole)
	{
	  fprintf (stderr, "cannot set punchhole mode for outer redirect\n");
	  exit (EXIT_FAILURE);
	}
    }
  if (opt->file_output && !opt->file_stdout)
    {
      int flags = O_WRONLY | O_CREAT | O_CLOEXEC;
      if (opt->append)
	flags |= O_APPEND;
      fds[1] = open (opt->file_output, flags, 0666);
      if (fds[1] == -1)
	{
	  perror (opt->file_output);
	  exit (EXIT_FAILURE);
	}
    }
  else
    {
      fds[1] = STDOUT_FILENO;
      if (opt->append)
	setopenflags (fds[1], O_APPEND);
    }
}

int
main (int argc, char *argv[])
{
  setlocale (LC_ALL, "");
  struct opt opt = OPT_INITIALIZER;

  check_stdio (&opt);
  parse_redirect (argc, argv, &opt);
  parse_options (argc, argv, &opt);

  int fds[2];
  open_iofile (&opt, fds);

  struct stat st[3];
  if (fstat (fds[0], st + 0) == -1)
    {
      perror ("fstat");
      exit (EXIT_FAILURE);
    }
  if (fstat (fds[1], st + 1) == -1)
    {
      perror ("fstat");
      exit (EXIT_FAILURE);
    }
  if (opt.file_rename != NULL)
    {
      if (!S_ISREG (st[1].st_mode))
	{
	  fprintf (stderr, "cannot rename non regular output\n");
	  exit (EXIT_FAILURE);
	}
      if (lstat (opt.file_rename, st + 2) == -1)
	{
	  if (errno != ENOENT)
	    {
	      perror ("lstat");
	      exit (EXIT_FAILURE);
	    }
	  char *file = strdup (opt.file_rename);
	  if (file == NULL)
	    {
	      perror ("strdup");
	      exit (EXIT_FAILURE);
	    }
	  char *dir = dirname (file);
	  if (stat (dir, st + 2) == -1)
	    {
	      perror (dir);
	      exit (EXIT_FAILURE);
	    }
	  if (!S_ISDIR (st[2].st_mode))
	    {
	      errno = ENOTDIR;
	      perror (dir);
	      exit (EXIT_FAILURE);
	    }
	  free (dir);
	  if (st[1].st_dev != st[2].st_dev)
	    {
	      errno = EXDEV;
	      perror (opt.file_rename);
	      exit (EXIT_FAILURE);
	    }
	}
      else
	{
	  if (S_ISDIR (st[2].st_mode))
	    {
	      errno = EISDIR;
	      perror (opt.file_rename);
	      exit (EXIT_FAILURE);
	    }
	  if (st[1].st_dev != st[2].st_dev)
	    {
	      errno = EXDEV;
	      perror (opt.file_rename);
	      exit (EXIT_FAILURE);
	    }
	  else if (st[1].st_ino == st[2].st_ino)
	    {
	      fprintf (stderr, "cannot rename to same file\n");
	      exit (EXIT_FAILURE);
	    }
	}
    }

  int overwrite = st[0].st_dev == st[1].st_dev && st[0].st_ino == st[1].st_ino
    && S_ISREG (st[0].st_mode) && S_ISREG (st[1].st_mode);
  if (opt.append && !S_ISREG (st[1].st_mode))
    {
      fprintf (stderr, "cannot append to non regular file\n");
      print_usage (stderr, argc, argv);
      exit (EXIT_FAILURE);
    }
  if (argc <= optind && !opt.punchhole && opt.file_rename == NULL)
    {
      if (!opt.append && S_ISREG (st[1].st_mode))
	{
	  if (ftruncate (fds[1], 0) == -1)
	    {
	      perror ("ftruncate");
	      exit (EXIT_FAILURE);
	    }
	}
      pump (fds);
    }
  if (!overwrite && !opt.punchhole && opt.file_rename == NULL)
    {
      dup2 (fds[0], STDIN_FILENO);
      dup2 (fds[1], STDOUT_FILENO);
      execvp (argv[optind], argv + optind);
      perror (argv[optind]);
      exit (EXIT_FAILURE);
    }
  int ipfds[2];
  int opfds[2];
  if (pipe2 (ipfds, O_CLOEXEC) == -1)
    {
      perror ("pipe");
      exit (EXIT_FAILURE);
    }
  if (pipe2 (opfds, O_CLOEXEC) == -1)
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
      if (argc <= optind)
	{
	  int pfds[2] = { ipfds[0], opfds[1] };
	  pump (pfds);
	}
      dup2 (ipfds[0], STDIN_FILENO);
      dup2 (opfds[1], STDOUT_FILENO);
      execvp (argv[optind], argv + optind);
      perror (argv[optind]);
      exit (EXIT_FAILURE);
    }
  close (ipfds[0]);
  close (opfds[1]);
  char ibuf[st[0].st_blksize];
  char obuf[st[1].st_blksize];
  size_t isize = 0;
  size_t osize = 0;
  off_t ipos = 0;
  off_t opos = opt.append ? st[1].st_size : 0;
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
      if (oeof && osize == 0)
	break;
      if (!ieof && isize < st[0].st_blksize)
	{
	  FD_SET (fds[0], &rfds);
	  if (maxfd < fds[0])
	    maxfd = fds[0];
	}
      if (isize > 0)
	{
	  FD_SET (ipfds[1], &wfds);
	  if (maxfd < ipfds[1])
	    maxfd = ipfds[1];
	}
      if (!oeof && osize < st[1].st_blksize)
	{
	  FD_SET (opfds[0], &rfds);
	  if (maxfd < opfds[0])
	    maxfd = opfds[0];
	}
      if (osize > 0 && (!overwrite || opt.append || ieof || ipos > opos))
	{
	  FD_SET (fds[1], &wfds);
	  if (maxfd < fds[1])
	    maxfd = fds[1];
	}
      if (maxfd == -1)
	{
	  if (ieof && isize == 0 && oeof && osize == 0)
	    break;
	  fprintf (stderr, "buffer exceeded\n");
	  fprintf (stderr,
		   "%s(%ju/%ju) -> %s (buffer = %zu/pipe buffer = %u)\n",
		   opt.file_input ==
		   NULL ? "<stdin>" : getrelative (opt.file_input),
		   (uintmax_t) ipos, (uintmax_t) st[0].st_size,
		   argv[optind] == NULL ? argv[0] : argv[optind], isize,
		   PIPE_BUF);
	  fprintf (stderr,
		   "%s(%ju/%ju) <- %s (buffer = %zu/pipe buffer = %u)\n",
		   opt.file_output ==
		   NULL ? "<stdout>" : getrelative (opt.file_output),
		   (uintmax_t) opos, (uintmax_t) st[1].st_size,
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
	  ssize_t sz =
	    read (opfds[0], obuf + osize, st[1].st_blksize - osize);
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
      if (FD_ISSET (fds[0], &rfds))
	{
	  size_t rsize = st[0].st_blksize - isize;
	  if (overwrite && opt.append && st[0].st_size - ipos < rsize)
	    rsize = st[0].st_size - ipos;
	  ssize_t sz = rsize == 0 ? 0 : read (fds[0], ibuf + isize, rsize);
	  if (sz == -1)
	    {
	      perror ("pread");
	      exit (EXIT_FAILURE);
	    }
	  if (sz == 0)
	    ieof = 1;
	  else
	    {
	      if (opt.punchhole)
		{
		  if (fallocate
		      (fds[0], FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
		       ipos, sz) == -1)
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
      if (FD_ISSET (fds[1], &wfds))
	{
	  size_t wsize = osize;
	  if (!ieof && overwrite && !opt.append && wsize > ipos - opos)
	    wsize = ipos - opos;
	  ssize_t sz = write (fds[1], obuf, wsize);
	  if (sz == -1)
	    {
	      perror ("pwrite");
	      exit (EXIT_FAILURE);
	    }
	  memmove (obuf, obuf + sz, st[1].st_blksize - sz);
	  opos += sz;
	  osize -= sz;
	  continue;
	}
    }
  close (fds[0]);
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
	      if (overwrite && ftruncate (fds[1], opos) == -1)
		{
		  perror (opt.file_output);
		  exit (EXIT_FAILURE);
		}
	      close (fds[1]);
	      if (opt.file_rename != NULL)
		{
		  if (opt.file_output != NULL
		      && rename (opt.file_output, opt.file_rename) == -1)
		    {
		      perror (opt.file_rename);
		      exit (EXIT_FAILURE);
		    }
		}
	    }
	}
    }
}
