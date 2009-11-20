#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <netinet/in.h>

/* 
 * Written by Nikolai Zeldovich, modified by Igor Ganichev
 */

static FILE *cur_f;	/* connection being currently handled */

/* Get at most size bytes of data form file f into buf, 
 * trimming newline caracters at the end */
static int
fgets_trim(char *buf, int size, FILE *f)   
 /*
 * If we provide a negative value here for size, C will implicitly cast this to an unsigned int thus 
 * yeilding an extermely large positive value. Consequently, p will point to a place on the stack that is
 * very large.
 */ 
{                                           
  char *p = fgets(buf, size, f);
  if (!p)
    return -1;

  /* Trim newline characters at the end of the line */
  while (buf[strlen(buf) - 1] == '\r' ||
	 buf[strlen(buf) - 1] == '\n')
    buf[strlen(buf) - 1] = '\0';

  return 0;
}

// There is a discussion note on this -- it also might show up online somehwhere.
static void
url_decode(char *src, char *dst)
{
  for (;;) {
    if (src[0] == '%' && src[1] && src[2]) {
      char hexbuf[3];
      hexbuf[0] = src[1];
      hexbuf[1] = src[2];
      hexbuf[2] = '\0';
        
      /* convert a hex string into long int */
      *dst = strtol(&hexbuf[0], 0, 16);
      src += 3;
    } else {
      *dst = *src;
      src++;

      if (*dst == '\0')
	break;
    }
    
    dst++;
  }
}

static char *
parse_req(char *reqpath)
{
  static char buf[8192];	/* static variables are not on the stack */

  if (fgets_trim(&buf[0], sizeof(buf), cur_f) < 0)
    return "Socket IO error";

  /** Parse request like "GET /foo.html HTTP/1.0" **/
  /* Separate the first word */
  char *sp1 = strchr(&buf[0], ' ');
  if (!sp1)
    return "Cannot parse HTTP request (1)";
  *sp1 = '\0';
  sp1++;

  /* Separate the second word */
  char *sp2 = strchr(sp1, ' ');
  if (!sp2)
    return "Cannot parse HTTP request (2)";
  *sp2 = '\0';
  sp2++;

  /* We only support GET requests */
  if (strcmp(&buf[0], "GET"))
    return "Non-GET request";

  /* Decode URL escape sequences in the requested path into reqpath */
  url_decode(sp1, reqpath);

  /* Parse out query string, e.g. "foo.py?user=bob" */
  char *qp = strchr(reqpath, '?');
  if (qp) {
    *qp = '\0';
    setenv("QUERY_STRING", qp+1, 1);
  }

  /* Now parse HTTP headers */
  for (;;) {
    if (fgets_trim(&buf[0], sizeof(buf), cur_f) < 0)
      return "Socket IO error";

    if (buf[0] == '\0')	/* end of headers */
      break;
    
    /* Parse things like "Cookie: foo bar" */
    char *sp = strchr(&buf[0], ' ');
    if (!sp)
      return "Header parse error (1)";
    *sp = '\0';
    sp++;

    /* Strip off the colon, making sure it's there */
    if (strlen(buf) == 0)
      return "Header parse error (2)";

    char *colon = &buf[strlen(buf) - 1];
    if (*colon != ':')
      return "Header parse error (3)";
    *colon = '\0';

    /* Set the header name to uppercase */
    for (int i = 0; i < strlen(buf); i++)
      buf[i] = toupper(buf[i]);

    /* Decode URL escape sequences in the value */
    char value[256];
    url_decode(sp, &value[0]);

    /* Store header in env. variable for application code */
    char envvar[256];
    sprintf(&envvar[0], "HTTP_%s", buf);
    setenv(envvar, value, 1);
  }
  return 0;
}

/* Send an HTTP Error reply with error code code, closing the connection afterwards.
 * f is the file stream wrapping the connection fd. This function takes
 * a variable number of arguments like printf. For a simple example and explanation
 * see http://bobobobo.wordpress.com/2008/01/28/how-to-use-variable-argument-lists-va_list/
 */
static void
http_err(FILE *f, int code, char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);

  fprintf(f, "HTTP/1.0 %d Error\r\n", code);
  fprintf(f, "Content-Type: text/html\r\n");
  fprintf(f, "\r\n");
  fprintf(f, "<H1>An error occurred</H1>\r\n");
  vfprintf(f, fmt, ap);
  
  va_end(ap);
  fclose(f);
}


/* Process the client request, where dir is the web home directory and
 * fd is the file descriptor of the connection to the client. 
 */
static void
process_client(const char *dir, int fd)
{
  /* Allocate a buffer for HTTP request path. Wrap our connection in a file
   * stream so that we can use all the useful functions like fprintf, fscanf 
   * directly on our connection as if it was an opened file.
   */
  char reqpath[256];
  FILE *f = fdopen(fd, "w+");
  cur_f = f;

  /* Parse the HTTP request and put the requested file path into reqpath.
   * Send an HTTP error reply if request is invalid */
  char *err = parse_req(&reqpath[0]);
  if (err) {
    http_err(f, 500, "Error parsing request: %s", err);
    return;
  }

  /* Allocate space for the complete file name ( home_directory/path_request )
   * and write it into the space 
   */
  char pn[1024];
  sprintf(&pn[0], "%s/%s", dir, reqpath);

  /* Check that the file exists and can be read sending an error reply otherwise */
  struct stat st;
  if (stat(pn, &st) < 0) {
    http_err(f, 404, "File not found or not accessible: %s", pn);
    return;
  }

  /* If the requested file is a directory, try finding index.html in that directory */
  if (S_ISDIR(st.st_mode)) {
    strcat(pn, "/index.html");
    if (stat(pn, &st) < 0) {
      http_err(f, 404, "File not found or not accessible: %s", pn);
      return;
    }
  }

  /* Check if the file is a regular file and whether the user (under which 
   * httpd is running) can execute this file 
   */
  if (S_ISREG(st.st_mode) && (st.st_mode & S_IXUSR)) {
    /* Requested file is executable. Fork and execute it in the child process */

    /* Flush all the buffers that the soon-to-be-parent(main process) wrote to f.
     * If we don't do it, the child will find data written to f that it did not
     * write itself. In our case, it is not too bad, but it is a good general practice
     * to flush before forking. 
     */
    fflush(f);
    
    /* Fork into a child and a parent process */
    int pid = fork();

    /* If fork did not succeed, send an error reply to the client */
    if (pid < 0) {
      http_err(f, 500, "Cannot fork: %s", strerror(errno));
      return;
    }
    
    /* pid is set to 0 in the child process */
    if (pid == 0) {
      /* Only the child process executes this */
      /* Connect the standard input (fd=0) to the null file. */
      int nullfd = open("/dev/null", O_RDONLY);
      dup2(nullfd, 0);

      /* Make standard output and standard error to point to f, so that when
       * our script writes to stdout/stderr it actually goes to the client 
       */
      dup2(fileno(f), 1);
      dup2(fileno(f), 2);

      /* The child needs neither the null file and f */
      close(nullfd);
      fclose(f);
      
      execl(pn, pn, (char *) 0);
      perror("execlll");
      exit(-1);
    }

    int status;
    waitpid(pid, &status, 0);
    fclose(f);
  } else {
    /* Non-executable: serve contents */
    int fd = open(pn, O_RDONLY);
    if (fd < 0) {
      http_err(f, 500, "Cannot open %s: %s", pn, strerror(errno));
      return;
    }

    fprintf(f, "HTTP/1.0 200 OK\r\n");
    fprintf(f, "Content-Type: text/html\r\n");
    fprintf(f, "\r\n");
    
    for (;;) {
      char readbuf[1024];
      int cc = read(fd, &readbuf[0], sizeof(readbuf));
      if (cc <= 0)
	break;
      
      fwrite(&readbuf[0], 1, cc, f);
    }

    close(fd);
    fclose(f);
  }
}


int
main(int argc, const char **argv)
{
  /* Check if we were called as we expect: a port number 
   * to listen on (8080) and the home directory from where 
   * we will be serving */
  if (argc != 3) {
    fprintf(stderr, "Usage: %s port dir\n", argv[0]);
    exit(-1);
  }

  /* Get the port number and the home directory string */
  int port = atoi(argv[1]);
  const char *dir = argv[2];
  
  if (!port) {
    fprintf(stderr, "Bad port number: %s\n", argv[1]);
    exit(-1);
  }

  /* Create a socket address strcture that specifies 
   * the parameters of the connections that we will be
   * listening for: 
   *   AF_INET - the address family we will be using is IP 
   *   INADDR_ANY - we want to accept connections from any address 
   *   port - the port number we will be listening on. 
   *          htons converts shorts in host format into short in 
   *          "network" format. 
   */
  struct sockaddr_in sin;
  sin.sin_family = AF_INET;
  sin.sin_addr.s_addr = INADDR_ANY;
  sin.sin_port = htons(port);
  
  /* Ask the OS to give us an IP socket (an abstraction for a network connection)
   * for two-way reliable connections (SOCK_STREAM). 0 selects the default protocol
   * that implements SOCK_STREAM, i.e. TCP
   */
  int srvfd = socket(AF_INET, SOCK_STREAM, 0);
  if (srvfd < 0) {
    perror("socket");
    exit(-1);
  }
  
  /* Make the socket reusable (when httpd exits, the OS maintains the socket for
   * some time and will not give another socket that asks for the same port.
   * This can be a problem if we want to restart httpd. It will fail because
   * the OS will not give it a socket). 
   * SOL_SOCKET lets setsockopt know that we want to change a per-socket option,
   * as opposed to a system wide option.
   */
  int on = 1;
  if (setsockopt(srvfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0) {
    perror("setsockopt SO_REUSEADDR");
    exit(-1);
  }
  
  /* Bind the socket that the OS gave us to the socket address structure 
   * we created above 
   */
  if (bind(srvfd, (struct sockaddr *) &sin, sizeof(sin)) < 0) {
    perror("bind");
    exit(-1);
  }

  /* Start listening for connections to our socket. As connections come in,
   * they are queued until we accept them and process. We say that we want
   * the OS to keep at most 5 connections in the queue. Connections that 
   * arrive when the queue is full will not be accepted and the client will
   * get a "connection refused" error. 
   */
  listen(srvfd, 5);

  /* Tell to the OS that we want to ignore the "nobody is
   * listening on the other end of the pipe" signal
   */
  signal(SIGPIPE, SIG_IGN);
  
  /* Enter an infinite loop of waiting and processing client requests */
  for (;;) {
    /* Allocate a socket address structure for the client's information*/
    struct sockaddr_in client_addr;
    unsigned int addrlen = sizeof(client_addr);

    /* Wait for a client connection request and accept it.
     * This function passes control to the OS and blocks. When it
     * returns, cfd is a file descriptor that represent our end of the
     * connection between us and the client. When we want to send data
     * to/from the client, we simply write/read into this descriptor */
    int cfd = accept(srvfd, (struct sockaddr *) &client_addr, &addrlen);
    if (cfd < 0) {
      perror("accept");
      continue;
    }
    
    /* Call the function that processes the client request */
    process_client(dir, cfd);

  }
}
